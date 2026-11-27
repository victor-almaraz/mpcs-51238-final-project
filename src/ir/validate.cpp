// Semantic IR checks (interpreter.md §12.3).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>
#include <espresso/metadata/pageSpec.h>
#include <espresso/metadata/primitiveSpec.h>
#include <espresso/metadata/text.h>

namespace ir {

namespace {

using metadata::bufferDecl;
using metadata::bufferRef;
using metadata::dimension;
using metadata::formatNumber;
using metadata::graphSpec;
using metadata::nodeSpec;
using metadata::pageKindSpec;
using metadata::pageNode;
using metadata::paramSpec;
using metadata::pipeDecl;
using metadata::pipeKind;
using metadata::pipeRef;
using metadata::portSpec;

// What is wrong with @p v as the value of a number, choice, text, or list
// parameter @p spec, if anything is: the message after the parameter's name.
std::optional<std::string> valueProblem(const paramSpec& spec, const metadata::paramValue& v) {
    switch (spec.dim) {
    case dimension::choice: {
        const auto s = metadata::as<std::string>(v);
        if (!s || std::ranges::find(spec.choices, s->get()) == spec.choices.end()) {
            return "must be one of: " + metadata::joinNames(spec.choices);
        }
        return std::nullopt;
    }
    case dimension::text:
        if (!std::holds_alternative<std::string>(v)) return "must be text";
        return std::nullopt;
    case dimension::textList:
        if (!std::holds_alternative<std::vector<std::string>>(v)) return "must be a list of strings";
        return std::nullopt;
    case dimension::list: {
        const auto l = metadata::as<std::vector<double>>(v);
        if (!l) return "must be a list of numbers";
        if (!std::ranges::all_of(l->get(), [](double x) { return std::isfinite(x); })) {
            return "must contain finite numbers";
        }
        return std::nullopt;
    }
    default: {
        const auto d = metadata::as<double>(v);
        if (!d) return "must be a number (" + std::string(metadata::toString(spec.dim)) + ")";
        if (!(d->get() >= spec.min) || !(d->get() <= spec.max)) {
            return "is " + formatNumber(*d) + "; expected " + formatNumber(spec.min) + " to " +
                   formatNumber(spec.max);
        }
        return std::nullopt;
    }
    }
}

// The text parameter @p name of node @p n, or its default in @p spec.
std::string textOf(const nodeSpec& n, const metadata::primitiveSpec& spec, std::string_view name) {
    if (const auto it = n.params.find(name); it != n.params.end()) {
        if (const auto t = metadata::as<std::string>(it->second)) return t->get();
    }
    const auto p = spec.param(name);
    return p ? std::string(p->get().defaultText) : std::string();
}

// The number parameter @p name of @p params, or its default in @p spec.
double numberOf(const std::map<std::string, metadata::paramValue, std::less<>>& params,
                const paramSpec& spec) {
    if (const auto it = params.find(spec.name); it != params.end()) {
        if (const auto d = metadata::as<double>(it->second)) return d->get();
    }
    return spec.defaultValue;
}

class checker {
public:
    explicit checker(const graphSpec& g) : graph_(g) {}

    std::vector<error> run() {
        checkPipes();
        checkBuffers();
        std::set<std::string_view> ids;
        for (const auto& n : graph_.nodes) {
            if (!ids.insert(n.id).second) fail(n, "duplicate node ID");
            checkNode(n);
        }
        checkFrames();
        checkReservedAddresses();
        checkLayout();
        if (errors_.empty()) checkOrder();
        return std::move(errors_);
    }

private:
    void fail(const nodeSpec& n, std::string message) {
        errors_.push_back({.node = n.id, .message = std::move(message), .src = n.src});
    }
    void fail(std::string message) {
        errors_.push_back({.node = {}, .message = std::move(message), .src = std::nullopt});
    }

    void checkPipes() {
        if (graph_.meter < 1) fail("meter must be at least 1");
        if (graph_.pagePort && !metadata::isPagePort(*graph_.pagePort)) {
            fail("page port must be " + std::to_string(metadata::minPagePort) + " to " +
                 std::to_string(metadata::maxPagePort));
        }
        if (graph_.tempo && (!(*graph_.tempo > 0.0) || !std::isfinite(*graph_.tempo)))
            fail("tempo must be a positive number");
        for (const pipeDecl& p : graph_.pipes) {
            const std::string name = metadata::toString(p.pipe);
            if (!pipes_.emplace(p.pipe, std::cref(p)).second) fail(name + " is declared twice");
            if (p.pipe.index < 0) fail(name + ": negative index");
            if (p.channels < 1 || p.channels > metadata::maxChannels) {
                fail(name + ": channels must be 1 to " + std::to_string(metadata::maxChannels));
            }
            if (p.pipe.kind == pipeKind::event && p.channels != 1) {
                fail(name + ": " + std::string(metadata::toString(p.pipe.kind)) + " pipes have one channel");
            }
            if (p.feedback && p.pipe.kind != pipeKind::audio && p.pipe.kind != pipeKind::control) {
                fail(name + ": only audio and control pipes can be feedback pipes");
            }
            if (p.pipe == pipeRef{.kind = pipeKind::control, .index = metadata::tempoPipe} &&
                (p.channels != 1 || p.feedback)) {
                fail(name + " is the tempo pipe: one channel, not feedback");
            }
        }
    }

    // Buffer declarations (samples.md §5.1, §12.2). Errors point at the
    // buffer form that declares the buffer.
    void checkBuffers() {
        std::set<std::string_view> ids;
        for (const bufferDecl& b : graph_.buffers) {
            const std::string prefix = metadata::toString(b.buffer) + ": ";
            const auto failBuffer = [&](std::string_view message) {
                errors_.push_back({.node = {}, .message = prefix + std::string(message), .src = b.src});
            };
            if (!buffers_.insert(b.buffer).second) failBuffer("declared twice");
            if (b.buffer.index < 0) failBuffer("negative index");
            if (b.id.empty()) failBuffer("has no ID");
            if (!b.id.empty() && !ids.insert(b.id).second) failBuffer("duplicate buffer ID " + b.id);
            if (b.channels < 1 || b.channels > metadata::maxChannels) {
                failBuffer("channels must be 1 to " + std::to_string(metadata::maxChannels));
            }
            if (!b.seconds && !b.file) failBuffer("needs a length in seconds, a file, or both");
            if (b.seconds) {
                // NaN is not positive.
                const bool positive = *b.seconds > 0.0 && std::isfinite(*b.seconds);
                if (!positive) failBuffer("seconds must be a positive number");
            }
            if (b.file && b.file->empty()) failBuffer("the file name is empty");
            if (!b.resample && !b.file) failBuffer("resample applies only to a file");
        }
    }

    bool declared(pipeRef r) const {
        return pipes_.contains(r) || r == pipeRef{.kind = pipeKind::control, .index = metadata::tempoPipe};
    }

    void checkRef(const nodeSpec& n, std::string_view what, pipeRef r, metadata::kindSet kinds) {
        if (!metadata::contains(kinds, r.kind)) {
            fail(n, std::string(what) + ": " + metadata::toString(r) + " has the wrong type");
        } else if (!declared(r)) {
            fail(n, std::string(what) + ": " + metadata::toString(r) + " is not declared");
        }
    }

    void checkParam(const nodeSpec& n, const paramSpec& spec, const metadata::paramValue& v) {
        const std::string what = "parameter " + std::string(spec.name);
        if (spec.dim == dimension::buffer) {
            const auto b = metadata::as<bufferRef>(v);
            if (!b) {
                fail(n, what + " must name a buffer");
            } else if (!buffers_.contains(*b)) {
                fail(n, what + ": " + metadata::toString(*b) + " is not declared");
            }
            return;
        }
        if (std::holds_alternative<bufferRef>(v)) {
            fail(n, what + " does not take a buffer");
            return;
        }
        if (const auto r = metadata::as<pipeRef>(v)) {
            if (spec.bindable == metadata::staticOnly) {
                fail(n, what + " cannot be bound to a pipe");
            } else {
                checkRef(n, what, *r, spec.bindable);
            }
            return;
        }
        if (auto problem = valueProblem(spec, v)) fail(n, what + " " + *problem);
    }

    void checkPorts(const nodeSpec& n, const metadata::primitiveSpec& spec, bool input) {
        const std::string direction = input ? "input " : "output ";
        const auto& bound = input ? n.inputs : n.outputs;
        for (const auto& [name, refs] : bound) {
            const auto port = input ? spec.input(name) : spec.output(name);
            const std::string what = direction + name;
            if (!port) {
                fail(n, "unknown " + what);
                continue;
            }
            const portSpec& p = *port;
            if (refs.empty()) fail(n, what + " has no pipes");
            if (refs.size() > 1 && !p.multiple) fail(n, what + " takes one pipe");
            for (auto r : refs) checkRef(n, what, r, p.kinds);
        }
        for (const auto& p : input ? spec.inputs : spec.outputs) {
            if (p.required && !bound.contains(p.name)) fail(n, "missing " + direction + std::string(p.name));
        }
    }

    void checkNode(const nodeSpec& n) {
        const auto found = metadata::findPrimitive(n.type);
        if (!found) {
            fail(n, "unknown primitive type " + n.type);
            return;
        }
        const metadata::primitiveSpec& spec = *found;
        for (const auto& [name, value] : n.params) {
            if (const auto p = spec.param(name)) {
                checkParam(n, *p, value);
            } else {
                fail(n, "unknown parameter " + name + " for " + n.type);
            }
        }
        checkPorts(n, spec, true);
        checkPorts(n, spec, false);
        for (auto& message : metadata::checkConstraints(spec, n, graph_, metadata::naming::ir))
            fail(n, std::move(message));
    }

    // Frame pipes (samples.md §10.4): an event pipe that a frame port
    // connects carries only frames, from one frame output, and every frame
    // input reads one that a frame output writes.
    void checkFrames() {
        struct use {
            std::vector<std::size_t> writers;  // frame outputs
            std::vector<std::size_t> readers;  // frame inputs
            std::vector<std::size_t> others;   // any other event port
        };
        std::map<pipeRef, use> uses;
        for (std::size_t i = 0; i < graph_.nodes.size(); ++i) {
            const nodeSpec& n = graph_.nodes[i];
            const auto spec = metadata::findPrimitive(n.type);
            if (!spec) continue;
            for (const bool input : {true, false}) {
                for (const auto& [name, refs] : input ? n.inputs : n.outputs) {
                    const auto port = input ? spec->get().input(name) : spec->get().output(name);
                    if (!port) continue;
                    for (const pipeRef r : refs) {
                        if (r.kind != pipeKind::event) continue;
                        auto& u = uses[r];
                        (port->get().frames ? (input ? u.readers : u.writers) : u.others).push_back(i);
                    }
                }
            }
        }
        for (const auto& [ref, u] : uses) {
            if (u.writers.empty() && u.readers.empty()) continue;
            const std::string name = metadata::toString(ref);
            if (!u.others.empty()) {
                fail(graph_.nodes[u.others.front()],
                     name + " carries spectral frames; only frame ports may connect to it");
            }
            if (u.writers.size() > 1) {
                fail(graph_.nodes[u.writers[1]],
                     name + " already has a frame writer, node " + graph_.nodes[u.writers[0]].id);
            }
            if (u.writers.empty()) {
                fail(graph_.nodes[u.readers.front()],
                     "reads frames from " + name + ", which no frame output writes");
            }
        }
    }

    // Every reader of a non-feedback pipe follows all of its writers
    // (AGENTS.md: the compiler sorts the graph; the engine runs nodes in
    // IR order). A node that reads a pipe it writes is a cycle.
    void checkOrder() {
        const auto writers = metadata::writersByPipe(graph_, true);
        for (std::size_t i = 0; i < graph_.nodes.size(); ++i) {
            const nodeSpec& n = graph_.nodes[i];
            for (auto r : metadata::pipesRead(n)) {
                const auto it = writers.find(r);
                if (it == writers.end()) continue;
                const std::size_t last = it->second.back();
                if (last < i) continue;
                if (last == i) {
                    fail(n, "reads " + metadata::toString(r) + ", which it writes; close the loop through a feedback pipe");
                } else {
                    fail(n, "reads " + metadata::toString(r) + " before node " + graph_.nodes[last].id +
                                " writes it; order nodes by data flow, or use a feedback pipe for a cycle");
                }
            }
        }
    }

    // The page's layout (dsl-ui.md §7.1): each element against its kind's
    // table and its parent's, and each control's address against the nodes
    // on the page port: a control sends to an address an oscIn reads (one
    // without an address reads every one), a display shows one an oscOut
    // sends.
    // No oscIn or oscOut on the page port uses the address the server
    // reloads pages on, which would reload them at every message.
    void checkReservedAddresses() {
        if (!graph_.pagePort) return;
        for (const nodeSpec& n : graph_.nodes) {
            if (n.type != "oscIn" && n.type != "oscOut") continue;
            const auto found = metadata::findPrimitive(n.type);
            if (!found) continue;
            const metadata::primitiveSpec& spec = *found;
            const auto port = spec.param("port");
            if (!port || numberOf(n.params, *port) != *graph_.pagePort) continue;
            if (textOf(n, spec, "address") == metadata::pageReloadAddress) {
                fail(n, std::string("address ") + std::string(metadata::pageReloadAddress) +
                            " on the page port is the server's, for reloading pages; choose another");
            }
        }
    }

    void checkLayout() {
        if (!graph_.layout) return;
        const pageNode& root = *graph_.layout;
        if (!graph_.pagePort) {
            failPage(root, "page", "a layout needs a page port");
            return;
        }
        for (const nodeSpec& n : graph_.nodes) {
            const bool reads = n.type == "oscIn";
            if (!reads && n.type != "oscOut") continue;
            const auto found = metadata::findPrimitive(n.type);
            if (!found) continue;
            const metadata::primitiveSpec& spec = *found;
            const auto port = spec.param("port");
            if (!port || numberOf(n.params, *port) != *graph_.pagePort) continue;
            (reads ? pageReads_ : pageWrites_).insert(textOf(n, spec, "address"));
        }
        if (root.kind != metadata::pageRoot) {
            failPage(root, "page", "the layout's root must be a page, not " + root.kind);
            return;
        }
        checkPageNode(root, "page", std::nullopt);
    }

    void failPage(const pageNode& p, const std::string& where, const std::string& message) {
        errors_.push_back({.node = {}, .message = "layout: " + where + ": " + message, .src = p.src});
    }

    void checkPageNode(const pageNode& p, const std::string& where,
                       metadata::optionalRef<pageKindSpec> parent) {
        const auto found = metadata::findPageKind(p.kind);
        if (!found) {
            failPage(p, where, "unknown kind " + p.kind);
            return;
        }
        const pageKindSpec& spec = *found;
        if (parent && std::ranges::find(parent->get().children, p.kind) == parent->get().children.end()) {
            failPage(p, where,
                     "a " + std::string(parent->get().kind) + " holds " +
                         metadata::joinNames(parent->get().children) + ", not a " + p.kind);
        }
        for (const auto& [name, value] : p.params) {
            const auto param = spec.param(name);
            const bool placement = std::ranges::find(metadata::placementParams(), name, &paramSpec::name) !=
                                   metadata::placementParams().end();
            if (!param || (placement && !parent)) {
                failPage(p, where, "unknown parameter " + name + " for " + p.kind);
                continue;
            }
            const std::string what = "parameter " + name;
            if (std::holds_alternative<pipeRef>(value) || std::holds_alternative<bufferRef>(value)) {
                failPage(p, where, what + " takes no pipe or buffer");
            } else if (auto problem = valueProblem(*param, value)) {
                failPage(p, where, what + " " + *problem);
            } else if (const auto cells = metadata::as<std::vector<double>>(value); placement && cells) {
                const bool whole =
                    std::ranges::all_of(cells->get(), [](double x) { return x >= 1 && x == std::floor(x); });
                if (cells->get().size() != 2 || !whole)
                    failPage(p, where, what + " must be two whole numbers from 1");
            }
        }
        if (const auto accent = p.params.find("accent"); accent != p.params.end()) {
            const auto color = metadata::as<std::string>(accent->second);
            if (color && !metadata::isPageColor(color->get())) {
                failPage(p, where,
                         "accent " + color->get() + " must be a color: #4fd1c5, or a name such as teal");
            }
        }
        for (const paramSpec& param : spec.params) {
            if (param.required && !p.params.contains(param.name)) {
                failPage(p, where, "missing parameter " + std::string(param.name));
            }
        }
        if (spec.role == metadata::pageRole::group) {
            if (!p.address.empty()) failPage(p, where, "a group has no address");
            for (const pageNode& c : p.children) {
                checkPageNode(c, where + " > " + c.kind + (c.label.empty() ? "" : " \"" + c.label + "\""),
                              spec);
            }
            return;
        }
        if (!p.children.empty()) failPage(p, where, "a " + p.kind + " holds no elements");
        checkControl(p, where, spec);
    }

    // A control's or a display's address and values.
    void checkControl(const pageNode& p, const std::string& where, const pageKindSpec& spec) {
        const std::string address = p.address.empty() ? std::string(spec.defaultAddress) : p.address;
        if (address.empty()) {
            failPage(p, where, "needs an address");
        } else if (!address.starts_with('/')) {
            failPage(p, where, "address " + address + " must start with /");
        } else if (spec.role == metadata::pageRole::sends && !pageReads_.contains(address) &&
                   !pageReads_.contains("")) {
            failPage(p, where, "sends to " + address + ", which no oscIn on the page port reads");
        } else if (spec.role == metadata::pageRole::shows && !pageWrites_.contains(address)) {
            failPage(p, where, "shows " + address + ", which no oscOut on the page port sends");
        }
        // The starting value lies in the range; a list's every value does.
        const auto lo = spec.param("min");
        const auto hi = spec.param("max");
        // Options on a control other than a select are a comma list on the
        // page: an option may hold no comma and not be empty.
        if (const auto it = p.params.find("options"); it != p.params.end() && p.kind != "select") {
            if (const auto names = metadata::as<std::vector<std::string>>(it->second)) {
                for (const auto& name : names->get()) {
                    if (name.empty() || name.find(',') != std::string::npos)
                        failPage(p, where, "option \"" + name + "\" must be a name with no comma");
                }
            }
        }
        if (lo && hi) {
            double min = numberOf(p.params, *lo);
            double max = numberOf(p.params, *hi);
            // A knob of steps or angles runs over its positions, as the page
            // draws it.
            if (p.kind == "knob") {
                const auto count = [&](std::string_view name) -> std::size_t {
                    const auto it = p.params.find(std::string(name));
                    if (it == p.params.end()) return 0;
                    if (const auto l = metadata::as<std::vector<double>>(it->second)) return l->get().size();
                    if (const auto d = metadata::as<double>(it->second)) return static_cast<std::size_t>(*d);
                    return 0;
                };
                const std::size_t positions = count("angles") >= 2 ? count("angles") : count("steps");
                if (positions >= 2) {
                    min = 0;
                    max = static_cast<double>(positions - 1);
                }
            }
            const bool ordered = min < max;  // false for NaN, too
            if (!ordered)
                failPage(p, where, "min " + formatNumber(min) + " must be below max " + formatNumber(max));
            if (const auto it = p.params.find("initial"); it != p.params.end() && ordered) {
                std::vector<double> values;
                if (const auto d = metadata::as<double>(it->second)) values.push_back(*d);
                if (const auto l = metadata::as<std::vector<double>>(it->second)) values = *l;
                for (const double v : values) {
                    const bool inside = v >= min && v <= max;  // false for NaN, too
                    if (!inside) {
                        failPage(p, where,
                                 "initial " + formatNumber(v) + " is outside " + formatNumber(min) + " to " +
                                     formatNumber(max));
                    }
                }
                if (p.kind == "xy" && values.size() != 2)
                    failPage(p, where, "initial must be two numbers, x and y");
            }
        }
        if (p.kind == "select") {
            const auto options = p.params.find("options");
            const auto initial = spec.param("initial");
            const auto count = options == p.params.end()
                                   ? std::nullopt
                                   : metadata::as<std::vector<std::string>>(options->second);
            const double chosen = initial ? numberOf(p.params, *initial) : 0;
            if (count &&
                (chosen != std::floor(chosen) || chosen >= static_cast<double>(count->get().size()))) {
                failPage(p, where, "initial " + formatNumber(chosen) + " is not the index of an option");
            }
        }
        const auto lowSpec = spec.param("low");
        const auto highSpec = spec.param("high");
        if (lowSpec && highSpec) {
            const double low = numberOf(p.params, *lowSpec);
            const double high = numberOf(p.params, *highSpec);
            if (low > high)
                failPage(p, where, "low " + formatNumber(low) + " is above high " + formatNumber(high));
        }
    }

    const graphSpec& graph_;
    std::map<pipeRef, std::reference_wrapper<const pipeDecl>> pipes_;
    std::set<bufferRef> buffers_;
    std::set<std::string> pageReads_;   // addresses oscIns on the page port read; "" for every one
    std::set<std::string> pageWrites_;  // addresses oscOuts on the page port send
    std::vector<error> errors_;
};

} // namespace

std::vector<error> validate(const graphSpec& graph) { return checker(graph).run(); }

} // namespace ir
