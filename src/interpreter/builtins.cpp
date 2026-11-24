// Builtin functions: arithmetic with dimensions (grammar.md §5.2),
// comparisons, the mapper operations on constants (primitives.md §7.9),
// tempo, and mapper.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/interpreter/lexer.h>
#include <espresso/interpreter/reader.h>
#include <espresso/interpreter/units.h>
#include <espresso/metadata/graph.h>
#include <espresso/metadata/primitiveSpec.h>

#include "evaluator.h"

namespace interpreter::detail {

using metadata::dimension;
using metadata::operands;

namespace {

bool anySymbolic(const std::vector<value>& args) {
    return std::ranges::any_of(args, [](const value& v) { return v.isSymbolic(); });
}

// Integer arithmetic that reports overflow instead of wrapping.
using limits = std::numeric_limits<std::int64_t>;

std::optional<std::int64_t> checkedAdd(std::int64_t a, std::int64_t b) {
    if ((b > 0 && a > limits::max() - b) || (b < 0 && a < limits::min() - b)) return std::nullopt;
    return a + b;
}

std::optional<std::int64_t> checkedSub(std::int64_t a, std::int64_t b) {
    if ((b < 0 && a > limits::max() + b) || (b > 0 && a < limits::min() + b)) return std::nullopt;
    return a - b;
}

std::optional<std::int64_t> checkedMul(std::int64_t a, std::int64_t b) {
    if (a > 0 ? (b > 0 ? a > limits::max() / b : b < limits::min() / a)
              : (b > 0 ? a < limits::min() / b : a != 0 && b < limits::max() / a)) {
        return std::nullopt;
    }
    return a * b;
}

} // namespace

value evaluator::arithmetic(callContext& c, char op) {
    auto& args = c.positional;
    if (anySymbolic(args)) return emitMath(c, std::nullopt);
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!args[i].isNumber()) fail(c.positionalSpans[i], std::string(1, op) + " expects numbers, got " + show(args[i]));
    }
    if (args.empty()) {
        if (op == '+') return makeInteger(0);
        if (op == '*') return makeInteger(1);
        fail(c.span, std::string(1, op) + " needs at least one argument");
    }
    if (args.size() == 1 && op == '-') {
        value v = args[0];
        if (v.k == kind::quantity && v.dim == dimension::level) {
            fail(c.positionalSpans[0], "negating a level is ambiguous; write the level in decibels, such as -6db");
        }
        if (v.k == kind::integer) {
            const auto negated = checkedSub(0, v.integer);
            if (!negated) fail(c.span, "integer overflow");
            v.integer = *negated;
        } else {
            v.number = -v.number;
        }
        return v;
    }
    if (args.size() == 1 && op == '/') {  // (/ x) is 1 / x
        args.insert(args.begin(), makeInteger(1));
        c.positionalSpans.insert(c.positionalSpans.begin(), c.span);
    }

    value acc = args[0];
    for (std::size_t i = 1; i < args.size(); ++i) {
        const value& b = args[i];
        const sourceSpan& at = c.positionalSpans[i];
        const bool aq = acc.k == kind::quantity, bq = b.k == kind::quantity;
        if (!aq && !bq) {
            if (acc.k == kind::integer && b.k == kind::integer && op != '/') {
                const auto r = op == '+'   ? checkedAdd(acc.integer, b.integer)
                               : op == '-' ? checkedSub(acc.integer, b.integer)
                                           : checkedMul(acc.integer, b.integer);
                if (!r) fail(at, "integer overflow");
                acc.integer = *r;
            } else {
                const double x = acc.asDouble(), y = b.asDouble();
                if (op == '/' && y == 0.0) fail(at, "division by zero");
                acc = makeReal(op == '+' ? x + y : op == '-' ? x - y : op == '*' ? x * y : x / y);
            }
            continue;
        }
        const double x = acc.asDouble(), y = b.asDouble();
        switch (op) {
        case '+':
        case '-':
            if (!aq || !bq || acc.dim != b.dim) {
                fail(at, std::string("cannot ") + (op == '+' ? "add " : "subtract ") + describeDim(b) +
                             (op == '+' ? " to " : " from ") + describeDim(acc));
            }
            // Adding decibels multiplies linear amplitude (grammar.md §5.2).
            if (acc.dim == dimension::level) {
                acc.number = op == '+' ? x * y : x / y;
            } else {
                acc.number = op == '+' ? x + y : x - y;
            }
            break;
        case '*':
            if (aq && bq) fail(at, "cannot multiply two quantities");
            acc = makeQuantity(x * y, aq ? acc.dim : b.dim);
            break;
        default:
            if (y == 0.0) fail(at, "division by zero");
            if (aq && !bq) {
                acc = makeQuantity(x / y, acc.dim);
            } else if (aq && bq) {
                if (acc.dim != b.dim) fail(at, "cannot divide " + describeDim(acc) + " by " + describeDim(b));
                acc = makeReal(x / y);
            } else if (b.dim == dimension::time || b.dim == dimension::frequency) {
                // 1 / time is a frequency and 1 / frequency is a time.
                acc = makeQuantity(x / y, b.dim == dimension::time ? dimension::frequency : dimension::time);
            } else {
                fail(at, "cannot divide a plain number by " + describeDim(b));
            }
        }
    }
    return acc;
}

value evaluator::compare(callContext& c, std::string_view op) {
    auto& args = c.positional;
    if (args.size() < 2) fail(c.span, std::string(op) + " needs two or more arguments");
    for (std::size_t i = 1; i < args.size(); ++i) {
        const value& a = args[i - 1];
        const value& b = args[i];
        if (op == "=" && (!a.isNumber() || !b.isNumber())) {
            bool same = a.k == b.k;
            if (same) {
                switch (a.k) {
                case kind::boolean: same = a.flag == b.flag; break;
                case kind::pipe: same = a.pipe == b.pipe; break;
                case kind::buffer: same = a.buffer == b.buffer; break;
                case kind::nil: break;
                default: same = a.text == b.text && a.obj == b.obj;  // a node's text is its ID
                }
            }
            if (!same) return makeBoolean(false);
            continue;
        }
        if (!a.isNumber() || !b.isNumber()) fail(c.positionalSpans[i], std::string(op) + " compares numbers");
        if ((a.k == kind::quantity) != (b.k == kind::quantity) || (a.k == kind::quantity && a.dim != b.dim)) {
            fail(c.positionalSpans[i], "cannot compare " + describeDim(a) + " with " + describeDim(b));
        }
        const double x = a.asDouble(), y = b.asDouble();
        const bool ok = op == "=" ? x == y : op == "<" ? x < y : op == ">" ? x > y : op == "<=" ? x <= y : x >= y;
        if (!ok) return makeBoolean(false);
    }
    return makeBoolean(true);
}

value evaluator::logicalNot(callContext& c) {
    if (c.positional.size() != 1) fail(c.span, "not takes one argument");
    return makeBoolean(!c.positional[0].truthy());
}

// (map f items): f applied to each item, in order. Every application has
// the same call site, so nodes built by f get occurrence indices 0, 1, 2,
// ... (interpreter.md §10.2.1).
value evaluator::map(callContext& c) {
    if (c.positional.size() != 2 || c.positional[1].k != kind::vector) fail(c.span, "expected (map function vector)");
    const auto items = std::static_pointer_cast<vectorObject>(c.positional[1].obj);
    std::vector<value> out;
    out.reserve(items->items.size());
    for (const auto& item : items->items) {
        callContext each;
        each.callee = "fn";
        each.span = c.span;
        each.positional.push_back(item);
        each.positionalSpans.push_back(c.positionalSpans[1]);
        out.push_back(apply(c.positional[0], each));
    }
    return makeVector(std::move(out));
}

// (nth vector i): item i, from 0. A real with an integer value is an
// index too.
value evaluator::nth(callContext& c) {
    if (c.positional.size() != 2 || c.positional[0].k != kind::vector)
        fail(c.span, "expected (nth vector index)");
    const auto index = c.positional[1].asInteger();
    if (!index) fail(c.positionalSpans[1], "nth: the index must be an integer, got ", show(c.positional[1]));
    const auto& items = static_cast<const vectorObject&>(*c.positional[0].obj).items;
    const std::int64_t i = *index;
    if (i < 0 || std::cmp_greater_equal(i, items.size()))
        fail(c.positionalSpans[1], "index ", std::to_string(i), " is outside a vector of ",
             std::to_string(items.size()), " items");
    return items[static_cast<std::size_t>(i)];
}

// (length vector): its number of items; nil has none.
value evaluator::length(callContext& c) {
    if (c.positional.size() != 1 || (c.positional[0].k != kind::vector && c.positional[0].k != kind::nil))
        fail(c.span, "expected (length vector)");
    if (c.positional[0].k == kind::nil) return makeInteger(0);
    return makeInteger(
        static_cast<std::int64_t>(static_cast<const vectorObject&>(*c.positional[0].obj).items.size()));
}

// (string-append s ...): the strings joined, in order; with none, the empty
// string (interpreter.md §4.1.4).
value evaluator::stringAppend(callContext& c) {
    std::string joined;
    for (std::size_t i = 0; i < c.positional.size(); ++i) {
        const value& v = c.positional[i];
        if (v.k != kind::string) fail(c.positionalSpans[i], "string-append expects strings, got ", show(v));
        joined += *v.text;
    }
    value out;
    out.k = kind::string;
    out.text = intern(joined);
    return out;
}

// (channel-count v): the channel count of pipe or buffer v (interpreter.md
// §4.1.2).
value evaluator::channelCount(callContext& c) {
    if (c.positional.size() != 1) fail(c.span, "expected (channel-count pipe-or-buffer)");
    const value& v = c.positional[0];
    if (v.k != kind::pipe && v.k != kind::buffer)
        fail(c.positionalSpans[0], "channel-count expects a pipe or a buffer, got ", show(v));
    return makeInteger(v.channels);
}

// (unit-value q 'unit): quantity q as a plain number in unit (grammar.md
// §5.5): (unit-value 250ms 's) is 0.25, (unit-value 0.5 'db) about -6.
value evaluator::unitValue(callContext& c) {
    if (c.positional.size() != 2 || c.positional[1].k != kind::symbol)
        fail(c.span, "expected (unit-value quantity 'unit), such as (unit-value 250ms 's)");
    const std::string& name = *c.positional[1].text;
    const auto unit = parseUnitSuffix(name);
    if (!unit) fail(c.positionalSpans[1], "unknown unit ", name, "; units are ", unitSuffixNames());
    const value& q = c.positional[0];
    const auto one = canonical({.value = 1.0, .unit = *unit}, options_.sampleRate);
    if (q.k != kind::quantity || q.dim != one.dim)
        fail(c.positionalSpans[0], "unit-value: ", name, " measures ", article(one.dim), ", got ",
             q.isNumber() ? describeDim(q) : show(q));
    if (*unit != unitSuffix::db) return makeReal(q.number / one.value);
    if (q.number <= 0.0) fail(c.positionalSpans[0], "unit-value: a level of 0 has no decibel value");
    return makeReal(decibels(q.number));
}

// (range n): the vector [0 1 ... n-1]. A real with an integer value is a
// count too.
value evaluator::range(callContext& c) {
    if (c.positional.size() != 1) fail(c.span, "expected (range n)");
    const auto n = c.positional[0].asInteger();
    if (!n || *n < 0 || *n > 100000)
        fail(c.positionalSpans[0], "range: n must be an integer from 0 to 100000, got ",
             show(c.positional[0]));
    std::vector<value> out;
    out.reserve(static_cast<std::size_t>(*n));
    for (std::int64_t i = 0; i < *n; ++i) out.push_back(makeInteger(i));
    return makeVector(std::move(out));
}

// The dimension rules of a mapper operation (metadata::operands), on
// constants and on the constant operands of a mapper; pipes match any
// dimension.
void evaluator::checkOperands(const metadata::primitiveSpec& spec, const callContext& c) const {
    const std::string op(c.callee);
    const auto& args = c.positional;
    const std::size_t count = spec.params.size() + 1;
    if (args.size() != count)
        fail(c.span, op + " takes " + std::to_string(count) + (count == 1 ? " argument" : " arguments"));
    // Operands [from, to) share one dimension.
    auto group = [&](std::size_t from, std::size_t to) {
        std::optional<std::size_t> first;
        for (std::size_t i = from; i < to; ++i) {
            if (args[i].isSymbolic()) continue;
            if (!first) {
                first = i;
                continue;
            }
            const value& a = args[*first];
            const value& b = args[i];
            if ((a.k == kind::quantity) != (b.k == kind::quantity) ||
                (b.k == kind::quantity && a.dim != b.dim)) {
                fail(c.positionalSpans[i],
                     op + " expects one dimension: got " + describeDim(b) + " after " + describeDim(a));
            }
        }
    };
    switch (spec.mapper) {
    case operands::none:
    case operands::keep: break;
    case operands::plain:
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (args[i].k == kind::quantity) {
                fail(c.positionalSpans[i], op + " expects a plain number, got " + describeDim(args[i]));
            }
        }
        break;
    case operands::sign:
        if (args[0].k == kind::quantity && args[0].dim == dimension::level) {
            fail(c.positionalSpans[0],
                 op + " of a level is ambiguous; write the level in decibels, such as -6db");
        }
        break;
    case operands::same: group(0, args.size()); break;
    case operands::ranges:
        group(0, 3);
        group(3, 5);
        break;
    }
}

// A mapper operation on constants. On pipes it becomes a generator.
value evaluator::math(const metadata::primitiveSpec& spec, callContext& c) {
    if (anySymbolic(c.positional)) return emitMath(c, std::cref(spec));
    const std::string op(c.callee);
    const auto& args = c.positional;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!args[i].isNumber()) fail(c.positionalSpans[i], op + " expects numbers, got " + show(args[i]));
    }
    checkOperands(spec, c);
    std::vector<double> x;
    x.reserve(args.size());
    for (const auto& a : args) x.push_back(a.asDouble());
    // Results keep the dimension of the value they come from.
    auto like = [&](const value& from, double r) { return from.k == kind::quantity ? makeQuantity(r, from.dim) : makeReal(r); };
    const value& a = args[0];
    const std::string_view type = spec.type;
    if (type == "neg") return like(a, -x[0]);
    if (type == "abs") return like(a, std::abs(x[0]));
    if (type == "floor") return like(a, std::floor(x[0]));
    if (type == "round") return like(a, std::round(x[0]));
    if (type == "exp") return makeReal(std::exp(x[0]));
    if (type == "log") {
        if (x[0] <= 0.0) fail(c.positionalSpans[0], "log of a number that is not positive");
        return makeReal(std::log(x[0]));
    }
    if (type == "sin") return makeReal(std::sin(x[0]));
    if (type == "cos") return makeReal(std::cos(x[0]));
    if (type == "tanh") return makeReal(std::tanh(x[0]));
    if (type == "pow") return makeReal(std::pow(x[0], x[1]));
    if (type == "min") return x[0] <= x[1] ? a : args[1];
    if (type == "max") return x[0] >= x[1] ? a : args[1];
    if (type == "quantize") return like(a, x[1] > 0.0 ? std::round(x[0] / x[1]) * x[1] : x[0]);
    if (type == "clamp") return like(a, std::clamp(x[0], std::min(x[1], x[2]), std::max(x[1], x[2])));
    const double span = x[2] - x[1];
    const double t = span == 0.0 ? 0.0 : (x[0] - x[1]) / span;
    const value& out = args[3];
    if (type == "scale" || x[3] == 0.0 || x[4] == 0.0 || (x[3] < 0.0) != (x[4] < 0.0)) {
        return like(out, x[3] + t * (x[4] - x[3]));
    }
    return like(out, x[3] * std::pow(x[4] / x[3], t));
}

value evaluator::tempo(callContext& c) {
    if (c.positional.size() != 1) fail(c.span, "tempo takes one argument: (tempo 120bpm)");
    const value& v = c.positional[0];
    double bpm = 0.0;
    if (v.k == kind::quantity && v.dim == dimension::frequency) {
        bpm = v.number * 60.0;
    } else if (v.isPlainNumber()) {
        bpm = v.asDouble();
    } else {
        fail(c.positionalSpans[0], "tempo expects beats per minute, such as 120bpm, got " + show(v));
    }
    if (!(bpm > 0.0) || !std::isfinite(bpm)) fail(c.positionalSpans[0], "tempo must be a positive number");
    graph_.tempo = bpm;
    return v;
}

value evaluator::mapper(callContext& c) {
    if (c.positional.size() != 2) fail(c.span, "expected (mapper pipe (fn (x) expression))");
    const value& source = c.positional[0];
    const value& fn = c.positional[1];
    if (!source.isSymbolic()) fail(c.positionalSpans[0], "mapper expects a pipe, got " + show(source));
    if (source.k == kind::pipe && source.pipe.kind != metadata::pipeKind::audio &&
        source.pipe.kind != metadata::pipeKind::control) {
        fail(c.positionalSpans[0], "mapper expects an audio or control pipe, got " + show(source));
    }
    if (fn.k != kind::closure) fail(c.positionalSpans[1], "mapper expects a function of one argument");
    auto f = std::static_pointer_cast<closureObject>(fn.obj);
    if (f->required.size() != 1 || !f->optional.empty() || f->rest || !f->keys.empty()) {
        fail(c.positionalSpans[1], "a mapper function takes exactly one argument");
    }
    auto m = make<mappedObject>();
    m->source = source;
    m->fn = std::move(f);
    m->span = c.span;
    // The mapper's step is taken now, so its nodes' IDs follow the order
    // in which mappers are made, not the order in which they are used.
    m->path = path_;
    if (building_) m->path.push_back(nextStep("mapper", c.span, c.key));
    value v;
    v.k = kind::mapped;
    v.obj = m;
    return v;
}

} // namespace interpreter::detail
