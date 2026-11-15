// Graph building: pipes, primitive calls, mapper lowering, node IDs,
// sorting, and validation (interpreter.md §9, §10, §12).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <proj/interpreter/compiler.h>
#include <proj/interpreter/reader.h>
#include <proj/ir/ir.h>
#include <proj/metadata/graph.h>
#include <proj/metadata/primitiveSpec.h>
#include <proj/metadata/text.h>

#include "evaluator.h"

namespace interpreter::detail {

using metadata::dimension;
using metadata::formatNumber;
using metadata::pipeKind;
using metadata::pipeRef;

namespace {

std::string join(const std::vector<std::string>& steps) {
    std::string out;
    for (const auto& s : steps) out += s + '/';
    return out;
}

// "an audio pipe", "an audio or control pipe".
std::string pipeKinds(metadata::kindSet kinds) {
    std::string out;
    for (const auto k : {pipeKind::audio, pipeKind::control, pipeKind::event, pipeKind::video}) {
        if (!metadata::contains(kinds, k)) continue;
        out += out.empty() ? (k == pipeKind::audio || k == pipeKind::event ? "an " : "a ") : " or ";
        out += metadata::toString(k);
    }
    return out + " pipe";
}

std::string kindOf(pipeRef r) { return pipeKinds(metadata::bit(r.kind)); }

// @p v, a canonical value of dimension @p d, as the user writes it.
std::string inUnits(double v, dimension d) {
    switch (d) {
    case dimension::time:
        return v != 0.0 && std::abs(v) < 1.0 ? formatNumber(v * 1e3) + "ms" : formatNumber(v) + "s";
    case dimension::frequency: return formatNumber(v) + "hz";
    case dimension::level:
        if (v <= 0.0) return formatNumber(v);  // no decibel value
        return formatNumber(std::round(2000.0 * std::log10(v)) / 100.0) + "db";
    case dimension::interval: return formatNumber(v) + "st";
    case dimension::beats: return formatNumber(v) + "beat";
    default: return formatNumber(v);
    }
}

} // namespace

metadata::sourceSpan toIrSpan(const sourceSpan& s, const std::string& file) {
    return {.file = file,
            .line = static_cast<int>(s.line),
            .column = static_cast<int>(s.column),
            .offset = s.offset,
            .length = s.length};
}

sourceSpan fromIrSpan(const std::optional<metadata::sourceSpan>& s) {
    if (!s) return {};
    return {.offset = static_cast<std::uint32_t>(s->offset),
            .length = static_cast<std::uint32_t>(s->length),
            .line = static_cast<std::uint32_t>(s->line),
            .column = static_cast<std::uint32_t>(s->column)};
}

// --- Node IDs (§10) -------------------------------------------------------------

// The next step of the instance path: callee, call position, and
// discriminator, which is the occurrence index of this call under the
// same parent, or the call's :key.
std::string evaluator::nextStep(std::string_view callee, const sourceSpan& span, const std::optional<std::string>& key) {
    const std::string parent = join(path_);
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

std::string evaluator::instanceId(const std::string& step) const {
    return files_.front()->path + '/' + root_ + '/' + join(path_) + step;
}

// --- Pipes (§9.2) -------------------------------------------------------------------

value evaluator::allocatePipe(metadata::pipeKind k, int channels, bool feedback) {
    value v;
    v.k = kind::pipe;
    v.pipe = {.kind = k, .index = nextIndex_[k]++};
    v.channels = channels;
    graph_.pipes.push_back({.pipe = v.pipe, .channels = channels, .feedback = feedback});
    return v;
}

value evaluator::makePipe(const datum& form, const std::shared_ptr<environment>& env) {
    const auto& items = form.items;
    if (!building_) fail(form.span, "pipes are made while a piece is built: call pipe inside defpiece");
    if (items.size() < 2 || items[1].kind != datumKind::keyword) {
        fail(form.span, "expected (pipe :audio), (pipe :control), (pipe :event), or (pipe :video)");
    }
    const auto k = metadata::parsePipeKind(items[1].text);
    if (!k)
        fail(items[1].span,
             "unknown pipe type :" + items[1].text + "; expected :audio, :control, :event, or :video");
    int channels = 1;
    bool feedback = false;
    std::set<std::string_view> given;
    for (std::size_t i = 2; i < items.size(); i += 2) {
        const datum& name = items[i];
        if (name.kind != datumKind::keyword || i + 1 >= items.size()) fail(name.span, "expected :channels n or :feedback #t");
        if (!given.insert(name.text).second) fail(name.span, ":" + name.text + " is given twice");
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
        return {lower(static_cast<mappedObject&>(*v.obj)).pipe};
    }
    if (v.k == kind::vector) {
        std::vector<pipeRef> out;
        for (const auto& item : static_cast<vectorObject&>(*v.obj).items) {
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

// The range of @p p, in the user's units (interpreter.md §8.4; IR
// validation is the backstop). Bounds at the table's open ends, and a
// level's floor of 0, which has no decibel value, are not shown.
void evaluator::checkRange(const metadata::paramSpec& p, double v, const sourceSpan& span,
                           const std::string& what) const {
    if (!std::isfinite(v)) fail(span, what + " must be a finite number");
    if (v >= p.min && v <= p.max) return;
    const bool low = p.min > -1e9 && (p.dim != dimension::level || p.min != 0.0);
    const bool high = p.max < 1e9;
    const std::string range = low && high ? inUnits(p.min, p.dim) + " to " + inUnits(p.max, p.dim)
                              : low       ? "at least " + inUnits(p.min, p.dim)
                                          : "at most " + inUnits(p.max, p.dim);
    fail(span, what + " is " + inUnits(v, p.dim) + "; expected " + range);
}

metadata::paramValue evaluator::paramOf(const metadata::paramSpec& p, const value& v, const sourceSpan& span) {
    const std::string what = "parameter :" + std::string(p.name);
    if (v.isSymbolic()) {
        if (p.bindable == metadata::staticOnly) fail(span, what + " takes a constant, not a pipe");
        const pipeRef r = pipesOf(v, span, true).front();
        if (!metadata::contains(p.bindable, r.kind)) {
            const std::string constant = p.dim == dimension::number ? "a number" : article(p.dim);
            fail(span, what + " expects " + constant + " or " + pipeKinds(p.bindable) + ", got " + kindOf(r));
        }
        if (p.dim == dimension::color && v.k == kind::pipe && v.channels < metadata::colorChannels) {
            fail(span, what + " must follow a pipe of " + std::to_string(metadata::colorChannels) +
                           " channels (R, G, B, A), such as (pipe :control :channels 4)");
        }
        return r;
    }
    switch (p.dim) {
    case dimension::color: {
        if (v.k == kind::keyword && *v.text == "none") {
            if (!p.noneAllowed) fail(span, what + " takes a color, not :none");
            return std::string("none");
        }
        // Built only on failure: most colors are valid.
        const auto expects = [&] {
            return what + " expects a color, such as (rgb 1 0.5 0)" + (p.noneAllowed ? " or :none" : "");
        };
        if (v.k != kind::vector) fail(span, expects() + ", got " + show(v));
        const auto& items = static_cast<vectorObject&>(*v.obj).items;
        if (items.size() != metadata::colorChannels) fail(span, expects());
        std::vector<double> out;
        out.reserve(items.size());
        for (const auto& item : items) {
            if (!item.isColorComponent()) fail(span, expects());
            out.push_back(item.asDouble());
        }
        return out;
    }
    case dimension::choice:
        if ((v.k == kind::keyword || v.k == kind::string) &&
            std::ranges::find(p.choices, *v.text) != p.choices.end()) {
            return *v.text;
        }
        fail(span, what + " expects one of " + metadata::joinNames(p.choices, ":") + ", got " + show(v));
    case dimension::text:
        if (v.k != kind::string) fail(span, what + " expects a string, got " + show(v));
        return *v.text;
    case dimension::list: {
        if (v.k != kind::vector) fail(span, what + " expects a vector of numbers, such as [1 2 3]");
        std::vector<double> out;
        for (const auto& item : static_cast<vectorObject&>(*v.obj).items) {
            if (!item.isNumber()) fail(span, what + " expects numbers, got " + show(item));
            if (!std::isfinite(item.asDouble())) fail(span, what + " must contain finite numbers");
            out.push_back(item.asDouble());
        }
        return out;
    }
    default: {
        if (v.k == kind::quantity && v.dim == dimension::beats && p.dim == dimension::time) {
            // Seconds at the tempo of each block (§8.6, primitives.md §7.4.6).
            if (!metadata::contains(p.bindable, pipeKind::control))
                fail(span,
                     what + " takes a constant time, which cannot follow the tempo; write it in seconds");
            if (v.number < 0.0) fail(span, what + " is " + show(v) + "; expected at least 0 beats");
            return emit("beats-to-seconds", "beatsToSeconds", {}, {{"beats", makeReal(v.number)}}, span).pipe;
        }
        if (v.k == kind::quantity && v.dim != p.dim) {
            fail(span, what + " expects " + (p.dim == dimension::number ? "a plain number" : article(p.dim)) +
                           ", got " + article(v.dim));
        }
        if (!v.isNumber()) fail(span, what + " expects a number, got " + show(v));
        const double d = v.asDouble();  // a plain number is in the canonical unit (§8.4)
        checkRange(p, d, span, what);
        return d;
    }
    }
}

value evaluator::callPrimitive(const metadata::primitiveSpec& spec, callContext& c) {
    const std::string name = languageName(spec.type);
    if (!building_) fail(c.span, name + " adds a node, so it must be called while a piece is built");
    if (!c.positional.empty()) fail(c.positionalSpans[0], name + " takes keyword arguments, such as :in and :out");
    metadata::nodeSpec n;
    n.type = std::string(spec.type);
    for (const auto& arg : c.keywords) {
        const std::string& key = *arg.name;
        const auto input = spec.input(key);
        const auto port = input ? input : spec.output(key);
        if (port) {
            const metadata::portSpec& p = *port;
            auto refs = pipesOf(arg.v, arg.valueSpan, input.has_value());
            if (refs.size() > 1 && !p.multiple) {
                fail(arg.valueSpan, ":" + key + " takes one pipe, got " + std::to_string(refs.size()));
            }
            for (const auto r : refs) {
                if (!metadata::contains(p.kinds, r.kind)) {
                    fail(arg.valueSpan, ":", key, " expects ", pipeKinds(p.kinds), ", got ", kindOf(r));
                }
            }
            (input ? n.inputs : n.outputs)[key] = std::move(refs);
        } else if (const auto param = spec.param(key)) {
            n.params[key] = paramOf(*param, arg.v, arg.valueSpan);
        } else {
            fail(arg.nameSpan, name, " has no parameter or port :", key);
        }
    }
    for (const bool input : {true, false}) {
        for (const auto& p : input ? spec.inputs : spec.outputs) {
            if (p.required && !(input ? n.inputs : n.outputs).contains(p.name))
                fail(c.span, name, " needs :", p.name);
        }
    }
    for (const auto& message : metadata::checkConstraints(spec, n, graph_, metadata::naming::language)) {
        fail(c.span, name, ": ", message);
    }
    n.id = instanceId(nextStep(name, c.span, c.key));
    n.src = toIrSpan(c.span, fileName());
    if (!ids_.insert(n.id).second) fail(c.span, "two nodes have the ID " + n.id);  // §10.4
    value out;
    out.k = kind::node;
    out.text = intern(n.id);  // a stable identity: graph indices change when the graph is sorted
    graph_.nodes.push_back(std::move(n));
    return out;
}

// --- Mappers (§9.4, §9.5) -------------------------------------------------------------

value evaluator::lower(mappedObject& m) {
    if (m.lowered) return *m.lowered;
    // A chain of mappers lowers recursively, so it counts as nesting.
    const depthGuard depth(*this, m.span);
    // Lower where the mapper was made, so its nodes' IDs do not depend on
    // where the mapped pipe is used.
    struct restore {
        evaluator& e;
        std::vector<std::string> path;
        ~restore() {
            e.path_ = std::move(path);
            --e.lowering_;
        }
    } const guard{.e = *this, .path = std::exchange(path_, m.path)};
    ++lowering_;
    callContext c;
    c.callee = "mapper";
    c.span = m.span;
    c.positional.push_back(m.source.k == kind::mapped ? lower(static_cast<mappedObject&>(*m.source.obj)) : m.source);
    c.positionalSpans.push_back(m.span);
    const value result = applyClosure(*m.fn, c, false);
    if (result.k != kind::pipe) fail(m.span, "a mapper must return its input transformed, got " + show(result));
    m.lowered = result;
    return result;
}

// Adds a generator for @p op and returns its output pipe: audio when any
// input is audio, otherwise control, with the widest input's channels.
value evaluator::emit(std::string_view op, std::string_view type, const std::vector<value>& inputs,
                      const std::vector<std::pair<std::string, value>>& params, const sourceSpan& span) {
    // Callers name built-in generator types and their parameters; a miss is
    // a compiler bug, reported at the call rather than read through.
    const auto found = metadata::findPrimitive(type);
    if (!found) fail(span, "internal error: no primitive named ", type);
    const metadata::primitiveSpec& spec = *found;
    metadata::pipeKind k = pipeKind::control;
    int channels = 1;
    auto consider = [&](const value& v) {
        if (v.k != kind::pipe) return;
        if (v.pipe.kind == pipeKind::audio) k = pipeKind::audio;
        channels = std::max(channels, v.channels);
    };
    std::ranges::for_each(inputs, consider);
    for (const auto& [name, v] : params) consider(v);
    const value out = allocatePipe(k, channels, false);

    metadata::nodeSpec n;
    n.type = std::string(type);
    for (const auto& in : inputs) n.inputs["in"].push_back(in.pipe);
    for (const auto& [name, v] : params) {
        if (v.k == kind::pipe) {
            n.params[name] = v.pipe;
        } else {
            const auto param = spec.param(name);
            if (!param) fail(span, "internal error: ", type, " has no parameter ", name);
            checkRange(*param, v.asDouble(), span, std::string(op) + ": :" + name);
            n.params[name] = v.asDouble();
        }
    }
    n.outputs["out"] = {out.pipe};
    n.id = instanceId(nextStep(op, span, std::nullopt));
    n.src = toIrSpan(span, fileName());
    ids_.insert(n.id);
    graph_.nodes.push_back(std::move(n));
    return out;
}

value evaluator::emitMath(callContext& c, metadata::optionalRef<metadata::primitiveSpec> op) {
    if (lowering_ == 0)
        fail(c.span, "a pipe can be transformed only inside a mapper: (mapper pipe (fn (x) ...))");
    const std::string name(c.callee);
    // A keyed operation adds a step named by its key; its nodes go under it.
    const std::size_t depth = path_.size();
    if (c.key) path_.push_back(nextStep(name, c.span, c.key));
    struct restore {
        std::vector<std::string>& path;
        std::size_t depth;
        ~restore() { path.resize(depth); }
    } const guard{.path = path_, .depth = depth};
    for (std::size_t i = 0; i < c.positional.size(); ++i) {
        auto& a = c.positional[i];
        const sourceSpan& at = c.positionalSpans[i];
        if (a.k == kind::mapped) a = lower(static_cast<mappedObject&>(*a.obj));
        if (a.k == kind::pipe && a.pipe.kind != pipeKind::audio && a.pipe.kind != pipeKind::control) {
            fail(at, name + " expects numbers or audio or control pipes, got " + show(a));
        }
        if (a.k != kind::pipe && !a.isNumber()) fail(at, name + " expects numbers or pipes, got " + show(a));
        if (a.k == kind::quantity && a.dim == dimension::beats) {
            fail(at, "beat values are not supported in mappers yet; write the time in seconds");
        }
    }
    if (!op) return emitArithmetic(c);

    const metadata::primitiveSpec& spec = *op;
    checkOperands(spec, c);
    auto& args = c.positional;
    if (args[0].k != kind::pipe) {
        // min and max commute; the other operations take the pipe first.
        if ((spec.type == "min" || spec.type == "max") && args[1].k == kind::pipe) {
            std::swap(args[0], args[1]);
        } else {
            fail(c.span, name + " takes the pipe as its first argument");
        }
    }
    // scale onto a range of levels interpolates in decibels, then converts
    // (§9.4): halfway from -40 dB to 0 dB is -20 dB.
    const auto isLevel = [](const value& v) { return v.k == kind::quantity && v.dim == dimension::level; };
    const bool toLevel = spec.type == "scale" && isLevel(args[3]) && isLevel(args[4]);
    if (toLevel) {
        for (const std::size_t i : {std::size_t{3}, std::size_t{4}}) {
            if (args[i].number <= 0.0)
                fail(c.positionalSpans[i], "a level of 0 has no decibel value; give a floor such as -60db");
            args[i] = makeReal(20.0 * std::log10(args[i].number));
        }
    }
    std::vector<std::pair<std::string, value>> params;
    params.reserve(spec.params.size());
    for (std::size_t i = 0; i < spec.params.size(); ++i)
        params.emplace_back(std::string(spec.params[i].name), args[i + 1]);
    const value out = emit(name, spec.type, {args[0]}, params, c.span);
    return toLevel ? emit("db-to-amp", "dbToAmp", {out}, {}, c.span) : out;
}

// + - * / on pipes and constants (grammar.md §5.2). The pipes' values are
// in canonical units of unknown dimension; constants follow the rules of
// constant arithmetic among themselves, and a level, which adds as
// decibels, is ambiguous next to a pipe.
value evaluator::emitArithmetic(callContext& c) {
    const std::string op(c.callee);
    const auto& args = c.positional;
    const sourceSpan& span = c.span;
    using params = std::vector<std::pair<std::string, value>>;
    auto isPipe = [](const value& v) { return v.k == kind::pipe; };
    // Constants [indices] folded by constant arithmetic, which checks their
    // dimensions and divisors.
    auto fold = [&](char with, const std::vector<std::size_t>& indices) {
        callContext sub;
        sub.callee = c.callee;
        sub.span = span;
        for (const std::size_t i : indices) {
            sub.positional.push_back(args[i]);
            sub.positionalSpans.push_back(c.positionalSpans[i]);
        }
        return arithmetic(sub, with);
    };

    if (op == "+" || op == "-") {
        // Constants share one dimension; + checks it when it folds them.
        std::vector<std::size_t> constants;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (isPipe(args[i])) continue;
            if (args[i].k == kind::quantity && args[i].dim == dimension::level) {
                fail(c.positionalSpans[i],
                     std::string(op == "+" ? "adding" : "subtracting") +
                         " a level is ambiguous; multiply by it instead, such as (* x -6db)");
            }
            constants.push_back(i);
        }
        if (op == "-" && constants.size() > 1) fold('-', constants);
    }

    if (op == "+" || op == "*") {
        const bool sum = op == "+";
        std::vector<value> pipes;
        std::vector<std::size_t> constants;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (isPipe(args[i])) {
                pipes.push_back(args[i]);
            } else {
                constants.push_back(i);
            }
        }
        const double constant =
            constants.empty() ? (sum ? 0.0 : 1.0) : fold(sum ? '+' : '*', constants).asDouble();
        value result = pipes.size() > 1 ? emit(op, sum ? "add" : "multiply", pipes, {}, span) : pipes.front();
        if (sum && constant != 0.0) result = emit(op, "subtract", {result}, params{{"b", makeReal(-constant)}}, span);
        if (!sum && constant != 1.0) result = emit(op, "gain", {result}, params{{"amount", makeReal(constant)}}, span);
        return result;
    }

    const bool minus = op == "-";
    if (args.size() == 1) {
        return minus ? emit(op, "neg", {args[0]}, {}, span)
                     : emit(op, "pow", {args[0]}, params{{"b", makeReal(-1.0)}}, span);
    }
    value acc = args[0];
    for (std::size_t i = 1; i < args.size(); ++i) {
        const value& b = args[i];
        if (!minus && !isPipe(b) && b.asDouble() == 0.0) fail(c.positionalSpans[i], "division by zero");
        if (isPipe(acc)) {
            acc = emit(op, minus ? "subtract" : "divide", {acc}, params{{"b", b}}, span);
        } else if (isPipe(b)) {
            // c - x is -x + c; c / x is c · x^-1.
            if (minus) {
                acc = emit(op, "subtract", {emit(op, "neg", {b}, {}, span)},
                           params{{"b", makeReal(-acc.asDouble())}}, span);
            } else {
                const double k = acc.asDouble();
                acc = emit(op, "pow", {b}, params{{"b", makeReal(-1.0)}}, span);
                if (k != 1.0) acc = emit(op, "gain", {acc}, params{{"amount", makeReal(k)}}, span);
            }
        } else {
            callContext pair;
            pair.callee = c.callee;
            pair.span = span;
            pair.positional = {acc, b};
            pair.positionalSpans = {c.positionalSpans[i - 1], c.positionalSpans[i]};
            acc = arithmetic(pair, minus ? '-' : '/');
        }
    }
    return acc;
}

// --- Sorting and validation ---------------------------------------------------------------

metadata::graphSpec evaluator::finish(std::vector<compileError>& errors, std::vector<compileError>& warnings) {
    // The file a node or message points into; the entry file when it has no source.
    const auto fileOf = [this](const std::optional<metadata::sourceSpan>& s) {
        return s ? s->file : files_.front()->path;
    };
    if (const auto cycle = ir::sortByDataFlow(graph_); !cycle.empty()) {
        std::string names;
        for (std::size_t i : cycle) {
            const auto& node = graph_.nodes[i];
            const sourceSpan at = fromIrSpan(node.src);
            names += (names.empty() ? "" : ", ") + languageName(node.type) + " (" + std::to_string(at.line) +
                     ':' + std::to_string(at.column) + ')';
        }
        errors.push_back({.message = "these nodes form a cycle: " + names +
                                     ". Close a loop through a feedback pipe: (pipe :audio :feedback #t)",
                          .file = fileOf(graph_.nodes[cycle.front()].src),
                          .span = fromIrSpan(graph_.nodes[cycle.front()].src),
                          .stack = {}});
        return {};
    }
    for (const auto& e : ir::validate(graph_))
        errors.push_back(
            {.message = e.message, .file = fileOf(e.src), .span = fromIrSpan(e.src), .stack = {}});
    if (errors.empty()) {
        for (const auto& w : ir::hazards(graph_, options_.sampleRate))
            warnings.push_back(
                {.message = w.message, .file = fileOf(w.src), .span = fromIrSpan(w.src), .stack = {}});
    }
    return std::move(graph_);
}

} // namespace interpreter::detail
