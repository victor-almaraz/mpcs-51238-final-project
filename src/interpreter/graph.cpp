// Graph building: pipes, primitive calls, mapper lowering, node IDs,
// sorting, and validation (interpreter.md §9, §10, §12).

#include "evaluator.h"

#include <proj/ir/ir.h>

#include <algorithm>
#include <cmath>
#include <queue>
#include <sstream>

namespace interpreter::detail {

using metadata::dimension;
using metadata::pipeKind;
using metadata::pipeRef;

namespace {

std::string join(const std::vector<std::string>& steps) {
    std::string out;
    for (const auto& s : steps) out += s + '/';
    return out;
}

} // namespace

metadata::sourceSpan evaluator::toSpan(const sourceSpan& s) const {
    return {file_, static_cast<int>(s.line), static_cast<int>(s.column), s.offset, s.length};
}

// --- Node IDs (§10) -------------------------------------------------------------

// The next step of the instance path: callee, call position, and
// discriminator, which is the occurrence index of this call under the
// same parent, or the call's :key.
std::string evaluator::nextStep(std::string_view callee, const sourceSpan& span, const std::optional<std::string>& key) {
    const std::string parent = join(steps_);
    std::string discriminator;
    if (key) {
        if (!keys_.insert(parent + '|' + std::string(callee) + '|' + *key).second) {
            fail(span, "two calls to " + std::string(callee) + " under the same parent use :key " + *key);
        }
        discriminator = *key;
    } else {
        discriminator = std::to_string(occurrences_[parent + '|' + std::to_string(span.offset)]++);
    }
    return std::string(callee) + '@' + std::to_string(span.line) + ':' + std::to_string(span.column) + '#' + discriminator;
}

std::string evaluator::instanceId(const std::string& step) const { return file_ + '/' + root_ + '/' + join(steps_) + step; }

// --- Pipes (§9.2) -------------------------------------------------------------------

value evaluator::allocatePipe(pipeKind k, int channels, bool feedback) {
    value v;
    v.k = kind::pipe;
    v.pipe = {k, nextIndex_[k]++};
    v.channels = channels;
    graph_.pipes.push_back({v.pipe, channels, feedback});
    return v;
}

value evaluator::makePipe(const datum& form, environment* env) {
    const auto& items = form.items;
    if (!building_) fail(form.span, "pipes are made while a piece is built: call pipe inside defpiece");
    if (items.size() < 2 || items[1].kind != datumKind::keyword) fail(form.span, "expected (pipe :audio), (pipe :control), or (pipe :event)");
    const auto k = metadata::parsePipeKind(items[1].text);
    if (!k) fail(items[1].span, "unknown pipe type :" + items[1].text + "; expected :audio, :control, or :event");
    if (*k == pipeKind::video) fail(items[1].span, "video pipes are not supported yet");
    int channels = 1;
    bool feedback = false;
    for (std::size_t i = 2; i < items.size(); i += 2) {
        const datum& name = items[i];
        if (name.kind != datumKind::keyword || i + 1 >= items.size()) fail(name.span, "expected :channels n or :feedback #t");
        const value v = eval(items[i + 1], env);
        if (name.text == "channels") {
            if (v.k != kind::integer || v.integer < 1 || v.integer > metadata::maxChannels) {
                fail(items[i + 1].span, "channels must be an integer from 1 to " + std::to_string(metadata::maxChannels));
            }
            if (*k == pipeKind::event && v.integer != 1) fail(items[i + 1].span, "event pipes have one channel");
            channels = static_cast<int>(v.integer);
        } else if (name.text == "feedback") {
            if (v.k != kind::boolean) fail(items[i + 1].span, "feedback is #t or #f");
            if (v.flag && *k == pipeKind::event) fail(items[i + 1].span, "event pipes cannot be feedback pipes");
            feedback = v.flag;
        } else {
            fail(name.span, "pipe has no option :" + name.text);
        }
    }
    return allocatePipe(*k, channels, feedback);
}

std::vector<pipeRef> evaluator::pipesOf(const value& v, const sourceSpan& span, bool input) {
    if (v.k == kind::pipe) return {v.pipe};
    if (v.k == kind::mapped) {
        if (!input) fail(span, "a mapped pipe can be read, not written");
        return {lower(*static_cast<mappedObject*>(v.obj)).pipe};
    }
    if (v.k == kind::vector) {
        std::vector<pipeRef> out;
        for (const auto& item : static_cast<vectorObject*>(v.obj)->items) {
            if (item.k == kind::vector) fail(span, "expected a pipe or a vector of pipes");
            const auto refs = pipesOf(item, span, input);
            out.insert(out.end(), refs.begin(), refs.end());
        }
        if (out.empty()) fail(span, "expected at least one pipe");
        return out;
    }
    fail(span, "expected a pipe or a vector of pipes, got " + show(v));
}

// --- Primitive calls (§9.1, §8.4) ------------------------------------------------------

metadata::paramValue evaluator::paramOf(const metadata::paramSpec& p, const value& v, const sourceSpan& span) {
    const std::string what = "parameter :" + std::string(p.name);
    if (v.k == kind::pipe || v.k == kind::mapped) {
        if (p.bindable == metadata::staticOnly) fail(span, what + " takes a constant, not a pipe");
        return pipesOf(v, span, true).front();
    }
    switch (p.dim) {
    case dimension::choice: {
        if ((v.k != kind::keyword && v.k != kind::string) ||
            std::find(p.choices.begin(), p.choices.end(), *v.text) == p.choices.end()) {
            break;
        }
        return *v.text;
    }
    case dimension::text:
        if (v.k != kind::string) fail(span, what + " expects a string, got " + show(v));
        return *v.text;
    case dimension::list: {
        if (v.k != kind::vector) fail(span, what + " expects a vector of numbers, such as [1 2 3]");
        std::vector<double> out;
        for (const auto& item : static_cast<vectorObject*>(v.obj)->items) {
            if (!item.isNumber()) fail(span, what + " expects numbers, got " + show(item));
            out.push_back(item.asDouble());
        }
        return out;
    }
    default:
        if (v.k == kind::integer || v.k == kind::real) return v.asDouble();  // the canonical unit (§8.4)
        if (v.k == kind::quantity) {
            if (v.dim == p.dim) return v.number;
            if (v.dim == dimension::beats && p.dim == dimension::time) {
                fail(span, what + ": beat values for time parameters are not supported yet; use seconds");
            }
            fail(span, what + " expects " + (p.dim == dimension::number ? "a plain number" : article(p.dim)) +
                           ", got " + article(v.dim));
        }
        fail(span, what + " expects a number, got " + show(v));
    }
    std::string names;
    for (auto c : p.choices) names += (names.empty() ? ":" : ", :") + std::string(c);
    fail(span, what + " expects one of " + names + ", got " + show(v));
}

value evaluator::callPrimitive(const metadata::primitiveSpec& spec, callContext& c) {
    const std::string name = languageName(spec.type);
    if (!building_) fail(c.span, name + " adds a node, so it must be called while a piece is built");
    if (!c.positional.empty()) fail(c.positionalSpans[0], name + " takes keyword arguments, such as :in and :out");
    metadata::nodeSpec n;
    n.type = std::string(spec.type);
    for (std::size_t i = 0; i < c.keywords.size(); ++i) {
        const auto& [key, v] = c.keywords[i];
        const sourceSpan& at = c.keywordSpans[i];
        if (spec.input(*key)) {
            n.inputs[*key] = pipesOf(v, at, true);
        } else if (spec.output(*key)) {
            n.outputs[*key] = pipesOf(v, at, false);
        } else if (const auto* p = spec.param(*key)) {
            n.params[*key] = paramOf(*p, v, at);
        } else {
            fail(at, name + " has no parameter or port :" + *key);
        }
    }
    n.id = instanceId(nextStep(name, c.span, c.key));
    n.src = toSpan(c.span);
    if (!ids_.insert(n.id).second) fail(c.span, "two nodes have the ID " + n.id);  // §10.4
    graph_.nodes.push_back(std::move(n));
    nodeSpans_.push_back(c.span);
    value out;
    out.k = kind::node;
    out.node = graph_.nodes.size() - 1;
    return out;
}

// --- Mappers (§9.4, §9.5) -------------------------------------------------------------

value evaluator::lower(mappedObject& m) {
    if (m.lowered) return *m.lowered;
    // Lower where the mapper was made, so its nodes' IDs do not depend on
    // where the mapped pipe is used.
    const std::vector<std::string> saved = steps_;
    steps_ = m.path;
    ++lowering_;
    callContext c;
    c.callee = "mapper";
    c.span = m.span;
    c.positional.push_back(m.source.k == kind::mapped ? lower(*static_cast<mappedObject*>(m.source.obj)) : m.source);
    c.positionalSpans.push_back(m.span);
    value result;
    try {
        result = applyClosure(*m.fn, c, false);
    } catch (...) {
        --lowering_;
        steps_ = saved;
        throw;
    }
    --lowering_;
    steps_ = saved;
    if (result.k != kind::pipe) fail(m.span, "a mapper must return its input transformed, got " + show(result));
    m.lowered = result;
    return result;
}

// Adds a generator for @p op and returns its output pipe: audio when any
// input is audio, otherwise control, with the widest input's channels.
value evaluator::emit(std::string_view op, std::string_view type, const std::vector<value>& inputs,
                      const std::vector<std::pair<std::string, value>>& params, const sourceSpan& span) {
    pipeKind k = pipeKind::control;
    int channels = 1;
    auto consider = [&](const value& v) {
        if (v.k != kind::pipe) return;
        if (v.pipe.kind == pipeKind::audio) k = pipeKind::audio;
        channels = std::max(channels, v.channels);
    };
    std::for_each(inputs.begin(), inputs.end(), consider);
    for (const auto& [name, v] : params) consider(v);
    const value out = allocatePipe(k, channels, false);

    metadata::nodeSpec n;
    n.type = std::string(type);
    for (const auto& in : inputs) n.inputs["in"].push_back(in.pipe);
    for (const auto& [name, v] : params) {
        if (v.k == kind::pipe) {
            n.params[name] = v.pipe;
        } else {
            n.params[name] = v.asDouble();
        }
    }
    n.outputs["out"] = {out.pipe};
    n.id = instanceId(nextStep(op, span, std::nullopt));
    n.src = toSpan(span);
    ids_.insert(n.id);
    graph_.nodes.push_back(std::move(n));
    nodeSpans_.push_back(span);
    return out;
}

value evaluator::emitMath(std::string_view op, std::vector<value> args, const sourceSpan& span) {
    if (lowering_ == 0) fail(span, "a pipe can be transformed only inside a mapper: (mapper pipe (fn (x) ...))");
    for (auto& a : args) {
        if (a.k == kind::mapped) a = lower(*static_cast<mappedObject*>(a.obj));
        if (a.k != kind::pipe && !a.isNumber()) fail(span, std::string(op) + " expects numbers or pipes, got " + show(a));
    }
    auto isPipe = [](const value& v) { return v.k == kind::pipe; };
    using params = std::vector<std::pair<std::string, value>>;

    if (op == "+" || op == "*") {
        const bool sum = op == "+";
        double constant = sum ? 0.0 : 1.0;
        std::vector<value> pipes;
        for (const auto& a : args) {
            if (isPipe(a)) {
                pipes.push_back(a);
            } else {
                constant = sum ? constant + a.asDouble() : constant * a.asDouble();
            }
        }
        value result = pipes.size() > 1 ? emit(op, sum ? "add" : "multiply", pipes, {}, span) : pipes.front();
        if (sum && constant != 0.0) result = emit(op, "subtract", {result}, params{{"b", makeReal(-constant)}}, span);
        if (!sum && constant != 1.0) result = emit(op, "gain", {result}, params{{"amount", makeReal(constant)}}, span);
        return result;
    }
    if (op == "-" || op == "/") {
        const bool minus = op == "-";
        if (args.size() == 1) {
            return minus ? emit(op, "neg", {args[0]}, {}, span) : emit(op, "pow", {args[0]}, params{{"b", makeReal(-1.0)}}, span);
        }
        value acc = args[0];
        for (std::size_t i = 1; i < args.size(); ++i) {
            const value& b = args[i];
            if (isPipe(acc)) {
                acc = emit(op, minus ? "subtract" : "divide", {acc}, params{{"b", b}}, span);
            } else if (isPipe(b)) {
                // c - x is -x + c; c / x is c · x^-1.
                if (minus) {
                    acc = emit(op, "subtract", {emit(op, "neg", {b}, {}, span)}, params{{"b", makeReal(-acc.asDouble())}}, span);
                } else {
                    const double c = acc.asDouble();
                    acc = emit(op, "pow", {b}, params{{"b", makeReal(-1.0)}}, span);
                    if (c != 1.0) acc = emit(op, "gain", {acc}, params{{"amount", makeReal(c)}}, span);
                }
            } else {
                acc = makeReal(minus ? acc.asDouble() - b.asDouble() : acc.asDouble() / b.asDouble());
            }
        }
        return acc;
    }

    struct form {
        std::string_view name;
        std::string_view type;
        std::vector<std::string_view> params;
    };
    static const form forms[] = {
        {"neg", "neg", {}},     {"abs", "abs", {}},         {"exp", "exp", {}},           {"log", "log", {}},
        {"sin", "sin", {}},     {"cos", "cos", {}},         {"tanh", "tanh", {}},         {"floor", "floor", {}},
        {"round", "round", {}}, {"pow", "pow", {"b"}},      {"min", "min", {"b"}},        {"max", "max", {"b"}},
        {"quantize", "quantize", {"step"}},                 {"clamp", "clamp", {"low", "high"}},
        {"scale", "scale", {"in-low", "in-high", "out-low", "out-high"}},
        {"scale-exp", "scaleExp", {"in-low", "in-high", "out-low", "out-high"}},
    };
    const auto it = std::find_if(std::begin(forms), std::end(forms), [&](const form& f) { return f.name == op; });
    if (it == std::end(forms)) fail(span, std::string(op) + " cannot be used in a mapper");
    if (args.size() != it->params.size() + 1) {
        fail(span, std::string(op) + " takes " + std::to_string(it->params.size() + 1) + " arguments");
    }
    if (!isPipe(args[0])) {
        // min and max commute; the other operations take the pipe first.
        if ((op == "min" || op == "max") && isPipe(args[1])) {
            std::swap(args[0], args[1]);
        } else {
            fail(span, std::string(op) + " takes the pipe as its first argument");
        }
    }
    params p;
    for (std::size_t i = 0; i < it->params.size(); ++i) p.emplace_back(std::string(it->params[i]), args[i + 1]);
    return emit(op, it->type, {args[0]}, p, span);
}

// --- Sorting and validation ---------------------------------------------------------------

metadata::graphSpec evaluator::finish(std::vector<compileError>& errors) {
    auto& nodes = graph_.nodes;
    const std::size_t n = nodes.size();
    std::set<pipeRef> feedback;
    for (const auto& d : graph_.pipes) {
        if (d.feedback) feedback.insert(d.pipe);
    }
    // Writers run before readers (AGENTS.md: the compiler sorts the graph);
    // ties keep the order of evaluation. A node reading a pipe it writes
    // is a cycle too.
    std::map<pipeRef, std::vector<std::size_t>> writers;
    for (std::size_t i = 0; i < n; ++i) {
        for (auto r : metadata::pipesWritten(nodes[i])) {
            if (!feedback.count(r)) writers[r].push_back(i);
        }
    }
    std::vector<std::vector<std::size_t>> successors(n);
    std::vector<std::size_t> indegree(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        for (auto r : metadata::pipesRead(nodes[i])) {
            const auto w = writers.find(r);
            if (w == writers.end()) continue;
            for (std::size_t from : w->second) {
                successors[from].push_back(i);
                ++indegree[i];
            }
        }
    }
    std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
    for (std::size_t i = 0; i < n; ++i) {
        if (indegree[i] == 0) ready.push(i);
    }
    std::vector<std::size_t> order;
    while (!ready.empty()) {
        const std::size_t i = ready.top();
        ready.pop();
        order.push_back(i);
        for (std::size_t s : successors[i]) {
            if (--indegree[s] == 0) ready.push(s);
        }
    }
    if (order.size() < n) {
        // The nodes left are on a cycle or downstream of one. Peel off those
        // with no remaining successor until only the cycles are left.
        std::vector<bool> left(n, false);
        for (std::size_t i = 0; i < n; ++i) left[i] = indegree[i] != 0;
        for (bool changed = true; changed;) {
            changed = false;
            for (std::size_t i = 0; i < n; ++i) {
                if (left[i] && std::none_of(successors[i].begin(), successors[i].end(), [&](std::size_t s) { return left[s]; })) {
                    left[i] = false;
                    changed = true;
                }
            }
        }
        std::string names;
        std::size_t first = n;
        for (std::size_t i = 0; i < n; ++i) {
            if (!left[i]) continue;
            if (first == n) first = i;
            names += (names.empty() ? "" : ", ") + languageName(nodes[i].type) + " (" +
                     std::to_string(nodeSpans_[i].line) + ':' + std::to_string(nodeSpans_[i].column) + ')';
        }
        errors.push_back({"these nodes form a cycle: " + names +
                              ". Close a loop through a feedback pipe: (pipe :audio :feedback #t)",
                          file_, nodeSpans_[first], {}});
        return {};
    }
    std::vector<metadata::nodeSpec> sorted;
    std::vector<sourceSpan> spans;
    for (std::size_t i : order) {
        sorted.push_back(std::move(nodes[i]));
        spans.push_back(nodeSpans_[i]);
    }
    nodes = std::move(sorted);
    nodeSpans_ = std::move(spans);

    for (const auto& e : ir::validate(graph_)) {
        sourceSpan at{};
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].id == e.node) at = nodeSpans_[i];
        }
        errors.push_back({e.message, file_, at, {}});
    }
    return std::move(graph_);
}

// --- Hazards ---------------------------------------------------------------------

namespace {

// Gains above +24 dB are almost always mistakes for audio.
constexpr double loudGain = 15.85;

// The static gain a node applies to the audio it reads from @p in: a
// gain's amount, an envelope's gain, a mix input's level. Other nodes,
// and bound gains, count as 1.
double staticGain(const metadata::nodeSpec& n, pipeRef in) {
    auto number = [&](const char* name) {
        const auto it = n.params.find(name);
        const auto* d = it == n.params.end() ? nullptr : std::get_if<double>(&it->second);
        return d ? std::abs(*d) : 1.0;
    };
    if (n.type == "gain") return number("amount");
    if (n.type == "adsr") return number("gain");
    if (n.type == "mix") {
        const auto it = n.params.find("levels");
        const auto* levels = it == n.params.end() ? nullptr : std::get_if<std::vector<double>>(&it->second);
        const auto& ins = n.inputs.at("in");
        double most = 0.0;
        for (std::size_t k = 0; k < ins.size(); ++k) {
            if (ins[k] == in) most = std::max(most, levels && k < levels->size() ? std::abs((*levels)[k]) : 1.0);
        }
        return most;
    }
    return 1.0;
}

std::string number(double v) {
    std::ostringstream out;
    out << v;
    return out.str();
}

} // namespace

// Runs on the sorted graph. Loop gains are estimated from static gains
// along audio inputs; nodes of other types count as unity.
std::vector<compileError> evaluator::hazards(const metadata::graphSpec& graph) const {
    std::vector<compileError> out;
    const auto& nodes = graph.nodes;
    const std::size_t n = nodes.size();
    auto warn = [&](std::size_t i, std::string message) { out.push_back({std::move(message), file_, nodeSpans_[i], {}}); };

    std::map<pipeRef, std::vector<std::size_t>> writers;
    for (std::size_t i = 0; i < n; ++i) {
        for (auto r : metadata::pipesWritten(nodes[i])) writers[r].push_back(i);
    }

    // Loops through audio feedback pipes (§5.6): the largest product of
    // gains from the pipe's readers to its writers, along audio inputs.
    for (const auto& d : graph.pipes) {
        if (!d.feedback || d.pipe.kind != pipeKind::audio) continue;
        std::vector<double> reach(n, 0.0);  // gain from the feedback pipe to each node's output
        for (std::size_t i = 0; i < n; ++i) {
            for (const auto& [port, refs] : nodes[i].inputs) {
                for (auto r : refs) {
                    if (r.kind != pipeKind::audio) continue;
                    double from = 0.0;
                    if (r == d.pipe) {
                        from = 1.0;
                    } else if (const auto w = writers.find(r); w != writers.end()) {
                        for (std::size_t j : w->second) {
                            if (j < i) from = std::max(from, reach[j]);  // the sort puts writers first
                        }
                    }
                    if (from > 0.0) reach[i] = std::max(reach[i], from * staticGain(nodes[i], r));
                }
            }
        }
        for (std::size_t w : writers[d.pipe]) {
            if (reach[w] >= 1.0) {
                warn(w, "the loop through feedback " + metadata::toString(d.pipe) + " has a gain of " + number(reach[w]) +
                            ", so it grows without bound; keep the loop's gain below 1");
            }
        }
    }

    const double nyquist = options_.sampleRate / 2.0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& node = nodes[i];
        const auto* spec = metadata::findPrimitive(node.type);
        const bool audioOut = std::any_of(node.outputs.begin(), node.outputs.end(), [](const auto& o) {
            return std::any_of(o.second.begin(), o.second.end(), [](pipeRef r) { return r.kind == pipeKind::audio; });
        });
        for (const auto& [name, v] : node.params) {
            const auto* p = spec ? spec->param(name) : nullptr;
            if (!p) continue;
            if (const auto* d = std::get_if<double>(&v)) {
                if (p->dim == dimension::frequency && *d > nyquist) {
                    warn(i, "parameter :" + name + " is " + number(*d) + " Hz, above half the " + number(options_.sampleRate) +
                                " Hz sample rate; it aliases or is clamped");
                }
                if (audioOut && p->dim == dimension::level && std::abs(*d) > loudGain && (node.type == "gain" || node.type == "adsr")) {
                    warn(i, "a gain of " + number(*d) + " is " + number(std::round(20.0 * std::log10(std::abs(*d)))) +
                                " dB; the output limiter will hold it at its ceiling");
                }
            } else if (const auto* l = std::get_if<std::vector<double>>(&v); l && node.type == "mix" && audioOut) {
                for (double x : *l) {
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

} // namespace interpreter::detail
