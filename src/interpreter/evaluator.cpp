// The evaluator: special forms, calls, closures, and units
// (interpreter.md §4, §5, §8).

#include "evaluator.h"

#include <proj/interpreter/units.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace interpreter {

using detail::evaluator;

std::string compileError::text() const {
    std::ostringstream out;
    out << file << ':' << span.line << ':' << span.column << ": " << message;
    return out.str();
}

std::string compileResult::text() const {
    std::string out;
    for (const auto& e : errors) {
        if (!out.empty()) out += '\n';
        out += e.text();
    }
    return out;
}

std::string languageName(std::string_view type) {
    std::string out;
    for (char c : type) {
        if (std::isupper(static_cast<unsigned char>(c))) {
            out += '-';
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            out += c;
        }
    }
    return out;
}

canonicalQuantity canonical(const quantityLiteral& q, double sampleRate) noexcept {
    using metadata::dimension;
    switch (q.unit) {
    case unitSuffix::s: return {q.value, dimension::time};
    case unitSuffix::ms: return {q.value * 1e-3, dimension::time};
    case unitSuffix::us: return {q.value * 1e-6, dimension::time};
    case unitSuffix::smp: return {q.value / sampleRate, dimension::time};
    case unitSuffix::beat: return {q.value, dimension::beats};
    case unitSuffix::bar: return {q.value * 4.0, dimension::beats};  // 4/4 (primitives.md §7.5.3)
    case unitSuffix::hz: return {q.value, dimension::frequency};
    case unitSuffix::khz: return {q.value * 1e3, dimension::frequency};
    case unitSuffix::bpm: return {q.value / 60.0, dimension::frequency};
    case unitSuffix::db: return {std::pow(10.0, q.value / 20.0), dimension::level};
    case unitSuffix::st: return {q.value, dimension::interval};
    case unitSuffix::ct: return {q.value * 0.01, dimension::interval};
    }
    return {q.value, dimension::number};
}

compileResult compile(std::string_view source, std::string_view file, const compileOptions& options) {
    evaluator e{std::string(file), options};
    return e.run(source);
}

namespace detail {

namespace {

constexpr std::size_t maxDepth = 2000;

using metadata::dimension;

} // namespace

evaluator::evaluator(std::string file, compileOptions options) : file_(std::move(file)), options_(std::move(options)) {
    global_ = make<environment>();
    nextIndex_ = {{metadata::pipeKind::audio, 0},
                  {metadata::pipeKind::control, metadata::tempoPipe + 1},
                  {metadata::pipeKind::event, 0}};
    installBuiltins();
}

void evaluator::fail(const sourceSpan& span, std::string message) const {
    throw failure{std::move(message), span, stack_};
}

interned evaluator::intern(std::string_view s) {
    auto& slot = names_[std::string(s)];
    if (!slot) slot = std::make_shared<const std::string>(s);
    return slot;
}

// Clears every object's references, breaking closure-environment cycles,
// so the objects are freed with the last references below.
evaluator::~evaluator() {
    for (const auto& o : objects_) o->clear();
}

compileResult evaluator::run(std::string_view source) {
    compileResult result;
    const auto read = interpreter::read(source);
    if (read.error) {
        result.errors.push_back({read.error->message, file_, read.error->span, {}});
        return result;
    }
    try {
        for (const auto& form : read.data) eval(form, global_);
        const auto it = global_->vars.find(intern(options_.entry));
        std::shared_ptr<closureObject> piece;
        if (it != global_->vars.end() && it->second.k == kind::closure) {
            piece = std::static_pointer_cast<closureObject>(it->second.obj);
        }
        if (!piece || !piece->piece) {
            fail(read.data.empty() ? sourceSpan{} : read.data.back().span,
                 "no piece named " + options_.entry + "; define it with (defpiece " + options_.entry + " () ...)");
        }
        building_ = true;
        root_ = piece->name;
        graph_.seed = options_.seed;
        callContext c;
        c.callee = piece->name;
        c.span = piece->span;
        applyClosure(*piece, c, false);
        building_ = false;
        auto graph = finish(result.errors, result.warnings);
        if (result.errors.empty()) result.graph = std::move(graph);
    } catch (const failure& f) {
        result.errors.push_back({f.message, file_, f.span, f.stack});
    }
    return result;
}

std::string evaluator::show(const value& v) const {
    std::ostringstream out;
    switch (v.k) {
    case kind::nil: return "nil";
    case kind::boolean: return v.flag ? "#t" : "#f";
    case kind::integer: out << v.integer; break;
    case kind::real: out << v.number; break;
    case kind::quantity: out << v.number << " (" << metadata::toString(v.dim) << ")"; break;
    case kind::symbol: return *v.text;
    case kind::keyword: return ":" + *v.text;
    case kind::string: return '"' + *v.text + '"';
    case kind::vector: return "a vector";
    case kind::closure: return "the function " + static_cast<const closureObject&>(*v.obj).name;
    case kind::builtin: return "the builtin " + static_cast<const builtinObject&>(*v.obj).name;
    case kind::pipe: out << metadata::toString(v.pipe.kind) << " pipe " << v.pipe.index; break;
    case kind::node: return "a node";
    case kind::mapped: return "a mapped pipe";
    }
    return out.str();
}

// --- Units (§8) ---------------------------------------------------------------

value evaluator::quantity(const quantityLiteral& q, const sourceSpan&) const {
    const auto c = canonical(q, options_.sampleRate);
    return makeQuantity(c.value, c.dim);
}

// --- Evaluation ---------------------------------------------------------------

value evaluator::quoted(const datum& d) {
    value v;
    switch (d.kind) {
    case datumKind::symbol: v.k = kind::symbol, v.text = intern(d.text); return v;
    case datumKind::list:
        if (d.items.empty()) return v;  // '() is nil, like ()
        [[fallthrough]];
    case datumKind::vector: {
        auto vec = make<vectorObject>();
        for (const auto& item : d.items) vec->items.push_back(quoted(item));
        v.k = kind::vector, v.obj = vec;
        return v;
    }
    default: return eval(d, global_);
    }
}

value evaluator::eval(const datum& d, const std::shared_ptr<environment>& env) {
    value v;
    switch (d.kind) {
    case datumKind::nil: return v;
    case datumKind::boolean: v.k = kind::boolean, v.flag = d.boolean; return v;
    case datumKind::integer: v.k = kind::integer, v.integer = d.integer; return v;
    case datumKind::floating: v.k = kind::real, v.number = d.floating; return v;
    case datumKind::quantity: return quantity(d.quantity, d.span);
    case datumKind::string: v.k = kind::string, v.text = intern(d.text); return v;
    case datumKind::keyword: v.k = kind::keyword, v.text = intern(d.text); return v;
    case datumKind::symbol: {
        const interned name = intern(d.text);
        for (auto e = env; e; e = e->parent) {
            if (const auto it = e->vars.find(name); it != e->vars.end()) return it->second;
        }
        fail(d.span, "unbound symbol " + d.text);
    }
    case datumKind::vector: {
        auto vec = make<vectorObject>();
        for (const auto& item : d.items) vec->items.push_back(eval(item, env));
        v.k = kind::vector, v.obj = vec;
        return v;
    }
    case datumKind::list: return evalList(d, env);
    }
    return v;
}

std::shared_ptr<closureObject> evaluator::makeClosure(std::string name, const datum& params, std::span<const datum> body,
                                      const std::shared_ptr<environment>& env, const sourceSpan& span) {
    if (params.kind != datumKind::list) fail(params.span, "expected a parameter list");
    if (body.empty()) fail(span, name + " has an empty body");
    auto f = make<closureObject>();
    f->name = std::move(name);
    f->body = body;
    f->env = env;
    f->span = span;
    enum class mode { required, optional, rest, key } m = mode::required;
    for (const auto& p : params.items) {
        if (p.kind != datumKind::symbol) fail(p.span, "parameters are symbols");
        if (p.text == "&optional") m = mode::optional;
        else if (p.text == "&rest") m = mode::rest;
        else if (p.text == "&key") m = mode::key;
        else if (m == mode::required) f->required.push_back(intern(p.text));
        else if (m == mode::optional) f->optional.push_back(intern(p.text));
        else if (m == mode::key) f->keys.push_back(intern(p.text));
        else if (!f->rest) f->rest = intern(p.text);
        else fail(p.span, "&rest takes one parameter");
    }
    return f;
}

value evaluator::defineForm(const datum& d, const std::shared_ptr<environment>& env, bool piece) {
    const auto& items = d.items;
    const std::string_view form = piece ? "defpiece" : "define";
    if (items.size() < 3) fail(d.span, std::string(form) + " needs a name and a value");
    value v;
    interned name;
    if (piece) {
        if (items[1].kind != datumKind::symbol || items.size() < 4) fail(d.span, "expected (defpiece name (params) body...)");
        name = intern(items[1].text);
        auto f = makeClosure(items[1].text, items[2], std::span(items).subspan(3), env, d.span);
        f->piece = true;
        v.k = kind::closure, v.obj = f;
    } else if (items[1].kind == datumKind::list) {
        const auto& head = items[1];
        if (head.items.empty() || head.items[0].kind != datumKind::symbol) fail(head.span, "expected (define (name params...) body...)");
        name = intern(head.items[0].text);
        datum params = head;  // the parameters are read now, not kept
        params.items.erase(params.items.begin());
        auto f = makeClosure(head.items[0].text, params, std::span(items).subspan(2), env, d.span);
        v.k = kind::closure, v.obj = f;
    } else if (items[1].kind == datumKind::symbol && items.size() == 3) {
        name = intern(items[1].text);
        v = eval(items[2], env);
        // (define v (fn ...)) names the function, so its calls are steps
        // of the instance path and honor :key.
        if (v.k == kind::closure) {
            auto& f = static_cast<closureObject&>(*v.obj);
            if (f.name == "fn") f.name = items[1].text;
        }
    } else {
        fail(d.span, "expected (define name value) or (define (name params...) body...)");
    }
    env->vars[name] = v;
    return v;
}

value evaluator::evalList(const datum& d, const std::shared_ptr<environment>& env) {
    const auto& items = d.items;
    if (items.empty()) return {};
    const datum& head = items[0];
    if (head.kind == datumKind::symbol) {
        const std::string& s = head.text;
        if (s == "quote") {
            if (items.size() != 2) fail(d.span, "quote takes one datum");
            return quoted(items[1]);
        }
        if (s == "if") {
            if (items.size() < 3 || items.size() > 4) fail(d.span, "expected (if test then [else])");
            if (eval(items[1], env).truthy()) return eval(items[2], env);
            return items.size() == 4 ? eval(items[3], env) : value{};
        }
        if (s == "cond") {
            for (std::size_t i = 1; i < items.size(); ++i) {
                const auto& clause = items[i];
                if (clause.kind != datumKind::list || clause.items.empty()) fail(clause.span, "expected (test body...)");
                if (clause.items[0].isSymbol("else") || eval(clause.items[0], env).truthy()) {
                    value v;
                    for (std::size_t j = 1; j < clause.items.size(); ++j) v = eval(clause.items[j], env);
                    return v;
                }
            }
            return {};
        }
        if (s == "let") {
            if (items.size() < 3 || items[1].kind != datumKind::list) fail(d.span, "expected (let ((name value)...) body...)");
            auto frame = make<environment>();
            frame->parent = env;
            // Bindings are sequential: each may refer to the ones before it.
            for (const auto& b : items[1].items) {
                if (b.kind != datumKind::list || b.items.size() != 2 || b.items[0].kind != datumKind::symbol) {
                    fail(b.span, "expected (name value)");
                }
                frame->vars[intern(b.items[0].text)] = eval(b.items[1], frame);
            }
            value v;
            for (std::size_t i = 2; i < items.size(); ++i) v = eval(items[i], frame);
            return v;
        }
        if (s == "fn") {
            if (items.size() < 3) fail(d.span, "expected (fn (params...) body...)");
            value v;
            v.k = kind::closure;
            v.obj = makeClosure("fn", items[1], std::span(items).subspan(2), env, d.span);
            return v;
        }
        if (s == "do") {
            value v;
            for (std::size_t i = 1; i < items.size(); ++i) v = eval(items[i], env);
            return v;
        }
        if (s == "set!") {
            if (items.size() != 3 || items[1].kind != datumKind::symbol) fail(d.span, "expected (set! name value)");
            const interned name = intern(items[1].text);
            // Evaluate first: the value's evaluation may add bindings and
            // invalidate iterators into the frame.
            const value v = eval(items[2], env);
            for (auto e = env; e; e = e->parent) {
                if (const auto it = e->vars.find(name); it != e->vars.end()) return it->second = v;
            }
            fail(items[1].span, "unbound symbol " + items[1].text);
        }
        if (s == "define") return defineForm(d, env, false);
        if (s == "defpiece") return defineForm(d, env, true);
        if (s == "module") return {};
        if (s == "pipe") return makePipe(d, env);
        if (s == "import") fail(d.span, "import is not supported yet; keep the project in one file");
        if (s == "defclass" || s == "defgeneric" || s == "defmethod" || s == "make") {
            fail(d.span, s + " is not supported yet");
        }
    }

    const value f = eval(head, env);
    callContext c;
    c.callee = head.kind == datumKind::symbol ? std::string_view(head.text) : std::string_view("fn");
    c.span = d.span;
    for (std::size_t i = 1; i < items.size(); ++i) {
        const datum& a = items[i];
        if (a.kind == datumKind::keyword) {
            if (i + 1 >= items.size()) fail(a.span, "keyword :" + a.text + " needs a value");
            value arg = eval(items[i + 1], env);
            if (a.text == "key") {  // reserved on every call (§10.2.2)
                c.key = arg.k == kind::symbol || arg.k == kind::string || arg.k == kind::keyword ? *arg.text : show(arg);
            } else {
                c.keywords.emplace_back(intern(a.text), arg);
                c.keywordSpans.push_back(a.span);
            }
            ++i;
        } else {
            if (!c.keywords.empty()) fail(a.span, "positional arguments come before keyword arguments");
            c.positional.push_back(eval(a, env));
            c.positionalSpans.push_back(a.span);
        }
    }
    return apply(f, c);
}

value evaluator::apply(const value& f, callContext& c) {
    if (f.k == kind::closure) return applyClosure(static_cast<closureObject&>(*f.obj), c, true);
    if (f.k != kind::builtin) fail(c.span, show(f) + " is not a function");
    auto& b = static_cast<builtinObject&>(*f.obj);
    c.callee = b.name;
    // A builtin with both forms is a primitive when called with keywords.
    if (b.primitive && (!b.math || !c.keywords.empty())) return callPrimitive(*b.primitive, c);
    if (!c.keywords.empty()) fail(c.keywordSpans[0], b.name + " takes no keyword arguments");
    // :key names the nodes a call makes: mapper's, and math on pipes.
    const bool makesNodes = b.name == "mapper" || std::any_of(c.positional.begin(), c.positional.end(), [](const value& v) {
                                return v.k == kind::pipe || v.k == kind::mapped;
                            });
    if (c.key && !makesNodes) fail(c.span, ":key names a node or a function call; " + b.name + " makes neither");
    return b.math(*this, c);
}

// Restores the instance path after a call.
struct stepGuard {
    evaluator& e;
    std::size_t depth;
    ~stepGuard() {
        e.steps_.resize(depth);
        e.stack_.pop_back();
    }
};

value evaluator::applyClosure(closureObject& f, callContext& c, bool pushStep) {
    if (stack_.size() >= maxDepth) fail(c.span, "calls nested too deeply in " + f.name);
    auto frame = make<environment>();
    frame->parent = f.env;
    const std::size_t n = c.positional.size();
    const std::size_t most = f.required.size() + f.optional.size();
    if (n < f.required.size() || (n > most && !f.rest)) {
        fail(c.span, f.name + " expects " + std::to_string(f.required.size()) +
                         (f.optional.empty() && !f.rest ? "" : " or more") + " arguments, got " + std::to_string(n));
    }
    for (std::size_t i = 0; i < f.required.size(); ++i) frame->vars[f.required[i]] = c.positional[i];
    for (std::size_t i = 0; i < f.optional.size(); ++i) {
        const std::size_t at = f.required.size() + i;
        frame->vars[f.optional[i]] = at < n ? c.positional[at] : value{};
    }
    if (f.rest) {
        auto rest = make<vectorObject>();
        for (std::size_t i = most; i < n; ++i) rest->items.push_back(c.positional[i]);
        value v;
        v.k = kind::vector, v.obj = rest;
        frame->vars[f.rest] = v;
    }
    for (const auto& k : f.keys) frame->vars[k] = {};
    for (std::size_t i = 0; i < c.keywords.size(); ++i) {
        const auto& [name, v] = c.keywords[i];
        if (std::find(f.keys.begin(), f.keys.end(), name) == f.keys.end()) {
            fail(c.keywordSpans[i], f.name + " has no keyword :" + *name);
        }
        frame->vars[name] = v;
    }

    stack_.push_back(c.span);
    const stepGuard guard{*this, steps_.size()};
    // Anonymous closures add no step: the occurrence index or :key of the
    // first named call inside identifies each application, so keys keep
    // IDs stable when the items of a map are reordered (§10.2.2).
    if (pushStep && building_ && (f.name != "fn" || c.key)) steps_.push_back(nextStep(f.name, c.span, c.key));
    value v;
    for (const auto& form : f.body) v = eval(form, frame);
    return v;
}

void evaluator::installBuiltins() {
    auto bind = [&](std::string_view name, builtinObject::fn math, metadata::optionalRef<metadata::primitiveSpec> spec) {
        auto b = make<builtinObject>();
        b->name = std::string(name);
        b->math = math;
        b->primitive = spec;
        value v;
        v.k = kind::builtin, v.obj = b;
        global_->vars[intern(name)] = v;
    };
    // Mapper operations that are also primitives (primitives.md §7.9).
    constexpr std::string_view dual[] = {"neg", "abs",  "exp",   "log",      "sin",   "cos",       "tanh", "floor",
                                         "round", "pow", "min", "max", "quantize", "clamp", "scale-exp", "scale"};
    for (const auto& spec : metadata::primitiveSpecs()) {
        const std::string name = languageName(spec.type);
        const bool isMath = std::find(std::begin(dual), std::end(dual), name) != std::end(dual);
        bind(name, isMath ? builtinObject::fn(&evaluator::math) : builtinObject::fn(), std::cref(spec));
    }
    bind("+", &evaluator::add, std::nullopt);
    bind("-", &evaluator::subtract, std::nullopt);
    bind("*", &evaluator::multiply, std::nullopt);
    bind("/", &evaluator::divide, std::nullopt);
    bind("=", &evaluator::equal, std::nullopt);
    bind("<", &evaluator::less, std::nullopt);
    bind(">", &evaluator::greater, std::nullopt);
    bind("<=", &evaluator::lessEqual, std::nullopt);
    bind(">=", &evaluator::greaterEqual, std::nullopt);
    bind("not", &evaluator::logicalNot, std::nullopt);
    bind("list", &evaluator::list, std::nullopt);
    bind("map", &evaluator::map, std::nullopt);
    bind("range", &evaluator::range, std::nullopt);
    bind("tempo", &evaluator::tempo, std::nullopt);
    bind("mapper", &evaluator::mapper, std::nullopt);
}

} // namespace detail

} // namespace interpreter
