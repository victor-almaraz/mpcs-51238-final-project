// Internal: the evaluator and graph builder behind compile().
//
// Values are small structs; heap objects (vectors, closures, builtins,
// mapped pipes, environments) are shared_ptrs. Closures and environments
// refer to each other, so the evaluator also registers every object and,
// when the compilation ends, clears each one's references, which breaks
// the cycles before the objects are freed. A compilation is short and
// builds one graph, so it needs no collector.

#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/interpreter/reader.h>
#include <espresso/metadata/graph.h>
#include <espresso/metadata/pageSpec.h>
#include <espresso/metadata/primitiveSpec.h>

namespace interpreter::detail {

struct object {
    object() = default;
    virtual ~object() = default;
    // Objects are shared by pointer, never copied or moved.
    object(const object&) = delete;
    object& operator=(const object&) = delete;
    object(object&&) = delete;
    object& operator=(object&&) = delete;
    // Drops this object's references to other objects (cycle breaking).
    virtual void clear() noexcept {}
};

using interned = std::shared_ptr<const std::string>;  // equal names are the same object

enum class kind : std::uint8_t {
    nil,
    boolean,
    integer,
    real,
    quantity,
    symbol,
    keyword,
    string,
    vector,
    closure,
    builtin,
    pipe,
    buffer,
    node,
    mapped,
    instance,
    generic,
    element,  // a display or a group of the page (dsl-ui.md §5)
};

struct value {
    kind k = kind::nil;
    bool flag = false;                                       // boolean
    std::int64_t integer = 0;                                // integer
    double number = 0.0;                                     // real; quantity, canonical
    metadata::dimension dim = metadata::dimension::number;   // quantity
    interned text;                                           // symbol, keyword, string; node: its ID
    std::shared_ptr<object> obj;  // vector, closure, builtin, mapped, instance, generic, element
    metadata::pipeRef pipe{};                                // pipe
    metadata::bufferRef buffer{};                            // buffer
    int channels = 1;                                        // pipe, buffer

    bool truthy() const noexcept { return k != kind::nil && (k != kind::boolean || flag); }
    // A number without a unit.
    bool isPlainNumber() const noexcept { return k == kind::integer || k == kind::real; }
    bool isNumber() const noexcept { return isPlainNumber() || k == kind::quantity; }
    bool isSymbolic() const noexcept { return k == kind::pipe || k == kind::mapped; }
    double asDouble() const noexcept { return k == kind::integer ? static_cast<double>(integer) : number; }
    // The integer a plain number equals: an integer, or a real with an
    // integer value, such as an index computed with / and floor. Builtins
    // that take an index or a count accept either.
    std::optional<std::int64_t> asInteger() const noexcept {
        if (k == kind::integer) return integer;
        // Reals in [-2^63, 2^63) convert exactly; NaN fails the comparison.
        if (k != kind::real || std::isnan(number) || number < -0x1p63 || number >= 0x1p63 ||
            std::trunc(number) != number) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(number);
    }
};

inline value makeInteger(std::int64_t x) {
    value v;
    v.k = kind::integer;
    v.integer = x;
    return v;
}

inline value makeBoolean(bool b) {
    value v;
    v.k = kind::boolean;
    v.flag = b;
    return v;
}

// A value of a heap kind: a vector, closure, builtin, instance, or generic.
inline value makeObject(kind k, std::shared_ptr<object> obj) {
    value v;
    v.k = k;
    v.obj = std::move(obj);
    return v;
}

// "1 argument", "2 arguments".
inline std::string arguments(std::size_t n) {
    return std::to_string(n) + (n == 1 ? " argument" : " arguments");
}

inline value makeReal(double x) {
    value v;
    v.k = kind::real;
    v.number = x;
    return v;
}

inline value makeQuantity(double x, metadata::dimension d) {
    value v;
    v.k = kind::quantity;
    v.number = x;
    v.dim = d;
    return v;
}

// "a time", "an interval": for messages.
inline std::string article(metadata::dimension d) {
    const std::string name(metadata::toString(d));
    return (name.front() == 'i' ? "an " : "a ") + name;
}

// "a time", or "a plain number" for a number without a unit.
inline std::string describeDim(const value& v) {
    return v.k == kind::quantity ? article(v.dim) : "a plain number";
}

struct environment : object {
    std::shared_ptr<environment> parent;
    std::unordered_map<interned, value> vars;
    void clear() noexcept override {
        parent.reset();
        vars.clear();
    }
};

struct vectorObject : object {
    std::vector<value> items;
    void clear() noexcept override { items.clear(); }
};

struct closureObject : object {
    std::string name;
    std::vector<interned> required;
    std::vector<interned> optional;
    std::vector<interned> keys;
    interned rest;
    std::span<const datum> body;  // into the evaluator's read result, which outlives every closure
    std::shared_ptr<environment> env;
    sourceSpan span;
    std::size_t file = 0;  // the evaluator's file the closure is written in
    bool piece = false;
    void clear() noexcept override { env.reset(); }
};

class evaluator;

// One keyword argument: its name, value, and where each is written.
struct keywordArg {
    interned name;
    value v;
    sourceSpan nameSpan;
    sourceSpan valueSpan;
};

// One call's evaluated arguments.
struct callContext {
    std::string_view callee;
    sourceSpan span;
    std::vector<value> positional;
    std::vector<sourceSpan> positionalSpans;
    std::vector<keywordArg> keywords;
    std::optional<std::string> key;  // the reserved :key argument (§10.2.2)
    sourceSpan keySpan;              // where :key is written
};

struct builtinObject : object {
    using fn = std::function<value(evaluator&, callContext&)>;
    std::string name;
    fn math;                                                 // positional form, if any
    metadata::optionalRef<metadata::primitiveSpec> primitive;  // keyword form, if any
    // What a positional builtin's call may carry: keyword arguments (buffer),
    // and :key, when the call makes something it names (mapper, buffer).
    struct traits {
        bool keywords = false;
        bool names = false;
    } takes;
    // call-next-method's function holds the arguments of its call.
    void clear() noexcept override { math = nullptr; }
};

// --- Object system (interpreter.md §7, grammar.md §3) ---------------------

// A slot of a class, with what it inherits.
struct slotSpec {
    interned name;
    std::vector<interned> initargs;         // keyword names, without the colon
    metadata::optionalRef<datum> initform;  // into the read result, like closure bodies
    std::shared_ptr<environment> env;       // where the initform evaluates: its defclass's
    std::size_t file = 0;                   // and in that defclass's file
    std::vector<interned> accessors;        // the class's own accessors for the slot
    // The page control a pedal or a synth panel draws the slot as (dsl-ui.md
    // §5.2): a call such as (knob "Gain" :max 1), evaluated where its
    // defclass is written, with the slot's value as its :initial.
    metadata::optionalRef<datum> control;
    std::shared_ptr<environment> controlEnv;
    std::size_t controlFile = 0;
};

struct classObject : object {
    interned name;
    std::vector<interned> precedence;  // C3 linearization, this class first (§7.4)
    std::vector<slotSpec> direct;      // as written in its defclass
    std::vector<slotSpec> slots;       // every slot, inherited ones included
    void clear() noexcept override {
        direct.clear();
        slots.clear();
    }
};

struct instanceObject : object {
    std::shared_ptr<const classObject> cls;
    std::vector<value> slots;  // in the order of cls->slots
    void clear() noexcept override { slots.clear(); }
};

enum class qualifier : std::uint8_t { primary, before, after, around };

struct methodObject {
    qualifier role = qualifier::primary;
    std::vector<interned> specializers;  // one class name per required parameter; null for any
    std::shared_ptr<closureObject> fn;   // null for an accessor
    interned slot;                       // an accessor's slot, else null
};

// A generic function's methods, sorted for one tuple of argument classes:
// most specific first in each role (§7.3).
struct effectiveMethod {
    std::vector<methodObject> around, before, primary, after;
};

struct genericObject : object {
    std::string name;
    std::size_t required = 0;
    std::vector<methodObject> methods;
    // Argument classes (null for a value that is not an instance) to
    // effective methods (§7.5). Keyed by class identity: an instance made
    // before its class was redefined keeps its own class's methods.
    std::map<std::vector<std::shared_ptr<const classObject>>, std::shared_ptr<const effectiveMethod>> cache;
    void clear() noexcept override {
        methods.clear();
        cache.clear();
    }
};

struct mappedObject : object {
    value source;
    std::shared_ptr<closureObject> fn;
    sourceSpan span;
    std::vector<std::string> path;  // instance path of the mapper, including its own step
    std::optional<value> lowered;
    void clear() noexcept override {
        source = {};
        fn.reset();
        lowered.reset();
    }
};

// An element of the page (dsl-ui.md §5): a control, a display, or a group,
// as the language made it. Its layout node takes its children and its
// address when the page is laid out (evaluator::finishPage).
struct elementObject : object {
    metadata::pageNode node;                                       // kind, label, address, params
    std::vector<std::shared_ptr<elementObject>> children;          // a group's
    std::map<std::string, metadata::dimension, std::less<>> dims;  // the dimensions of quantities given
    std::vector<std::string> nodeIds;                              // a control's or display's nodes
    std::optional<metadata::pipeRef> pipe;                         // a control's pipe
    sourceSpan span;                                               // its call
    bool labelled = false;                                         // a label given
    bool explicitAddress = false;                                  // :address given
    bool placed = false;                                           // in a group
    void clear() noexcept override { children.clear(); }
};

// Thrown to abort a compilation; caught by compile(). The message is
// what().
struct failure : std::runtime_error {
    failure(const std::string& message, std::string file, sourceSpan span, std::vector<callSite> stack)
        : std::runtime_error(message), file(std::move(file)), span(span), stack(std::move(stack)) {}
    std::string file;
    sourceSpan span;
    std::vector<callSite> stack;
};

// One file of the project (§11): its forms, its top-level bindings, and
// what it exports.
struct sourceFile {
    std::string path;  // relative to the project root
    readResult read;   // closures' bodies are spans into it
    std::shared_ptr<environment> env;
    bool exportsAll = true;  // no (export ...) clause
    std::vector<interned> exports;
    std::set<interned> imported;  // names it imports, which it does not export
    bool declared = false;        // has a module form
    bool loaded = false;          // false while its forms are evaluated
};

// A call on the evaluator's stack, in the file it is written in.
struct frame {
    std::size_t file = 0;
    sourceSpan span;
};

// The one conversion between the reader's spans and the IR's spans.
metadata::sourceSpan toIrSpan(const sourceSpan& span, const std::string& file);
sourceSpan fromIrSpan(const std::optional<metadata::sourceSpan>& span);

class evaluator {
public:
    // Reads @p source, evaluates it, and builds the entry piece. An
    // evaluator compiles once: closures keep spans into its read result.
    static compileResult compile(std::string_view source, const std::string& file, compileOptions options);
    // The global names that are builtins and not primitives, sorted (builtinNames()).
    static std::vector<std::string> builtinNames();

    ~evaluator();
    evaluator(const evaluator&) = delete;
    evaluator& operator=(const evaluator&) = delete;

private:
    friend class pathGuard;
    friend class stepGuard;
    friend class depthGuard;
    friend class fileGuard;

    evaluator(std::string_view source, const std::string& file, compileOptions options);
    compileResult run();

    template <typename T>
    std::shared_ptr<T> make() {
        auto p = std::make_shared<T>();
        objects_.push_back(p);
        return p;
    }
    value makeVector(std::vector<value> items);
    // A builtin function named @p name.
    value makeBuiltin(std::string_view name, builtinObject::fn f);

    // Language core (evaluator.cpp).
    value eval(const datum& d, const std::shared_ptr<environment>& env);
    value evalForm(const datum& d, const std::shared_ptr<environment>& env);
    value evalList(const datum& d, const std::shared_ptr<environment>& env);
    value evalCall(const datum& d, const std::shared_ptr<environment>& env);
    // The arguments of call @p d, evaluated in @p env.
    callContext callArguments(const datum& d, const std::shared_ptr<environment>& env);
    value formQuote(const datum& d);
    value formIf(const datum& d, const std::shared_ptr<environment>& env);
    value formCond(const datum& d, const std::shared_ptr<environment>& env);
    value formLet(const datum& d, const std::shared_ptr<environment>& env);
    value formFn(const datum& d, const std::shared_ptr<environment>& env);
    value formDo(const datum& d, const std::shared_ptr<environment>& env);
    value formSet(const datum& d, const std::shared_ptr<environment>& env);
    value apply(const value& f, callContext& c);
    // Applies @p f in a frame whose parent is @p outer, or f's own
    // environment when @p outer is null.
    value applyClosure(closureObject& f, callContext& c, bool pushStep,
                       const std::shared_ptr<environment>& outer = nullptr);
    std::shared_ptr<environment> bindArguments(closureObject& f, const callContext& c,
                                               const std::shared_ptr<environment>& parent);
    value lookup(const datum& d, const std::shared_ptr<environment>& env);
    value evalVector(const datum& d, const std::shared_ptr<environment>& env);
    std::shared_ptr<closureObject> makeClosure(std::string name, const datum& params,
                                               std::span<const datum> body,
                                               const std::shared_ptr<environment>& env,
                                               const sourceSpan& span);
    value defineForm(const datum& d, const std::shared_ptr<environment>& env, bool piece);
    // Projects and modules (§11, modules.cpp).
    value formModule(const datum& d, const std::shared_ptr<environment>& env);
    value formImport(const datum& d, const std::shared_ptr<environment>& env);
    std::size_t loadFile(const std::string& path, std::string_view source);
    std::string sourceOf(const std::string& path, const sourceSpan& span);
    void evaluateFile(std::size_t index);
    const std::string& fileName() const { return files_[current_]->path; }
    value quantity(const quantityLiteral& q) const;
    value quoted(const datum& d);
    void installBuiltins();
    // Fails with the concatenation of @p parts. Building the message here
    // keeps its temporaries out of the callers' frames, which matters on
    // the recursive path (maxEvalDepth).
    template <typename... Parts>
    [[noreturn]] void fail(const sourceSpan& span, const Parts&... parts) const {
        std::string message;
        ((message += parts), ...);
        raise(span, message);
    }
    [[noreturn]] void raise(const sourceSpan& span, const std::string& message) const;
    [[noreturn]] void failArity(const closureObject& f, const callContext& c) const;
    interned intern(std::string_view s);
    std::string show(const value& v) const;

    // Object system (objects.cpp).
    value formDefclass(const datum& d, const std::shared_ptr<environment>& env);
    value formDefgeneric(const datum& d);
    value formDefmethod(const datum& d, const std::shared_ptr<environment>& env);
    value formMake(const datum& d, const std::shared_ptr<environment>& env);
    value setPlace(const datum& place, const value& v, const std::shared_ptr<environment>& env);
    value slotValue(callContext& c);
    value applyGeneric(genericObject& g, callContext& c);
    std::shared_ptr<genericObject> ensureGeneric(const std::string& name, std::size_t required,
                                                 const sourceSpan& span);
    void addMethod(genericObject& g, methodObject m);
    std::shared_ptr<const effectiveMethod> effectiveMethods(genericObject& g, std::span<const value> given);
    using nextMethod = std::function<value(callContext&)>;
    value callMethod(const methodObject& m, callContext& c, nextMethod next);
    value callAround(const std::shared_ptr<const effectiveMethod>& em, std::size_t i, callContext& c);
    value callMain(const std::shared_ptr<const effectiveMethod>& em, callContext& c);
    value callPrimary(const std::shared_ptr<const effectiveMethod>& em, std::size_t i, callContext& c);
    std::vector<interned> linearize(const datum& name,
                                    const std::vector<std::shared_ptr<classObject>>& supers);
    value& slotOf(const value& object, const interned& slot, const sourceSpan& span);

    // Builtins (builtins.cpp).
    value arithmetic(callContext& c, char op);
    value compare(callContext& c, std::string_view op);
    value logicalNot(callContext& c);
    value map(callContext& c);
    value range(callContext& c);
    value nth(callContext& c);
    value unitValue(callContext& c);
    value length(callContext& c);
    value stringAppend(callContext& c);
    value channelCount(callContext& c);
    value math(const metadata::primitiveSpec& spec, callContext& c);
    void checkOperands(const metadata::primitiveSpec& spec, const callContext& c) const;
    value tempo(callContext& c);
    value mapper(callContext& c);

    // The page (page.cpp).
    void installPageBuiltins();
    static std::string pageName(const elementObject& e);
    value textValue(kind k, std::string_view text);
    metadata::paramValue pageParam(std::string_view who, const metadata::paramSpec& spec, const value& v,
                                   const sourceSpan& span);
    value emitPrimitive(std::string_view type, const callContext& at,
                        const std::vector<std::pair<std::string_view, value>>& args);
    std::shared_ptr<elementObject> pageElement(const metadata::pageKindSpec& spec, callContext& c);
    metadata::dimension pageRange(elementObject& e, const callContext& c);
    value pageControl(const metadata::pageKindSpec& spec, callContext& c);
    value pageDisplay(const metadata::pageKindSpec& spec, callContext& c);
    value pageGroupCall(const metadata::pageKindSpec& spec, callContext& c);
    void pageItems(const value& v, const sourceSpan& span, std::vector<std::shared_ptr<elementObject>>& out);
    void slotControls(instanceObject& inst, std::vector<std::shared_ptr<elementObject>>& out);
    std::shared_ptr<elementObject> pageGroup(std::string_view kind,
                                             std::vector<std::shared_ptr<elementObject>> items,
                                             const sourceSpan& span);
    void finishPage();

    // Graph building (graph.cpp).
    value makePipe(const datum& form, const std::shared_ptr<environment>& env);
    value makeBuffer(callContext& c);
    value callPrimitive(const metadata::primitiveSpec& spec, callContext& c);
    value lower(mappedObject& m);
    value emitMath(callContext& c, metadata::optionalRef<metadata::primitiveSpec> op);
    value emitArithmetic(callContext& c);
    metadata::graphSpec finish(std::vector<compileError>& errors, std::vector<compileError>& warnings);
    std::string nextStep(std::string_view callee, const sourceSpan& span, const std::optional<std::string>& key);
    std::string instanceId(const std::string& step) const;
    value allocatePipe(metadata::pipeKind kind, int channels, bool feedback);
    // @p v, a pipe's or a buffer's :channels, written at @p span.
    int channelCount(const value& v, const sourceSpan& span) const;
    std::vector<metadata::pipeRef> pipesOf(const value& v, const sourceSpan& span, bool input);
    metadata::paramValue paramOf(const metadata::paramSpec& p, const value& v, const sourceSpan& span);
    void checkRange(const metadata::paramSpec& p, double v, const sourceSpan& span,
                    const std::string& what) const;
    value emit(std::string_view op, std::string_view type, const std::vector<value>& inputs,
               const std::vector<std::pair<std::string, value>>& params, const sourceSpan& span);

    // Declared first and destroyed last: closures' bodies are spans into
    // the files' read results. The entry file is the first.
    std::vector<std::unique_ptr<sourceFile>> files_;
    std::size_t current_ = 0;           // the file being evaluated
    std::vector<std::size_t> loading_;  // files whose imports are being loaded, outermost first
    compileOptions options_;
    std::vector<std::shared_ptr<object>> objects_;  // every object, for clearing at the end
    std::unordered_map<std::string, interned> names_;
    std::shared_ptr<environment> global_;
    std::vector<frame> stack_;
    std::size_t depth_ = 0;    // nested evaluations (depthGuard)
    std::uint64_t steps_ = 0;  // evaluations so far, against options_.maxSteps
    std::unordered_map<interned, std::shared_ptr<classObject>> classes_;
    // What every method body binds (§7.3): the names, and the builtins of a
    // method with no next method and of next-method-p, made once.
    struct {
        interned callNext;
        interned nextP;
        value noNext;
        value hasNext;
        value hasNoNext;
    } methods_;

    // Graph under construction.
    bool building_ = false;
    int lowering_ = 0;
    std::string root_;
    metadata::graphSpec graph_;
    std::map<metadata::pipeKind, std::int32_t> nextIndex_;
    std::vector<std::string> path_;  // the instance path (§10.2)
    std::map<std::string, int> occurrences_;
    std::set<std::string> keys_;
    std::set<std::string> ids_;
    // The page under construction (page.cpp).
    std::map<metadata::pipeRef, std::shared_ptr<elementObject>> controls_;  // by their pipes
    std::vector<std::shared_ptr<elementObject>> pageElements_;              // controls and displays, as made
    std::shared_ptr<elementObject> page_;                                   // the piece's (page ...)
};

// Restores the instance path, after a step pushed on it, for its lifetime.
class pathGuard {
public:
    explicit pathGuard(evaluator& e) : e_(e), depth_(e.path_.size()) {}
    ~pathGuard() { e_.path_.resize(depth_); }
    pathGuard(const pathGuard&) = delete;
    pathGuard& operator=(const pathGuard&) = delete;

private:
    evaluator& e_;
    std::size_t depth_;
};

// A call on the call stack, and its steps on the instance path, for its
// lifetime.
class stepGuard {
public:
    stepGuard(evaluator& e, frame call) : e_(e), path_(e) { e_.stack_.push_back(call); }
    ~stepGuard() { e_.stack_.pop_back(); }
    stepGuard(const stepGuard&) = delete;
    stepGuard& operator=(const stepGuard&) = delete;

private:
    evaluator& e_;
    pathGuard path_;
};

// Evaluates in file @p file for its lifetime: errors and node sources
// name it.
class fileGuard {
public:
    fileGuard(evaluator& e, std::size_t file) : e_(e), saved_(e.current_) { e_.current_ = file; }
    ~fileGuard() { e_.current_ = saved_; }
    fileGuard(const fileGuard&) = delete;
    fileGuard& operator=(const fileGuard&) = delete;

private:
    evaluator& e_;
    std::size_t saved_;
};

// Counts one level of nested evaluation, and one step of the budget, for
// its lifetime, and fails before the C++ stack runs out (calls recurse on
// the C++ stack: design-notes "M2 compiler subset").
class depthGuard {
public:
    depthGuard(evaluator& e, const sourceSpan& span);
    ~depthGuard();
    depthGuard(const depthGuard&) = delete;
    depthGuard& operator=(const depthGuard&) = delete;

private:
    evaluator& e_;
};

} // namespace interpreter::detail
