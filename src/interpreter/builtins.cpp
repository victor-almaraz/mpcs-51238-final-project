// Builtin functions: arithmetic with dimensions (grammar.md §5.2),
// comparisons, the mapper operations on constants (primitives.md §7.9),
// tempo, and mapper.

#include "evaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace interpreter::detail {

using metadata::dimension;

namespace {

value boolean(bool b) {
    value v;
    v.k = kind::boolean;
    v.flag = b;
    return v;
}

bool symbolic(const value& v) { return v.k == kind::pipe || v.k == kind::mapped; }

bool anySymbolic(const std::vector<value>& args) { return std::any_of(args.begin(), args.end(), symbolic); }

std::string describeDim(const value& v) { return v.k == kind::quantity ? article(v.dim) : "a plain number"; }

} // namespace

value evaluator::arithmetic(callContext& c, char op) {
    auto& args = c.positional;
    if (anySymbolic(args)) return emitMath(std::string(1, op), args, c.span);
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!args[i].isNumber()) fail(c.positionalSpans[i], std::string(1, op) + " expects numbers, got " + show(args[i]));
    }
    if (args.empty()) {
        if (op == '+') return value{kind::integer};
        if (op == '*') {
            value one{kind::integer};
            one.integer = 1;
            return one;
        }
        fail(c.span, std::string(1, op) + " needs at least one argument");
    }
    if (args.size() == 1 && op == '-') {
        value v = args[0];
        if (v.k == kind::quantity && v.dim == dimension::level) {
            fail(c.positionalSpans[0], "negating a level is ambiguous; write the level in decibels, such as -6db");
        }
        if (v.k == kind::integer) {
            if (v.integer == std::numeric_limits<std::int64_t>::min()) fail(c.span, "integer overflow");
            v.integer = -v.integer;
        } else {
            v.number = -v.number;
        }
        return v;
    }
    if (args.size() == 1 && op == '/') {  // (/ x) is 1 / x
        value one{kind::integer};
        one.integer = 1;
        args.insert(args.begin(), one);
        c.positionalSpans.insert(c.positionalSpans.begin(), c.span);
    }

    value acc = args[0];
    for (std::size_t i = 1; i < args.size(); ++i) {
        const value& b = args[i];
        const sourceSpan& at = c.positionalSpans[i];
        const bool aq = acc.k == kind::quantity, bq = b.k == kind::quantity;
        if (!aq && !bq) {
            if (acc.k == kind::integer && b.k == kind::integer && op != '/') {
                std::int64_t r = 0;
                const bool overflow = op == '+'   ? __builtin_add_overflow(acc.integer, b.integer, &r)
                                      : op == '-' ? __builtin_sub_overflow(acc.integer, b.integer, &r)
                                                  : __builtin_mul_overflow(acc.integer, b.integer, &r);
                if (overflow) fail(at, "integer overflow");
                acc.integer = r;
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
                case kind::node: same = a.node == b.node; break;
                case kind::nil: break;
                default: same = a.text == b.text && a.obj == b.obj;
                }
            }
            if (!same) return boolean(false);
            continue;
        }
        if (!a.isNumber() || !b.isNumber()) fail(c.positionalSpans[i], std::string(op) + " compares numbers");
        if ((a.k == kind::quantity) != (b.k == kind::quantity) || (a.k == kind::quantity && a.dim != b.dim)) {
            fail(c.positionalSpans[i], "cannot compare " + describeDim(a) + " with " + describeDim(b));
        }
        const double x = a.asDouble(), y = b.asDouble();
        const bool ok = op == "=" ? x == y : op == "<" ? x < y : op == ">" ? x > y : op == "<=" ? x <= y : x >= y;
        if (!ok) return boolean(false);
    }
    return boolean(true);
}

value evaluator::logicalNot(callContext& c) {
    if (c.positional.size() != 1) fail(c.span, "not takes one argument");
    return boolean(!c.positional[0].truthy());
}

value evaluator::list(callContext& c) {
    auto* v = make<vectorObject>();
    v->items = c.positional;
    value out;
    out.k = kind::vector;
    out.obj = v;
    return out;
}

// (map f items): f applied to each item, in order. Every application has
// the same call site, so nodes built by f get occurrence indices 0, 1, 2,
// ... (interpreter.md §10.2.1).
value evaluator::map(callContext& c) {
    if (c.positional.size() != 2 || c.positional[1].k != kind::vector) fail(c.span, "expected (map function vector)");
    auto* out = make<vectorObject>();
    for (const auto& item : static_cast<vectorObject*>(c.positional[1].obj)->items) {
        callContext each;
        each.callee = "fn";
        each.span = c.span;
        each.positional.push_back(item);
        each.positionalSpans.push_back(c.positionalSpans[1]);
        out->items.push_back(apply(c.positional[0], each));
    }
    value v;
    v.k = kind::vector;
    v.obj = out;
    return v;
}

// (range n): the vector [0 1 ... n-1].
value evaluator::range(callContext& c) {
    if (c.positional.size() != 1 || c.positional[0].k != kind::integer || c.positional[0].integer < 0 ||
        c.positional[0].integer > 100000) {
        fail(c.span, "expected (range n) with an integer n from 0 to 100000");
    }
    auto* out = make<vectorObject>();
    for (std::int64_t i = 0; i < c.positional[0].integer; ++i) {
        value v{kind::integer};
        v.integer = i;
        out->items.push_back(v);
    }
    value v;
    v.k = kind::vector;
    v.obj = out;
    return v;
}

// The mapper operations on constants. On pipes they become generators.
value evaluator::math(callContext& c) {
    const std::string op(c.callee);
    auto& args = c.positional;
    if (anySymbolic(args)) return emitMath(op, args, c.span);
    struct arity {
        std::string_view name;
        std::size_t count;
    };
    constexpr arity arities[] = {{"neg", 1},      {"abs", 1},   {"exp", 1},   {"log", 1},   {"sin", 1},
                                 {"cos", 1},      {"tanh", 1},  {"floor", 1}, {"round", 1}, {"pow", 2},
                                 {"min", 2},      {"max", 2},   {"quantize", 2}, {"clamp", 3}, {"scale", 5},
                                 {"scale-exp", 5}};
    const auto it = std::find_if(std::begin(arities), std::end(arities), [&](const arity& a) { return a.name == op; });
    if (args.size() != it->count) {
        fail(c.span, op + " takes " + std::to_string(it->count) + " argument" + (it->count == 1 ? "" : "s"));
    }
    std::vector<double> x;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!args[i].isNumber()) fail(c.positionalSpans[i], op + " expects numbers, got " + show(args[i]));
        x.push_back(args[i].asDouble());
    }
    // Results keep the dimension of the value they come from.
    auto like = [&](const value& from, double r) { return from.k == kind::quantity ? makeQuantity(r, from.dim) : makeReal(r); };
    const value& a = args[0];
    if (op == "neg") return like(a, -x[0]);
    if (op == "abs") return like(a, std::abs(x[0]));
    if (op == "floor") return like(a, std::floor(x[0]));
    if (op == "round") return like(a, std::round(x[0]));
    if (op == "exp") return makeReal(std::exp(x[0]));
    if (op == "log") {
        if (x[0] <= 0.0) fail(c.positionalSpans[0], "log of a number that is not positive");
        return makeReal(std::log(x[0]));
    }
    if (op == "sin") return makeReal(std::sin(x[0]));
    if (op == "cos") return makeReal(std::cos(x[0]));
    if (op == "tanh") return makeReal(std::tanh(x[0]));
    if (op == "pow") return makeReal(std::pow(x[0], x[1]));
    if (op == "min") return x[0] <= x[1] ? a : args[1];
    if (op == "max") return x[0] >= x[1] ? a : args[1];
    if (op == "quantize") return like(a, x[1] > 0.0 ? std::round(x[0] / x[1]) * x[1] : x[0]);
    if (op == "clamp") return like(a, std::clamp(x[0], std::min(x[1], x[2]), std::max(x[1], x[2])));
    const double span = x[2] - x[1];
    const double t = span == 0.0 ? 0.0 : (x[0] - x[1]) / span;
    const value& out = args[3];
    if (op == "scale" || x[3] == 0.0 || x[4] == 0.0 || (x[3] < 0.0) != (x[4] < 0.0)) {
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
    } else if (v.k == kind::integer || v.k == kind::real) {
        bpm = v.asDouble();
    } else {
        fail(c.positionalSpans[0], "tempo expects beats per minute, such as 120bpm, got " + show(v));
    }
    if (!(bpm > 0.0)) fail(c.positionalSpans[0], "tempo must be positive");
    graph_.tempo = bpm;
    return v;
}

value evaluator::mapper(callContext& c) {
    if (c.positional.size() != 2) fail(c.span, "expected (mapper pipe (fn (x) expression))");
    const value& source = c.positional[0];
    const value& fn = c.positional[1];
    if (source.k != kind::pipe && source.k != kind::mapped) fail(c.positionalSpans[0], "mapper expects a pipe, got " + show(source));
    if (fn.k != kind::closure) fail(c.positionalSpans[1], "mapper expects a function of one argument");
    auto* f = static_cast<closureObject*>(fn.obj);
    if (f->required.size() != 1 || !f->optional.empty() || f->rest || !f->keys.empty()) {
        fail(c.positionalSpans[1], "a mapper function takes exactly one argument");
    }
    auto* m = make<mappedObject>();
    m->source = source;
    m->fn = f;
    m->span = c.span;
    // The mapper's step is taken now, so its nodes' IDs follow the order
    // in which mappers are made, not the order in which they are used.
    m->path = steps_;
    if (building_) m->path.push_back(nextStep("mapper", c.span, c.key));
    value v;
    v.k = kind::mapped;
    v.obj = m;
    return v;
}

} // namespace interpreter::detail
