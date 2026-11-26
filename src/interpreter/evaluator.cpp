// The evaluator: special forms, calls, closures, and units
// (interpreter.md §4, §5, §8).

#include "evaluator.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/interpreter/lexer.h>
#include <espresso/interpreter/reader.h>
#include <espresso/interpreter/units.h>
#include <espresso/metadata/graph.h>
#include <espresso/metadata/primitiveSpec.h>
#include <espresso/metadata/text.h>

namespace interpreter {

using detail::evaluator;

std::string compileError::text() const {
    std::ostringstream out;
    out << file << ':' << span.line << ':' << span.column << ": " << message;
    return out.str();
}

std::string compileResult::text() const { return metadata::joinLines(errors); }

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
    case unitSuffix::s: return {.value = q.value, .dim = dimension::time};
    case unitSuffix::ms: return {.value = q.value * 1e-3, .dim = dimension::time};
    case unitSuffix::us: return {.value = q.value * 1e-6, .dim = dimension::time};
    case unitSuffix::smp: return {.value = q.value / sampleRate, .dim = dimension::time};
    case unitSuffix::beat: return {.value = q.value, .dim = dimension::beats};
    case unitSuffix::bar:
        return {.value = q.value * 4.0, .dim = dimension::beats};  // 4/4 (primitives.md §7.5.3)
    case unitSuffix::hz: return {.value = q.value, .dim = dimension::frequency};
    case unitSuffix::khz: return {.value = q.value * 1e3, .dim = dimension::frequency};
    case unitSuffix::bpm: return {.value = q.value / 60.0, .dim = dimension::frequency};
    case unitSuffix::db: return {.value = std::pow(10.0, q.value / 20.0), .dim = dimension::level};
    case unitSuffix::st: return {.value = q.value, .dim = dimension::interval};
    case unitSuffix::ct: return {.value = q.value * 0.01, .dim = dimension::interval};
    }
    return {.value = q.value, .dim = dimension::number};
}

double decibels(double amplitude) noexcept { return 20.0 * std::log10(amplitude); }

compileResult compile(std::string_view source, std::string_view file, const compileOptions& options) {
    return evaluator::compile(source, std::string(file), options);
}

std::vector<std::string> builtinNames() { return detail::evaluator::builtinNames(); }

namespace detail {

namespace {

using metadata::dimension;

} // namespace

depthGuard::depthGuard(evaluator& e, const sourceSpan& span) : e_(e) {
    if (e_.depth_ >= maxEvalDepth) {
        e_.fail(span, "forms and calls are nested too deeply (more than " + std::to_string(maxEvalDepth) +
                          " levels)");
    }
    if (++e_.steps_ > e_.options_.maxSteps) {
        e_.fail(span, "the program did not finish within " + std::to_string(e_.options_.maxSteps) +
                          " evaluation steps; check for a loop or recursion that does not end");
    }
    ++e_.depth_;
}

depthGuard::~depthGuard() { --e_.depth_; }

compileResult evaluator::compile(std::string_view source, const std::string& file, compileOptions options) {
    evaluator e(source, file, std::move(options));
    return e.run();
}

std::vector<std::string> evaluator::builtinNames() {
    const evaluator e({}, {}, {});
    std::vector<std::string> names;
    for (const auto& [name, v] : e.global_->vars) {
        if (v.k == kind::builtin && !static_cast<const builtinObject&>(*v.obj).primitive)
            names.push_back(*name);
    }
    std::ranges::sort(names);
    return names;
}

evaluator::evaluator(std::string_view source, const std::string& file, compileOptions options)
    : options_(std::move(options)) {
    global_ = make<environment>();
    loadFile(file, source);
    nextIndex_ = {{metadata::pipeKind::audio, 0},
                  {metadata::pipeKind::control, metadata::tempoPipe + 1},
                  {metadata::pipeKind::event, 0}};
    installBuiltins();
}

void evaluator::raise(const sourceSpan& span, const std::string& message) const {
    std::vector<callSite> sites;
    sites.reserve(stack_.size());
    for (const auto& f : stack_) sites.push_back({.file = files_[f.file]->path, .span = f.span});
    throw failure(message, fileName(), span, std::move(sites));
}

interned evaluator::intern(std::string_view s) {
    auto& slot = names_[std::string(s)];
    if (!slot) slot = std::make_shared<const std::string>(s);
    return slot;
}

value evaluator::makeVector(std::vector<value> items) {
    auto vec = make<vectorObject>();
    vec->items = std::move(items);
    return makeObject(kind::vector, std::move(vec));
}

// Clears every object's references, breaking closure-environment cycles,
// so the objects are freed with the last references below.
evaluator::~evaluator() {
    for (const auto& o : objects_) o->clear();
}

compileResult evaluator::run() {
    compileResult result;
    try {
        evaluateFile(0);
        const sourceFile& entry = *files_.front();
        const auto it = entry.env->vars.find(intern(options_.entry));
        std::shared_ptr<closureObject> piece;
        if (it != entry.env->vars.end() && it->second.k == kind::closure) {
            piece = std::static_pointer_cast<closureObject>(it->second.obj);
        }
        if (!piece || !piece->piece) {
            fail(entry.read.data.empty() ? sourceSpan{} : entry.read.data.back().span,
                 "no piece named " + options_.entry + "; define it with (defpiece " + options_.entry +
                     " () ...)");
        }
        building_ = true;
        root_ = piece->name;
        graph_.seed = options_.seed;
        graph_.pagePort = options_.pagePort;
        callContext c;
        c.callee = piece->name;
        c.span = piece->span;
        applyClosure(*piece, c, false);
        building_ = false;
        finishPage();
        auto graph = finish(result.errors, result.warnings);
        if (result.errors.empty()) result.graph = std::move(graph);
    } catch (const failure& f) {
        result.errors.push_back({.message = f.what(), .file = f.file, .span = f.span, .stack = f.stack});
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
    case kind::buffer: return metadata::toString(v.buffer);
    case kind::node: return "a node";
    case kind::mapped: return "a mapped pipe";
    case kind::instance: return "an instance of " + *static_cast<const instanceObject&>(*v.obj).cls->name;
    case kind::generic: return "the generic function " + static_cast<const genericObject&>(*v.obj).name;
    case kind::element: return "the page's " + pageName(static_cast<const elementObject&>(*v.obj));
    }
    return out.str();
}

// --- Units (§8) ---------------------------------------------------------------

value evaluator::quantity(const quantityLiteral& q) const {
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
        std::vector<value> items;
        items.reserve(d.items.size());
        for (const auto& item : d.items) items.push_back(quoted(item));
        return makeVector(std::move(items));
    }
    default: return eval(d, global_);
    }
}

value evaluator::eval(const datum& d, const std::shared_ptr<environment>& env) {
    const depthGuard guard(*this, d.span);
    return evalForm(d, env);
}

value evaluator::evalForm(const datum& d, const std::shared_ptr<environment>& env) {
    value v;
    switch (d.kind) {
    case datumKind::nil: return v;
    case datumKind::boolean: v.k = kind::boolean, v.flag = d.boolean; return v;
    case datumKind::integer: return makeInteger(d.integer);
    case datumKind::floating: return makeReal(d.floating);
    case datumKind::quantity: return quantity(d.quantity);
    case datumKind::string: v.k = kind::string, v.text = intern(d.text); return v;
    case datumKind::keyword: v.k = kind::keyword, v.text = intern(d.text); return v;
    case datumKind::symbol: return lookup(d, env);
    case datumKind::vector: return evalVector(d, env);
    case datumKind::list: return evalList(d, env);
    }
    return v;
}

value evaluator::lookup(const datum& d, const std::shared_ptr<environment>& env) {
    const interned name = intern(d.text);
    for (auto e = env; e; e = e->parent) {
        if (const auto it = e->vars.find(name); it != e->vars.end()) return it->second;
    }
    fail(d.span, "unbound symbol ", d.text);
}

value evaluator::evalVector(const datum& d, const std::shared_ptr<environment>& env) {
    std::vector<value> items;
    items.reserve(d.items.size());
    for (const auto& item : d.items) items.push_back(eval(item, env));
    return makeVector(std::move(items));
}

std::shared_ptr<closureObject> evaluator::makeClosure(std::string name, const datum& params,
                                                      std::span<const datum> body,
                                                      const std::shared_ptr<environment>& env,
                                                      const sourceSpan& span) {
    if (params.kind != datumKind::list) fail(params.span, "expected a parameter list");
    if (body.empty()) fail(span, name + " has an empty body");
    auto f = make<closureObject>();
    f->name = std::move(name);
    f->body = body;
    f->env = env;
    f->span = span;
    f->file = current_;
    enum class mode : std::uint8_t { required, optional, rest, key } m = mode::required;
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

// Special forms (§5.1) each have their own function, and messages are
// built inside fail(), so the frames on the recursive path stay small
// (maxEvalDepth).
value evaluator::evalList(const datum& d, const std::shared_ptr<environment>& env) {
    if (d.items.empty()) return {};
    const datum& head = d.items[0];
    if (head.kind == datumKind::symbol) {
        const std::string& s = head.text;
        if (s == "quote") return formQuote(d);
        if (s == "if") return formIf(d, env);
        if (s == "cond") return formCond(d, env);
        if (s == "let") return formLet(d, env);
        if (s == "fn") return formFn(d, env);
        if (s == "do") return formDo(d, env);
        if (s == "set!") return formSet(d, env);
        if (s == "define") return defineForm(d, env, false);
        if (s == "defpiece") return defineForm(d, env, true);
        if (s == "module") return formModule(d, env);
        if (s == "pipe") return makePipe(d, env);
        if (s == "import") return formImport(d, env);
        if (s == "defclass") return formDefclass(d, env);
        if (s == "defgeneric") return formDefgeneric(d);
        if (s == "defmethod") return formDefmethod(d, env);
        if (s == "make") return formMake(d, env);
    }
    return evalCall(d, env);
}

value evaluator::formQuote(const datum& d) {
    if (d.items.size() != 2) fail(d.span, "quote takes one datum");
    return quoted(d.items[1]);
}

value evaluator::formIf(const datum& d, const std::shared_ptr<environment>& env) {
    const auto& items = d.items;
    if (items.size() < 3 || items.size() > 4) fail(d.span, "expected (if test then [else])");
    if (eval(items[1], env).truthy()) return eval(items[2], env);
    return items.size() == 4 ? eval(items[3], env) : value{};
}

value evaluator::formCond(const datum& d, const std::shared_ptr<environment>& env) {
    for (std::size_t i = 1; i < d.items.size(); ++i) {
        const auto& clause = d.items[i];
        if (clause.kind != datumKind::list || clause.items.empty())
            fail(clause.span, "expected (test body...)");
        if (clause.items[0].isSymbol("else") || eval(clause.items[0], env).truthy()) {
            value v;
            for (std::size_t j = 1; j < clause.items.size(); ++j) v = eval(clause.items[j], env);
            return v;
        }
    }
    return {};
}

value evaluator::formLet(const datum& d, const std::shared_ptr<environment>& env) {
    const auto& items = d.items;
    if (items.size() < 3 || items[1].kind != datumKind::list)
        fail(d.span, "expected (let ((name value)...) body...)");
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

value evaluator::formFn(const datum& d, const std::shared_ptr<environment>& env) {
    if (d.items.size() < 3) fail(d.span, "expected (fn (params...) body...)");
    value v;
    v.k = kind::closure;
    v.obj = makeClosure("fn", d.items[1], std::span(d.items).subspan(2), env, d.span);
    return v;
}

value evaluator::formDo(const datum& d, const std::shared_ptr<environment>& env) {
    value v;
    for (std::size_t i = 1; i < d.items.size(); ++i) v = eval(d.items[i], env);
    return v;
}

value evaluator::formSet(const datum& d, const std::shared_ptr<environment>& env) {
    const auto& items = d.items;
    if (items.size() != 3 || (items[1].kind != datumKind::symbol && items[1].kind != datumKind::list))
        fail(d.span, "expected (set! name value) or (set! (accessor object) value)");
    // Evaluate first: the value's evaluation may add bindings and
    // invalidate iterators into the frame.
    const value v = eval(items[2], env);
    if (items[1].kind == datumKind::list) return setPlace(items[1], v, env);
    const interned name = intern(items[1].text);
    for (auto e = env; e; e = e->parent) {
        if (const auto it = e->vars.find(name); it != e->vars.end()) return it->second = v;
    }
    fail(items[1].span, "unbound symbol ", items[1].text);
}

value evaluator::evalCall(const datum& d, const std::shared_ptr<environment>& env) {
    const auto& items = d.items;
    const datum& head = items[0];
    const value f = eval(head, env);
    callContext c;
    c.callee = head.kind == datumKind::symbol ? std::string_view(head.text) : std::string_view("fn");
    c.span = d.span;
    std::set<std::string_view> given;
    for (std::size_t i = 1; i < items.size(); ++i) {
        const datum& a = items[i];
        if (a.kind != datumKind::keyword) {
            if (!c.keywords.empty()) fail(a.span, "positional arguments come before keyword arguments");
            c.positional.push_back(eval(a, env));
            c.positionalSpans.push_back(a.span);
            continue;
        }
        if (i + 1 >= items.size()) fail(a.span, "keyword :", a.text, " needs a value");
        if (!given.insert(a.text).second) fail(a.span, ":", a.text, " is given twice");
        const datum& form = items[++i];
        value arg = eval(form, env);
        if (a.text != "key") {
            c.keywords.push_back(
                {.name = intern(a.text), .v = std::move(arg), .nameSpan = a.span, .valueSpan = form.span});
            continue;
        }
        // :key is reserved on every call (§10.2.2). Scalars only, so two
        // different keys cannot print alike.
        if (arg.k == kind::symbol || arg.k == kind::string || arg.k == kind::keyword) {
            c.key = *arg.text;
        } else if (arg.k == kind::integer) {
            c.key = std::to_string(arg.integer);
        } else {
            fail(form.span, ":key must be a symbol, string, keyword, or integer, got ", show(arg));
        }
        c.keySpan = a.span;
    }
    return apply(f, c);
}

value evaluator::apply(const value& f, callContext& c) {
    if (c.key && !building_)
        fail(c.keySpan, ":key names calls while a piece is built; it has no effect here");
    if (f.k == kind::closure) return applyClosure(static_cast<closureObject&>(*f.obj), c, true);
    if (f.k == kind::generic) return applyGeneric(static_cast<genericObject&>(*f.obj), c);
    if (f.k != kind::builtin) fail(c.span, show(f), " is not a function");
    auto& b = static_cast<builtinObject&>(*f.obj);
    c.callee = b.name;
    // A builtin with both forms is a primitive when called with keywords.
    if (b.primitive && (!b.math || !c.keywords.empty())) return callPrimitive(*b.primitive, c);
    if (!c.keywords.empty() && !b.takes.keywords)
        fail(c.keywords.front().nameSpan, b.name, " takes no keyword arguments");
    // :key names what a call makes: mapper's nodes, a buffer, and math on pipes.
    const bool makesNodes =
        b.takes.names || std::ranges::any_of(c.positional, [](const value& v) { return v.isSymbolic(); });
    if (c.key && !makesNodes)
        fail(c.keySpan, ":key names a node, a buffer, or a function call; ", b.name, " makes none of them");
    return b.math(*this, c);
}

void evaluator::failArity(const closureObject& f, const callContext& c) const {
    const std::size_t least = f.required.size();
    const std::size_t most = least + f.optional.size();
    const std::string expected = f.rest          ? "at least " + arguments(least)
                                 : most == least ? arguments(least)
                                                 : std::to_string(least) + " to " + arguments(most);
    fail(c.span, f.name, " expects ", expected, ", got ", std::to_string(c.positional.size()));
}

std::shared_ptr<environment> evaluator::bindArguments(closureObject& f, const callContext& c,
                                                      const std::shared_ptr<environment>& parent) {
    auto frame = make<environment>();
    frame->parent = parent;
    const std::size_t n = c.positional.size();
    const std::size_t least = f.required.size();
    const std::size_t most = least + f.optional.size();
    if (n < least || (n > most && !f.rest)) failArity(f, c);
    for (std::size_t i = 0; i < least; ++i) frame->vars[f.required[i]] = c.positional[i];
    for (std::size_t i = 0; i < f.optional.size(); ++i) {
        const std::size_t at = least + i;
        frame->vars[f.optional[i]] = at < n ? c.positional[at] : value{};
    }
    if (f.rest) {
        std::vector<value> rest;
        for (std::size_t i = most; i < n; ++i) rest.push_back(c.positional[i]);
        frame->vars[f.rest] = makeVector(std::move(rest));
    }
    for (const auto& k : f.keys) frame->vars[k] = {};
    for (const auto& arg : c.keywords) {
        if (std::ranges::find(f.keys, arg.name) == f.keys.end()) {
            fail(arg.nameSpan, f.name, " has no keyword :", *arg.name);
        }
        frame->vars[arg.name] = arg.v;
    }
    return frame;
}

value evaluator::applyClosure(closureObject& f, callContext& c, bool pushStep,
                              const std::shared_ptr<environment>& outer) {
    const auto frame = bindArguments(f, c, outer ? outer : f.env);
    const stepGuard guard(*this, {.file = current_, .span = c.span});
    const fileGuard inFile(*this, f.file);
    // Anonymous closures add no step: the occurrence index or :key of the
    // first named call inside identifies each application, so keys keep
    // IDs stable when the items of a map are reordered (§10.2.2).
    if (pushStep && building_ && (f.name != "fn" || c.key)) path_.push_back(nextStep(f.name, c.span, c.key));
    value v;
    for (const auto& form : f.body) v = eval(form, frame);
    return v;
}

value evaluator::makeBuiltin(std::string_view name, builtinObject::fn f) {
    auto b = make<builtinObject>();
    b->name = std::string(name);
    b->math = std::move(f);
    return makeObject(kind::builtin, std::move(b));
}

void evaluator::installBuiltins() {
    auto bind = [&](std::string_view name, builtinObject::fn math,
                    metadata::optionalRef<metadata::primitiveSpec> spec, builtinObject::traits takes = {}) {
        value v = makeBuiltin(name, std::move(math));
        auto& b = static_cast<builtinObject&>(*v.obj);
        b.primitive = spec;
        b.takes = takes;
        global_->vars[intern(name)] = v;
    };
    methods_.callNext = intern("call-next-method");
    methods_.nextP = intern("next-method-p");
    methods_.noNext = makeBuiltin("call-next-method", [](evaluator& e, callContext& c) -> value {
        e.fail(c.span, "call-next-method: there is no next method");
    });
    methods_.hasNext =
        makeBuiltin("next-method-p", [](evaluator&, callContext&) { return makeBoolean(true); });
    methods_.hasNoNext =
        makeBuiltin("next-method-p", [](evaluator&, callContext&) { return makeBoolean(false); });
    // Every primitive, by its language name; mapper operations also have a
    // positional form on constants and pipes (primitives.md §7.9).
    for (const auto& spec : metadata::primitiveSpecs()) {
        builtinObject::fn math;
        if (spec.mapper != metadata::operands::none) {
            math = [&spec](evaluator& e, callContext& c) { return e.math(spec, c); };
        }
        bind(languageName(spec.type), std::move(math), std::cref(spec));
    }
    for (const char op : {'+', '-', '*', '/'}) {
        bind(
            std::string(1, op), [op](evaluator& e, callContext& c) { return e.arithmetic(c, op); },
            std::nullopt);
    }
    for (const std::string_view op : {"=", "<", ">", "<=", ">="}) {
        bind(op, [op](evaluator& e, callContext& c) { return e.compare(c, op); }, std::nullopt);
    }
    bind("not", [](evaluator& e, callContext& c) { return e.logicalNot(c); }, std::nullopt);
    bind("list", [](evaluator& e, callContext& c) { return e.makeVector(c.positional); }, std::nullopt);
    bind("map", [](evaluator& e, callContext& c) { return e.map(c); }, std::nullopt);
    bind("range", [](evaluator& e, callContext& c) { return e.range(c); }, std::nullopt);
    bind("nth", [](evaluator& e, callContext& c) { return e.nth(c); }, std::nullopt);
    bind("unit-value", [](evaluator& e, callContext& c) { return e.unitValue(c); }, std::nullopt);
    bind("length", [](evaluator& e, callContext& c) { return e.length(c); }, std::nullopt);
    bind("string-append", [](evaluator& e, callContext& c) { return e.stringAppend(c); }, std::nullopt);
    bind("channel-count", [](evaluator& e, callContext& c) { return e.channelCount(c); }, std::nullopt);
    bind("tempo", [](evaluator& e, callContext& c) { return e.tempo(c); }, std::nullopt);
    bind("mapper", [](evaluator& e, callContext& c) { return e.mapper(c); }, std::nullopt, {.names = true});
    bind("buffer", [](evaluator& e, callContext& c) { return e.makeBuffer(c); }, std::nullopt,
         {.keywords = true, .names = true});
    bind("slot-value", [](evaluator& e, callContext& c) { return e.slotValue(c); }, std::nullopt);
    installPageBuiltins();
}

} // namespace detail

} // namespace interpreter
