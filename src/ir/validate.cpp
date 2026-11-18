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
using metadata::paramSpec;
using metadata::pipeDecl;
using metadata::pipeKind;
using metadata::pipeRef;
using metadata::portSpec;

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
        if (graph_.tempo && (!(*graph_.tempo > 0.0) || !std::isfinite(*graph_.tempo)))
            fail("tempo must be a positive number");
        for (const pipeDecl& p : graph_.pipes) {
            const std::string name = metadata::toString(p.pipe);
            if (!pipes_.emplace(p.pipe, std::cref(p)).second) fail(name + " is declared twice");
            if (p.pipe.index < 0) fail(name + ": negative index");
            if (p.channels < 1 || p.channels > metadata::maxChannels) {
                fail(name + ": channels must be 1 to " + std::to_string(metadata::maxChannels));
            }
            if ((p.pipe.kind == pipeKind::event || p.pipe.kind == pipeKind::video) && p.channels != 1) {
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
                if (spec.dim == dimension::color) {
                    const auto decl = pipes_.find(*r);
                    if (decl != pipes_.end() && decl->second.get().channels < metadata::colorChannels) {
                        fail(n, what + " must follow a pipe of " + std::to_string(metadata::colorChannels) +
                                    " channels (R, G, B, A)");
                    }
                }
            }
            return;
        }
        switch (spec.dim) {
        case dimension::choice: {
            const auto s = metadata::as<std::string>(v);
            if (!s || std::ranges::find(spec.choices, s->get()) == spec.choices.end()) {
                fail(n, what + " must be one of: " + metadata::joinNames(spec.choices));
            }
            return;
        }
        case dimension::text:
            if (!std::holds_alternative<std::string>(v)) fail(n, what + " must be text");
            return;
        case dimension::color: {
            if (const auto s = metadata::as<std::string>(v)) {
                if (!spec.noneAllowed || s->get() != "none")
                    fail(n, what + " must be a color" + (spec.noneAllowed ? " or none" : ""));
                return;
            }
            const auto l = metadata::as<std::vector<double>>(v);
            if (!l || !metadata::isColor(l->get())) {
                fail(n, what + " must be four numbers from 0 to 1 (R, G, B, A)");
            }
            return;
        }
        case dimension::list: {
            const auto l = metadata::as<std::vector<double>>(v);
            if (!l) {
                fail(n, what + " must be a list of numbers");
            } else if (!std::all_of(l->get().begin(), l->get().end(), [](double x) { return std::isfinite(x); })) {
                fail(n, what + " must contain finite numbers");
            }
            return;
        }
        default: {
            const auto d = metadata::as<double>(v);
            if (!d) {
                fail(n, what + " must be a number (" + std::string(metadata::toString(spec.dim)) + ")");
            } else if (!(d->get() >= spec.min) || !(d->get() <= spec.max)) {
                fail(n, what + " is " + formatNumber(*d) + "; expected " + formatNumber(spec.min) + " to " +
                            formatNumber(spec.max));
            }
        }
        }
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

    const graphSpec& graph_;
    std::map<pipeRef, std::reference_wrapper<const pipeDecl>> pipes_;
    std::set<bufferRef> buffers_;
    std::vector<error> errors_;
};

} // namespace

std::vector<error> validate(const graphSpec& graph) { return checker(graph).run(); }

} // namespace ir
