// Internal: the evaluator and graph builder behind compile().
//
// Values are small structs; heap objects (vectors, closures, builtins,
// mapped pipes, environments) are shared_ptrs. Closures and environments
// refer to each other, so the evaluator also registers every object and,
// when the compilation ends, clears each one's references, which breaks
// the cycles before the objects are freed. A compilation is short and
// builds one graph, so it needs no collector.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <proj/interpreter/compiler.h>
#include <proj/interpreter/reader.h>
#include <proj/metadata/graph.h>
#include <proj/metadata/primitiveSpec.h>

namespace interpreter::detail {

struct object {
    virtual ~object() = default;
    // Drops this object's references to other objects (cycle breaking).
    virtual void clear() noexcept {}
};

using interned = std::shared_ptr<const std::string>;  // equal names are the same object

enum class kind : std::uint8_t {
    nil, boolean, integer, real, quantity, symbol, keyword, string, vector, closure, builtin, pipe, node, mapped,
};

struct value {
    kind k = kind::nil;
    bool flag = false;                                       // boolean
    std::int64_t integer = 0;                                // integer
    double number = 0.0;                                     // real; quantity, canonical
    metadata::dimension dim = metadata::dimension::number;   // quantity
    interned text{};                                         // symbol, keyword, string
    std::shared_ptr<object> obj{};                           // vector, closure, builtin, mapped
    metadata::pipeRef pipe{};                                // pipe
    int channels = 1;                                        // pipe
    std::size_t node = 0;                                    // node: index into the graph

    bool truthy() const noexcept { return !(k == kind::nil || (k == kind::boolean && !flag)); }
    bool isNumber() const noexcept { return k == kind::integer || k == kind::real || k == kind::quantity; }
    double asDouble() const noexcept { return k == kind::integer ? static_cast<double>(integer) : number; }
};

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
    std::span<const datum> body;  // into the read result, which outlives the compilation
    std::shared_ptr<environment> env;
    sourceSpan span;
    bool piece = false;
    void clear() noexcept override { env.reset(); }
};

class evaluator;

// One call's evaluated arguments.
struct callContext {
    std::string_view callee;
    sourceSpan span;
    std::vector<value> positional;
    std::vector<sourceSpan> positionalSpans;
    std::vector<std::pair<interned, value>> keywords;
    std::vector<sourceSpan> keywordSpans;
    std::optional<std::string> key;  // the reserved :key argument (§10.2.2)
};

struct builtinObject : object {
    using fn = std::function<value(evaluator&, callContext&)>;
    std::string name;
    fn math;                                                 // positional form, if any
    metadata::optionalRef<metadata::primitiveSpec> primitive;  // keyword form, if any
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

// Thrown to abort a compilation; caught by compile().
struct failure {
    std::string message;
    sourceSpan span;
    std::vector<sourceSpan> stack;
};

class evaluator {
public:
    evaluator(std::string file, compileOptions options);
    ~evaluator();
    evaluator(const evaluator&) = delete;
    evaluator& operator=(const evaluator&) = delete;
    compileResult run(std::string_view source);

    // Language core (evaluator.cpp).
    value eval(const datum& d, const std::shared_ptr<environment>& env);
    value apply(const value& f, callContext& c);
    [[noreturn]] void fail(const sourceSpan& span, std::string message) const;
    interned intern(std::string_view s);
    std::string show(const value& v) const;

    // Builtins (builtins.cpp).
    value arithmetic(callContext& c, char op);
    value add(callContext& c) { return arithmetic(c, '+'); }
    value subtract(callContext& c) { return arithmetic(c, '-'); }
    value multiply(callContext& c) { return arithmetic(c, '*'); }
    value divide(callContext& c) { return arithmetic(c, '/'); }
    value compare(callContext& c, std::string_view op);
    value equal(callContext& c) { return compare(c, "="); }
    value less(callContext& c) { return compare(c, "<"); }
    value greater(callContext& c) { return compare(c, ">"); }
    value lessEqual(callContext& c) { return compare(c, "<="); }
    value greaterEqual(callContext& c) { return compare(c, ">="); }
    value logicalNot(callContext& c);
    value list(callContext& c);
    value map(callContext& c);
    value range(callContext& c);
    value math(callContext& c);
    value tempo(callContext& c);
    value mapper(callContext& c);

    // Graph building (graph.cpp).
    value makePipe(const datum& form, const std::shared_ptr<environment>& env);
    value callPrimitive(const metadata::primitiveSpec& spec, callContext& c);
    value lower(mappedObject& m);
    value emitMath(std::string_view op, std::vector<value> args, const sourceSpan& span,
                   const std::optional<std::string>& key = std::nullopt);
    metadata::graphSpec finish(std::vector<compileError>& errors, std::vector<compileError>& warnings);

private:
    friend struct stepGuard;

    template <typename T>
    std::shared_ptr<T> make() {
        auto p = std::make_shared<T>();
        objects_.push_back(p);
        return p;
    }

    value evalList(const datum& d, const std::shared_ptr<environment>& env);
    value applyClosure(closureObject& f, callContext& c, bool pushStep);
    std::shared_ptr<closureObject> makeClosure(std::string name, const datum& params, std::span<const datum> body,
                               const std::shared_ptr<environment>& env, const sourceSpan& span);
    value defineForm(const datum& d, const std::shared_ptr<environment>& env, bool piece);
    value quantity(const quantityLiteral& q, const sourceSpan& span) const;
    value quoted(const datum& d);
    void installBuiltins();

    // Graph helpers (graph.cpp).
    std::string nextStep(std::string_view callee, const sourceSpan& span, const std::optional<std::string>& key);
    std::string instanceId(const std::string& step) const;
    value allocatePipe(metadata::pipeKind kind, int channels, bool feedback);
    std::vector<metadata::pipeRef> pipesOf(const value& v, const sourceSpan& span, bool input);
    metadata::paramValue paramOf(const metadata::paramSpec& p, const value& v, const sourceSpan& span);
    value emit(std::string_view op, std::string_view type, const std::vector<value>& inputs,
               const std::vector<std::pair<std::string, value>>& params, const sourceSpan& span);
    metadata::sourceSpan toSpan(const sourceSpan& span) const;

    std::string file_;
    compileOptions options_;
    std::vector<std::shared_ptr<object>> objects_;  // every object, for clearing at the end
    std::unordered_map<std::string, interned> names_;
    std::shared_ptr<environment> global_;
    std::vector<sourceSpan> stack_;

    // Graph under construction.
    bool building_ = false;
    int lowering_ = 0;
    std::string root_;
    metadata::graphSpec graph_;
    std::map<metadata::pipeKind, std::int32_t> nextIndex_;
    std::vector<std::string> steps_;
    std::map<std::string, int> occurrences_;
    std::set<std::string> keys_;
    std::set<std::string> ids_;
};

} // namespace interpreter::detail
