// Likely audio hazards in a valid, sorted graph: loops that grow, very
// large gains, and frequencies above half the sample rate.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>
#include <espresso/metadata/primitiveSpec.h>
#include <espresso/metadata/text.h>

namespace ir {

using metadata::dimension;
using metadata::formatNumber;
using metadata::nodeSpec;
using metadata::pipeKind;
using metadata::pipeRef;

namespace {

// Gains above +24 dB are almost always mistakes for audio.
constexpr double loudGain = 15.85;

// An upper bound on the gain a node applies to one audio pipe it reads,
// and whether the bound is known. Unknown gains count as 1: bound gains,
// and nodes whose gain depends on their settings or input, such as
// resonant filters.
struct gainEstimate {
    double gain = 1.0;
    bool known = true;
};

// One audio read of a node: the pipe, at one port position or parameter,
// and the gain from it to the node's output.
struct audioRead {
    pipeRef pipe;
    gainEstimate gain;
};

// Nodes that pass every audio input at unity or less.
constexpr auto unity = std::to_array<std::string_view>(
    {"add", "subtract", "min", "max", "neg", "abs", "delayLine", "pan", "crossfade"});

bool isUnity(std::string_view type) { return std::ranges::find(unity, type) != unity.end(); }

gainEstimate staticParam(const nodeSpec& n, std::string_view name) {
    const auto it = n.params.find(name);
    if (it == n.params.end()) return {.gain = 1.0, .known = true};
    if (const auto d = metadata::as<double>(it->second)) return {.gain = std::abs(d->get()), .known = true};
    return {.gain = 1.0, .known = false};  // bound to a pipe
}

// The gain from position @p k of input @p port: a static `gain` amount, a
// static `adsr` gain (the envelope stays within 0 to 1), the `mix` level
// at that position, and unity for the nodes above.
gainEstimate inputGain(const nodeSpec& n, std::string_view port, std::size_t k) {
    if (n.type == "gain") return staticParam(n, "amount");
    if (n.type == "adsr" && port == "in") return staticParam(n, "gain");
    if (n.type == "mix") {
        const auto it = n.params.find("levels");
        const auto levels = it == n.params.end() ? std::nullopt : metadata::as<std::vector<double>>(it->second);
        return {.gain = levels && k < levels->get().size() ? std::abs(levels->get()[k]) : 1.0, .known = true};
    }
    return {.gain = 1.0, .known = isUnity(n.type)};
}

// The gain from a parameter bound to an audio pipe: `b` of subtract, min,
// and max passes at unity; anything else (modulation) is unknown.
gainEstimate paramGain(const nodeSpec& n, std::string_view name) {
    const bool passes = name == "b" && (n.type == "subtract" || n.type == "min" || n.type == "max");
    return {.gain = 1.0, .known = passes};
}

std::vector<audioRead> audioReads(const nodeSpec& n) {
    std::vector<audioRead> out;
    for (const auto& [port, refs] : n.inputs) {
        for (std::size_t k = 0; k < refs.size(); ++k) {
            if (refs[k].kind == pipeKind::audio)
                out.push_back({.pipe = refs[k], .gain = inputGain(n, port, k)});
        }
    }
    for (const auto& [name, value] : n.params) {
        const auto r = metadata::as<pipeRef>(value);
        if (r && r->get().kind == pipeKind::audio) out.push_back({.pipe = *r, .gain = paramGain(n, name)});
    }
    return out;
}

// Gains from one feedback pipe to every node and pipe of a graph.
class reachFrom {
public:
    reachFrom(pipeRef from, const std::vector<std::vector<audioRead>>& reads,
              const std::map<pipeRef, std::vector<std::size_t>>& writers, std::size_t passes)
        : from_(from), writers_(writers), reach_(reads.size(), 0.0), known_(reads.size(), true) {
        // Writers of non-feedback pipes precede their readers, so one pass
        // in node order follows every path that crosses no other feedback
        // pipe; each further pass crosses one more.
        for (std::size_t pass = 0; pass < passes; ++pass) {
            bool changed = false;
            for (std::size_t i = 0; i < reads.size(); ++i) {
                double gain = 0.0;
                bool known = true;
                for (const auto& r : reads[i]) {
                    const auto [g, k] = at(r.pipe);
                    if (g == 0.0) continue;
                    gain += g * r.gain.gain;
                    known = known && k && r.gain.known;
                }
                changed = changed || gain != reach_[i] || known != known_[i];
                reach_[i] = gain;
                known_[i] = known;
            }
            if (!changed) break;
        }
    }

    // The gain from the feedback pipe to @p pipe, and whether it is known.
    // Writers to an audio pipe sum, so parallel paths add.
    gainEstimate at(pipeRef pipe) const {
        if (pipe == from_) return {.gain = 1.0, .known = true};
        gainEstimate total{.gain = 0.0, .known = true};
        if (const auto w = writers_.find(pipe); w != writers_.end()) {
            for (const std::size_t j : w->second) {
                if (reach_[j] == 0.0) continue;
                total.gain += reach_[j];
                total.known = total.known && known_[j];
            }
        }
        return total;
    }

    // The gain once around the loop: from the pipe back to its writers.
    gainEstimate loop() const {
        gainEstimate total{.gain = 0.0, .known = true};
        for (const std::size_t j : writers_.at(from_)) {
            if (reach_[j] == 0.0) continue;
            total.gain += reach_[j];
            total.known = total.known && known_[j];
        }
        return total;
    }

private:
    pipeRef from_;
    const std::map<pipeRef, std::vector<std::size_t>>& writers_;
    std::vector<double> reach_;  // gain to each node's output
    std::vector<bool> known_;    // no unknown gain along the way
};

// "audio pipe 0", or "audio pipes 0, 1, and 2".
std::string pipeList(const std::vector<pipeRef>& pipes) {
    if (pipes.size() == 1) return metadata::toString(pipes.front());
    std::string out = std::string(metadata::toString(pipes.front().kind)) + " pipes ";
    for (std::size_t i = 0; i < pipes.size(); ++i) {
        if (i > 0) out += pipes.size() == 2 ? " and " : i + 1 == pipes.size() ? ", and " : ", ";
        out += std::to_string(pipes[i].index);
    }
    return out;
}

std::string decibels(double gain) { return formatNumber(std::round(20.0 * std::log10(std::abs(gain)))); }

} // namespace

std::vector<error> hazards(const metadata::graphSpec& graph, double sampleRate) {
    std::vector<error> out;
    const auto& nodes = graph.nodes;
    auto warn = [&](std::size_t i, std::string message) {
        out.push_back({.node = nodes[i].id, .message = std::move(message), .src = nodes[i].src});
    };

    // Loops through audio feedback pipes (primitives.md §5.6): an upper
    // bound on the gain once around, along every path back to the pipe,
    // including paths through other feedback pipes and through parameters.
    const auto writers = metadata::writersByPipe(graph, false);
    std::vector<std::vector<audioRead>> reads;
    reads.reserve(nodes.size());
    for (const auto& n : nodes) reads.push_back(audioReads(n));
    std::vector<pipeRef> loops;
    for (const auto& d : graph.pipes) {
        if (d.feedback && d.pipe.kind == pipeKind::audio && writers.contains(d.pipe)) loops.push_back(d.pipe);
    }
    std::vector<reachFrom> from;
    from.reserve(loops.size());
    for (const auto pipe : loops) from.emplace_back(pipe, reads, writers, loops.size() + 1);

    // Feedback pipes that reach each other are one loop: warn once, for the
    // member with the largest gain.
    std::set<std::size_t> reported;
    for (std::size_t a = 0; a < loops.size(); ++a) {
        if (reported.contains(a)) continue;
        std::vector<pipeRef> members;
        std::size_t worst = a;
        for (std::size_t b = a; b < loops.size(); ++b) {
            if (b != a && (from[a].at(loops[b]).gain == 0.0 || from[b].at(loops[a]).gain == 0.0)) continue;
            reported.insert(b);
            members.push_back(loops[b]);
            if (from[b].loop().gain > from[worst].loop().gain) worst = b;
        }
        const auto [gain, known] = from[worst].loop();
        const std::size_t at = writers.at(loops[worst]).back();
        const std::string loop = "the loop through feedback " + pipeList(members);
        if (known && gain >= 1.0) {
            warn(at, loop + " has a gain of " + formatNumber(gain) +
                         ", so it grows without bound; keep the loop's gain below 1");
        } else if (!known && gain > 1.0) {
            warn(at, loop + " may have a gain of " + formatNumber(gain) +
                         " (counting nodes of unknown gain as 1), so it may grow without bound");
        }
    }

    const double nyquist = sampleRate / 2.0;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        const auto spec = metadata::findPrimitive(node.type);
        const bool audioOut = std::ranges::any_of(node.outputs, [](const auto& o) {
            return std::ranges::any_of(o.second, [](pipeRef r) { return r.kind == pipeKind::audio; });
        });
        for (const auto& [name, v] : node.params) {
            const auto found = spec ? spec->get().param(name) : std::nullopt;
            if (!found) continue;
            const metadata::paramSpec& p = *found;
            if (const auto value = metadata::as<double>(v)) {
                const double d = *value;
                if (p.dim == dimension::frequency && d > nyquist) {
                    warn(i, "parameter " + name + " is " + formatNumber(d) + " Hz, above half the " +
                                formatNumber(sampleRate) + " Hz sample rate; it aliases or is clamped");
                }
                if (audioOut && p.dim == dimension::level && std::abs(d) > loudGain && (node.type == "gain" || node.type == "adsr")) {
                    warn(i, "a gain of " + formatNumber(d) + " is " + decibels(d) +
                                " dB; the output limiter will hold it at its ceiling");
                }
            } else if (const auto l = metadata::as<std::vector<double>>(v); l && node.type == "mix" && audioOut) {
                const auto& levels = l->get();
                const auto loud =
                    std::ranges::find_if(levels, [](double x) { return std::abs(x) > loudGain; });
                if (loud != levels.end())
                    warn(i, "a mix level of " + formatNumber(*loud) + " is " + decibels(*loud) + " dB");
            }
        }
    }
    return out;
}

} // namespace ir
