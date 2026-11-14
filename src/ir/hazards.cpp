// Likely audio hazards in a valid, sorted graph: loops that grow, very
// large gains, and frequencies above half the sample rate.

#include <proj/ir/ir.h>
#include <proj/metadata/primitiveSpec.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

namespace ir {

using metadata::dimension;
using metadata::pipeKind;
using metadata::pipeRef;

namespace {

// Gains above +24 dB are almost always mistakes for audio.
constexpr double loudGain = 15.85;

// The gain a node applies to the audio it reads from @p in, and whether
// it is known. Known: a static `gain` amount, a static `adsr` gain (the
// envelope stays within 0 to 1), static `mix` levels, and nodes that pass
// audio at unity or less (add, delays, pans, crossfades). Unknown, and
// counted as 1: bound gains and nodes whose gain depends on their
// settings or input, such as resonant filters.
struct gainEstimate {
    double gain = 1.0;
    bool known = true;
};

gainEstimate staticGain(const metadata::nodeSpec& n, pipeRef in) {
    auto number = [&](std::string_view name) -> gainEstimate {
        const auto it = n.params.find(name);
        if (it == n.params.end()) return {1.0, true};
        if (const auto d = metadata::as<double>(it->second)) return {std::abs(d->get()), true};
        return {1.0, false};  // bound to a pipe
    };
    if (n.type == "gain") return number("amount");
    if (n.type == "adsr") return number("gain");
    if (n.type == "mix") {
        const auto it = n.params.find("levels");
        const auto levels = it == n.params.end() ? std::nullopt : metadata::as<std::vector<double>>(it->second);
        const auto& ins = n.inputs.at("in");
        double sum = 0.0;  // the same pipe on several inputs adds up
        for (std::size_t k = 0; k < ins.size(); ++k) {
            if (ins[k] == in) sum += levels && k < levels->get().size() ? std::abs(levels->get()[k]) : 1.0;
        }
        return {sum, true};
    }
    constexpr std::string_view unity[] = {"add", "delayLine", "pan", "crossfade", "audioOut"};
    if (std::find(std::begin(unity), std::end(unity), n.type) != std::end(unity)) return {1.0, true};
    return {1.0, false};
}

std::string number(double v) {
    std::ostringstream out;
    out << v;
    return out.str();
}

} // namespace

std::vector<error> hazards(const metadata::graphSpec& graph, double sampleRate) {
    std::vector<error> out;
    const auto& nodes = graph.nodes;
    const std::size_t n = nodes.size();
    auto warn = [&](std::size_t i, std::string message) { out.push_back({nodes[i].id, std::move(message), nodes[i].src}); };

    std::map<pipeRef, std::vector<std::size_t>> writers;
    for (std::size_t i = 0; i < n; ++i) {
        for (auto r : metadata::pipesWritten(nodes[i])) writers[r].push_back(i);
    }

    // Loops through audio feedback pipes (§5.6): an upper bound on the gain
    // from the pipe's readers back to it. Parallel paths add, because
    // writers to an audio pipe sum.
    for (const auto& d : graph.pipes) {
        if (!d.feedback || d.pipe.kind != pipeKind::audio) continue;
        std::vector<double> reach(n, 0.0);    // gain from the feedback pipe to each node's output
        std::vector<bool> known(n, true);     // no unknown gain along the way
        for (std::size_t i = 0; i < n; ++i) {
            for (const auto& [port, refs] : nodes[i].inputs) {
                for (auto r : refs) {
                    if (r.kind != pipeKind::audio) continue;
                    double from = 0.0;
                    bool exact = true;
                    if (r == d.pipe) {
                        from = 1.0;
                    } else if (const auto w = writers.find(r); w != writers.end()) {
                        for (std::size_t j : w->second) {
                            if (j < i && reach[j] > 0.0) {  // the sort puts writers first
                                from += reach[j];
                                exact = exact && known[j];
                            }
                        }
                    }
                    if (from == 0.0) continue;
                    const auto g = staticGain(nodes[i], r);
                    reach[i] += from * g.gain;
                    known[i] = known[i] && exact && g.known;
                }
            }
        }
        double loop = 0.0;
        bool exact = true;
        for (std::size_t w : writers[d.pipe]) {
            loop += reach[w];
            if (reach[w] > 0.0) exact = exact && known[w];
        }
        const auto& at = writers[d.pipe];
        if (at.empty()) continue;
        if (exact && loop >= 1.0) {
            warn(at.back(), "the loop through feedback " + metadata::toString(d.pipe) + " has a gain of " + number(loop) +
                                ", so it grows without bound; keep the loop's gain below 1");
        } else if (!exact && loop > 1.0) {
            warn(at.back(), "the loop through feedback " + metadata::toString(d.pipe) + " may have a gain of " + number(loop) +
                                " (counting nodes of unknown gain as 1), so it may grow without bound");
        }
    }

    const double nyquist = sampleRate / 2.0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& node = nodes[i];
        const auto spec = metadata::findPrimitive(node.type);
        const bool audioOut = std::any_of(node.outputs.begin(), node.outputs.end(), [](const auto& o) {
            return std::any_of(o.second.begin(), o.second.end(), [](pipeRef r) { return r.kind == pipeKind::audio; });
        });
        for (const auto& [name, v] : node.params) {
            const auto found = spec ? spec->get().param(name) : std::nullopt;
            if (!found) continue;
            const metadata::paramSpec& p = *found;
            if (const auto value = metadata::as<double>(v)) {
                const double d = *value;
                if (p.dim == dimension::frequency && d > nyquist) {
                    warn(i, "parameter " + name + " is " + number(d) + " Hz, above half the " + number(sampleRate) +
                                " Hz sample rate; it aliases or is clamped");
                }
                if (audioOut && p.dim == dimension::level && std::abs(d) > loudGain && (node.type == "gain" || node.type == "adsr")) {
                    warn(i, "a gain of " + number(d) + " is " + number(std::round(20.0 * std::log10(std::abs(d)))) +
                                " dB; the output limiter will hold it at its ceiling");
                }
            } else if (const auto l = metadata::as<std::vector<double>>(v); l && node.type == "mix" && audioOut) {
                for (double x : l->get()) {
                    if (std::abs(x) > loudGain) {
                        warn(i, "a mix level of " + number(x) + " is " + number(std::round(20.0 * std::log10(std::abs(x)))) + " dB");
                        break;
                    }
                }
            }
        }
    }
    return out;
}

} // namespace ir
