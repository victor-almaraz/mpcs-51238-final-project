// Semantic IR checks (interpreter.md §12.3).

#include <proj/ir/ir.h>
#include <proj/metadata/primitiveSpec.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace ir {

namespace {

using metadata::dimension;
using metadata::graphSpec;
using metadata::nodeSpec;
using metadata::paramSpec;
using metadata::pipeDecl;
using metadata::pipeKind;
using metadata::pipeRef;
using metadata::portSpec;

std::string number(double v) {
    std::ostringstream out;
    out << v;
    return out.str();
}

class checker {
public:
    explicit checker(const graphSpec& g) : graph_(g) {}

    std::vector<error> run() {
        checkPipes();
        std::set<std::string_view> ids;
        for (const auto& n : graph_.nodes) {
            if (!ids.insert(n.id).second) fail(n, "duplicate node ID");
            checkNode(n);
        }
        if (errors_.empty()) checkOrder();
        return std::move(errors_);
    }

private:
    void fail(const nodeSpec& n, std::string message) { errors_.push_back({n.id, std::move(message), n.src}); }
    void fail(std::string message) { errors_.push_back({{}, std::move(message), std::nullopt}); }

    void checkPipes() {
        if (graph_.meter < 1) fail("meter must be at least 1");
        if (graph_.tempo && !(*graph_.tempo > 0.0 && std::isfinite(*graph_.tempo))) fail("tempo must be a positive number");
        for (const pipeDecl& p : graph_.pipes) {
            const std::string name = metadata::toString(p.pipe);
            if (!pipes_.emplace(p.pipe, std::cref(p)).second) fail(name + " is declared twice");
            if (p.pipe.index < 0) fail(name + ": negative index");
            if (p.channels < 1 || p.channels > metadata::maxChannels) {
                fail(name + ": channels must be 1 to " + std::to_string(metadata::maxChannels));
            }
            if (p.pipe.kind == pipeKind::event && p.channels != 1) fail(name + ": event pipes have one channel");
            if (p.pipe.kind == pipeKind::video) fail(name + ": video pipes are not supported yet");
            if (p.feedback && p.pipe.kind != pipeKind::audio && p.pipe.kind != pipeKind::control) {
                fail(name + ": only audio and control pipes can be feedback pipes");
            }
            if (p.pipe == pipeRef{pipeKind::control, metadata::tempoPipe} && (p.channels != 1 || p.feedback)) {
                fail(name + " is the tempo pipe: one channel, not feedback");
            }
        }
    }

    bool declared(pipeRef r) const {
        return pipes_.count(r) != 0 || r == pipeRef{pipeKind::control, metadata::tempoPipe};
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
        if (const auto r = metadata::as<pipeRef>(v)) {
            if (spec.bindable == metadata::staticOnly) {
                fail(n, what + " cannot be bound to a pipe");
            } else {
                checkRef(n, what, *r, spec.bindable);
            }
            return;
        }
        switch (spec.dim) {
        case dimension::choice: {
            const auto s = metadata::as<std::string>(v);
            if (!s || std::find(spec.choices.begin(), spec.choices.end(), s->get()) == spec.choices.end()) {
                std::string names;
                for (auto c : spec.choices) names += (names.empty() ? "" : ", ") + std::string(c);
                fail(n, what + " must be one of: " + names);
            }
            return;
        }
        case dimension::text:
            if (!std::holds_alternative<std::string>(v)) fail(n, what + " must be text");
            return;
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
            } else if (!(d->get() >= spec.min && d->get() <= spec.max)) {
                fail(n, what + " is " + number(*d) + "; expected " + number(spec.min) + " to " + number(spec.max));
            }
        }
        }
    }

    void checkPorts(const nodeSpec& n, std::string_view direction, std::span<const portSpec> specs,
                    const std::map<std::string, std::vector<pipeRef>, std::less<>>& bound) {
        for (const auto& [name, refs] : bound) {
            const auto it = std::find_if(specs.begin(), specs.end(), [&](const portSpec& p) { return p.name == name; });
            const std::string what = std::string(direction) + ' ' + name;
            if (it == specs.end()) {
                fail(n, "unknown " + what);
                continue;
            }
            if (refs.empty()) fail(n, what + " has no pipes");
            if (refs.size() > 1 && !it->multiple) fail(n, what + " takes one pipe");
            for (auto r : refs) checkRef(n, what, r, it->kinds);
        }
        for (const auto& p : specs) {
            if (p.required && bound.find(p.name) == bound.end()) {
                fail(n, "missing " + std::string(direction) + ' ' + std::string(p.name));
            }
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
        checkPorts(n, "input", spec.inputs, n.inputs);
        checkPorts(n, "output", spec.outputs, n.outputs);
    }

    bool feedback(pipeRef r) const {
        const auto it = pipes_.find(r);
        return it != pipes_.end() && it->second.get().feedback;
    }

    // Every reader of a non-feedback pipe follows all of its writers
    // (AGENTS.md: the compiler sorts the graph; the engine runs nodes in
    // IR order). A node that reads a pipe it writes is a cycle.
    void checkOrder() {
        std::map<pipeRef, std::size_t> lastWriter;
        for (std::size_t i = 0; i < graph_.nodes.size(); ++i) {
            for (auto r : metadata::pipesWritten(graph_.nodes[i])) lastWriter[r] = i;
        }
        for (std::size_t i = 0; i < graph_.nodes.size(); ++i) {
            const nodeSpec& n = graph_.nodes[i];
            for (auto r : metadata::pipesRead(n)) {
                if (feedback(r)) continue;
                const auto it = lastWriter.find(r);
                if (it == lastWriter.end() || it->second < i) continue;
                if (it->second == i) {
                    fail(n, "reads " + metadata::toString(r) + ", which it writes; close the loop through a feedback pipe");
                } else {
                    fail(n, "reads " + metadata::toString(r) + " before node " + graph_.nodes[it->second].id +
                                " writes it; order nodes by data flow, or use a feedback pipe for a cycle");
                }
            }
        }
    }

    const graphSpec& graph_;
    std::map<pipeRef, std::reference_wrapper<const pipeDecl>> pipes_;
    std::vector<error> errors_;
};

} // namespace

std::vector<error> validate(const graphSpec& graph) { return checker(graph).run(); }

} // namespace ir
