// The object system: classes, instances, generic functions, and standard
// method combination (interpreter.md §7, grammar.md §3).

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <proj/interpreter/reader.h>

#include "evaluator.h"

namespace interpreter::detail {

namespace {

const std::vector<interned> noClasses;

// The class precedence list of @p v; empty for a value that is not an
// instance.
const std::vector<interned>& precedenceOf(const value& v) {
    return v.k == kind::instance ? static_cast<const instanceObject&>(*v.obj).cls->precedence : noClasses;
}

// How specific @p specializer is for an argument with @p precedence: its
// position in the list, and last for an unspecialized parameter.
std::size_t rank(const interned& specializer, const std::vector<interned>& precedence) {
    if (!specializer) return std::numeric_limits<std::size_t>::max();
    return static_cast<std::size_t>(std::ranges::find(precedence, specializer) - precedence.begin());
}

bool applies(const methodObject& m, std::span<const value> args) {
    for (std::size_t i = 0; i < m.specializers.size(); ++i) {
        const auto& s = m.specializers[i];
        if (s && std::ranges::find(precedenceOf(args[i]), s) == precedenceOf(args[i]).end()) return false;
    }
    return true;
}

value boolean(bool b) {
    value v;
    v.k = kind::boolean;
    v.flag = b;
    return v;
}

// "1 argument", "2 arguments".
std::string arguments(std::size_t n) { return std::to_string(n) + (n == 1 ? " argument" : " arguments"); }

// Required parameters of a lambda list: those before the first &-marker.
std::size_t requiredCount(const datum& params) {
    std::size_t n = 0;
    for (const auto& p : params.items) {
        if (p.kind == datumKind::symbol && p.text.starts_with('&')) break;
        ++n;
    }
    return n;
}

}  // namespace

// --- Classes and instances ------------------------------------------------

// C3 linearization (§7.4): the class, then the merge of its superclasses'
// lists and the list of the superclasses themselves.
std::vector<interned> evaluator::linearize(const datum& name,
                                           const std::vector<std::shared_ptr<classObject>>& supers) {
    std::vector<std::vector<interned>> lists;
    std::vector<interned> direct;
    for (const auto& s : supers) {
        lists.push_back(s->precedence);
        direct.push_back(s->name);
    }
    lists.push_back(std::move(direct));
    std::vector<interned> out{intern(name.text)};
    for (;;) {
        std::erase_if(lists, [](const auto& l) { return l.empty(); });
        if (lists.empty()) return out;
        interned next;
        for (const auto& l : lists) {
            const auto inTail = [&](const auto& other) {
                return std::find(other.begin() + 1, other.end(), l.front()) != other.end();
            };
            if (std::ranges::none_of(lists, inTail)) {
                next = l.front();
                break;
            }
        }
        if (!next)
            fail(name.span, "the superclasses of ", name.text,
                 " have no consistent order (C3 linearization)");
        out.push_back(next);
        for (auto& l : lists) {
            if (l.front() == next) l.erase(l.begin());
        }
    }
}

value evaluator::formDefclass(const datum& d, const std::shared_ptr<environment>& env) {
    const auto& items = d.items;
    if (items.size() < 3 || items.size() > 4 || items[1].kind != datumKind::symbol ||
        items[2].kind != datumKind::list || (items.size() == 4 && items[3].kind != datumKind::list))
        fail(d.span, "expected (defclass name (superclasses...) ((slot options...)...))");
    auto cls = make<classObject>();
    cls->name = intern(items[1].text);
    cls->env = env;
    cls->file = current_;
    std::vector<std::shared_ptr<classObject>> supers;
    for (const auto& s : items[2].items) {
        if (s.kind != datumKind::symbol) fail(s.span, "superclasses are class names");
        if (s.text == items[1].text) fail(s.span, "a class cannot be its own superclass");
        const auto it = classes_.find(intern(s.text));
        if (it == classes_.end())
            fail(s.span, "no class named ", s.text, "; define it before its subclasses");
        if (std::ranges::find(supers, it->second) != supers.end()) fail(s.span, s.text, " is listed twice");
        supers.push_back(it->second);
    }
    cls->precedence = linearize(items[1], supers);
    const std::span<const datum> specs =
        items.size() == 4 ? std::span(items[3].items) : std::span<const datum>();
    for (const datum& spec : specs) {
        const bool bare = spec.kind == datumKind::symbol;
        if (!bare &&
            (spec.kind != datumKind::list || spec.items.empty() || spec.items[0].kind != datumKind::symbol))
            fail(spec.span, "expected (slot-name options...)");
        slotSpec slot{.name = intern(bare ? spec.text : spec.items[0].text),
                      .initargs = {},
                      .initform = std::nullopt,
                      .accessors = {}};
        if (std::ranges::any_of(cls->direct, [&](const slotSpec& s) { return s.name == slot.name; }))
            fail(spec.span, "slot ", *slot.name, " is defined twice");
        for (std::size_t j = 1; !bare && j < spec.items.size(); j += 2) {
            const datum& option = spec.items[j];
            if (option.kind != datumKind::keyword || j + 1 >= spec.items.size())
                fail(option.span, "expected a slot option and its value: :initarg, :initform, or :accessor");
            const datum& arg = spec.items[j + 1];
            if (option.text == "initarg") {
                if (arg.kind != datumKind::keyword) fail(arg.span, ":initarg takes a keyword");
                slot.initargs.push_back(intern(arg.text));
            } else if (option.text == "initform") {
                slot.initform = std::cref(arg);
            } else if (option.text == "accessor") {
                if (arg.kind != datumKind::symbol) fail(arg.span, ":accessor takes a symbol");
                slot.accessors.push_back(intern(arg.text));
            } else {
                fail(option.span, "unknown slot option :", option.text,
                     "; expected :initarg, :initform, or :accessor");
            }
        }
        cls->direct.push_back(std::move(slot));
    }
    // Each slot once, most specific class first: the most specific
    // initform wins and initargs accumulate, as in CLOS.
    for (const auto& name : cls->precedence) {
        const classObject& from = name == cls->name ? *cls : *classes_.at(name);
        for (const auto& s : from.direct) {
            const auto it =
                std::ranges::find_if(cls->slots, [&](const slotSpec& e) { return e.name == s.name; });
            if (it == cls->slots.end()) {
                cls->slots.push_back(
                    {.name = s.name, .initargs = s.initargs, .initform = s.initform, .accessors = {}});
                continue;
            }
            if (!it->initform) it->initform = s.initform;
            for (const auto& a : s.initargs) {
                if (std::ranges::find(it->initargs, a) == it->initargs.end()) it->initargs.push_back(a);
            }
        }
    }
    classes_[cls->name] = cls;
    for (const auto& g : generics_) g->cache.clear();  // §7.5
    for (const auto& slot : cls->direct) {
        for (const auto& accessor : slot.accessors) {
            addMethod(
                *ensureGeneric(*accessor, 1, d.span),
                {.role = qualifier::primary, .specializers = {cls->name}, .fn = nullptr, .slot = slot.name});
        }
    }
    value v;
    v.k = kind::symbol;
    v.text = cls->name;
    return v;
}

value evaluator::formMake(const datum& d, const std::shared_ptr<environment>& env) {
    const auto& items = d.items;
    if (items.size() < 2 || items[1].kind != datumKind::symbol)
        fail(d.span, "expected (make class :initarg value...)");
    const auto found = classes_.find(intern(items[1].text));
    if (found == classes_.end()) fail(items[1].span, "no class named ", items[1].text);
    const std::shared_ptr<const classObject> cls = found->second;
    auto inst = make<instanceObject>();
    inst->cls = cls;
    inst->slots.resize(cls->slots.size());
    std::vector<bool> given(cls->slots.size(), false);
    for (std::size_t i = 2; i < items.size(); i += 2) {
        const datum& k = items[i];
        if (k.kind != datumKind::keyword || i + 1 >= items.size()) fail(k.span, "expected :initarg value");
        const interned name = intern(k.text);
        const auto slot = std::ranges::find_if(cls->slots, [&](const slotSpec& s) {
            return std::ranges::find(s.initargs, name) != s.initargs.end();
        });
        if (slot == cls->slots.end()) fail(k.span, "class ", *cls->name, " has no initarg :", k.text);
        const auto at = static_cast<std::size_t>(slot - cls->slots.begin());
        if (given[at]) fail(k.span, "slot ", *slot->name, " is given twice");
        inst->slots[at] = eval(items[i + 1], env);
        given[at] = true;
    }
    for (std::size_t i = 0; i < cls->slots.size(); ++i) {
        const auto& initform = cls->slots[i].initform;
        if (given[i] || !initform) continue;
        const fileGuard inFile(*this, cls->file);
        inst->slots[i] = eval(initform->get(), cls->env);
    }
    value v;
    v.k = kind::instance;
    v.obj = inst;
    return v;
}

value& evaluator::slotOf(const value& object, const interned& slot, const sourceSpan& span) {
    if (object.k != kind::instance) fail(span, "expected an instance, got ", show(object));
    auto& inst = static_cast<instanceObject&>(*object.obj);
    const auto it = std::ranges::find_if(inst.cls->slots, [&](const slotSpec& s) { return s.name == slot; });
    if (it == inst.cls->slots.end()) fail(span, "class ", *inst.cls->name, " has no slot ", *slot);
    return inst.slots[static_cast<std::size_t>(it - inst.cls->slots.begin())];
}

value evaluator::slotValue(callContext& c) {
    if (c.positional.size() != 2 || c.positional[1].k != kind::symbol)
        fail(c.span, "expected (slot-value object 'slot)");
    return slotOf(c.positional[0], c.positional[1].text, c.positionalSpans[1]);
}

// (set! (slot-value object 'slot) v) or (set! (accessor object) v).
value evaluator::setPlace(const datum& place, const value& v, const std::shared_ptr<environment>& env) {
    const auto& items = place.items;
    if (items.empty() || items[0].kind != datumKind::symbol) fail(place.span, "expected (accessor object)");
    if (items[0].text == "slot-value") {
        if (items.size() != 3) fail(place.span, "expected (slot-value object 'slot)");
        const value object = eval(items[1], env);
        const value slot = eval(items[2], env);
        if (slot.k != kind::symbol) fail(items[2].span, "a slot is named by a symbol");
        return slotOf(object, slot.text, items[2].span) = v;
    }
    const value f = lookup(items[0], env);
    if (f.k != kind::generic || items.size() != 2 || static_cast<const genericObject&>(*f.obj).required != 1)
        fail(place.span, items[0].text, " is not an accessor");
    callContext c;
    c.callee = items[0].text;
    c.span = place.span;
    c.positional = {eval(items[1], env)};
    c.positionalSpans = {items[1].span};
    const auto em = effectiveMethods(static_cast<genericObject&>(*f.obj), c);
    if (em->primary.empty() || !em->primary.front().slot)
        fail(place.span, items[0].text, " is not an accessor of ", show(c.positional[0]));
    return slotOf(c.positional[0], em->primary.front().slot, place.span) = v;
}

// --- Generic functions ----------------------------------------------------

std::shared_ptr<genericObject> evaluator::ensureGeneric(const std::string& name, std::size_t required,
                                                        const sourceSpan& span) {
    value& bound = global_->vars[intern(name)];
    if (bound.k == kind::generic) {
        auto g = std::static_pointer_cast<genericObject>(bound.obj);
        if (g->required != required)
            fail(span, name, " takes ", arguments(g->required), " to dispatch on, not ",
                 std::to_string(required));
        return g;
    }
    if (bound.k != kind::nil)
        fail(span, name, " is already ", show(bound), "; a generic function needs its own name");
    auto g = make<genericObject>();
    g->name = name;
    g->required = required;
    generics_.push_back(g);
    bound.k = kind::generic;
    bound.obj = g;
    return g;
}

void evaluator::addMethod(genericObject& g, methodObject m) {
    const auto same = std::ranges::find_if(g.methods, [&](const methodObject& o) {
        return o.role == m.role && o.specializers == m.specializers;
    });
    if (same != g.methods.end()) {
        *same = std::move(m);  // a method with the same qualifier and specializers is replaced
    } else {
        g.methods.push_back(std::move(m));
    }
    g.cache.clear();  // §7.5
}

value evaluator::formDefgeneric(const datum& d) {
    const auto& items = d.items;
    if (items.size() != 3 || items[1].kind != datumKind::symbol || items[2].kind != datumKind::list)
        fail(d.span, "expected (defgeneric name (params...))");
    value v;
    v.k = kind::generic;
    v.obj = ensureGeneric(items[1].text, requiredCount(items[2]), d.span);
    return v;
}

value evaluator::formDefmethod(const datum& d, const std::shared_ptr<environment>& env) {
    const auto& items = d.items;
    if (items.size() < 4 || items[1].kind != datumKind::symbol)
        fail(d.span, "expected (defmethod name [qualifier] ((param class)...) body...)");
    methodObject m;
    std::size_t at = 2;
    if (items[2].kind == datumKind::keyword) {
        const std::string& q = items[2].text;
        m.role = q == "before"   ? qualifier::before
                 : q == "after"  ? qualifier::after
                 : q == "around" ? qualifier::around
                                 : qualifier::primary;
        if (m.role == qualifier::primary)
            fail(items[2].span, "unknown qualifier :", q, "; expected :before, :after, or :around");
        ++at;
    }
    const datum& lambda = items[at];
    if (lambda.kind != datumKind::list) fail(lambda.span, "expected a parameter list");
    datum params = lambda;  // the specializers are read now, not kept
    const std::size_t required = requiredCount(lambda);
    for (std::size_t i = 0; i < required; ++i) {
        datum& p = params.items[i];
        if (p.kind == datumKind::symbol) {
            m.specializers.emplace_back();
            continue;
        }
        if (p.kind != datumKind::list || p.items.size() != 2 || p.items[0].kind != datumKind::symbol ||
            p.items[1].kind != datumKind::symbol)
            fail(p.span, "expected a parameter or (parameter class)");
        const std::string& cls = p.items[1].text;
        if (cls != "t" && !classes_.contains(intern(cls))) fail(p.items[1].span, "no class named ", cls);
        m.specializers.push_back(cls == "t" ? interned{} : intern(cls));
        datum name = p.items[0];
        p = std::move(name);
    }
    if (at + 1 >= items.size()) fail(d.span, items[1].text, " has an empty body");
    m.fn = makeClosure(items[1].text, params, std::span(items).subspan(at + 1), env, d.span);
    auto g = ensureGeneric(items[1].text, required, d.span);
    addMethod(*g, std::move(m));
    value v;
    v.k = kind::generic;
    v.obj = g;
    return v;
}

// The applicable methods for @p c's arguments, sorted by specificity
// (§7.3) and cached by argument classes (§7.5).
std::shared_ptr<const effectiveMethod> evaluator::effectiveMethods(genericObject& g, const callContext& c) {
    const std::span<const value> args(c.positional.data(), g.required);
    std::vector<interned> key;
    for (const auto& a : args) key.push_back(a.k == kind::instance ? precedenceOf(a).front() : interned{});
    if (const auto it = g.cache.find(key); it != g.cache.end()) return it->second;
    std::vector<methodObject> applicable;
    std::ranges::copy_if(g.methods, std::back_inserter(applicable),
                         [&](const methodObject& m) { return applies(m, args); });
    // Leftmost argument first, each by its class precedence list.
    std::ranges::stable_sort(applicable, [&](const methodObject& a, const methodObject& b) {
        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto ra = rank(a.specializers[i], precedenceOf(args[i]));
            const auto rb = rank(b.specializers[i], precedenceOf(args[i]));
            if (ra != rb) return ra < rb;
        }
        return false;
    });
    auto em = std::make_shared<effectiveMethod>();
    for (auto& m : applicable) {
        auto& role = m.role == qualifier::around   ? em->around
                     : m.role == qualifier::before ? em->before
                     : m.role == qualifier::after  ? em->after
                                                   : em->primary;
        role.push_back(std::move(m));
    }
    g.cache.emplace(std::move(key), em);
    return em;
}

value evaluator::applyGeneric(genericObject& g, callContext& c) {
    if (c.positional.size() < g.required) {
        fail(c.span, g.name, " expects at least ", arguments(g.required), ", got ",
             std::to_string(c.positional.size()));
    }
    const auto em = effectiveMethods(g, c);
    if (em->primary.empty()) {
        std::string args;
        for (std::size_t i = 0; i < g.required; ++i) args += (i == 0 ? "" : ", ") + show(c.positional[i]);
        fail(c.span, "no method of ", g.name, " applies to ", args);
    }
    // A generic function call is one step of the instance path, whichever
    // methods run (§10.2).
    stack_.push_back({.file = current_, .span = c.span});
    const stepGuard guard{.e = *this, .depth = path_.size()};
    if (building_) path_.push_back(nextStep(g.name, c.span, c.key));
    return em->around.empty() ? callMain(em, c) : callAround(em, 0, c);
}

value evaluator::callMethod(const methodObject& m, callContext& c, nextMethod next) {
    if (m.slot) return slotOf(c.positional.front(), m.slot, c.positionalSpans.front());
    auto frame = make<environment>();
    frame->parent = m.fn->env;
    const auto bind = [&](std::string_view name, builtinObject::fn f) {
        auto b = make<builtinObject>();
        b->name = std::string(name);
        b->math = std::move(f);
        value v;
        v.k = kind::builtin;
        v.obj = b;
        frame->vars[intern(name)] = v;
    };
    const bool hasNext = static_cast<bool>(next);
    // Without arguments, the next method gets this method's arguments.
    bind("call-next-method", [next = std::move(next), args = c](evaluator& e, callContext& call) {
        if (!next) e.fail(call.span, "call-next-method: there is no next method");
        callContext again = args;
        if (!call.positional.empty()) {
            again.positional = call.positional;
            again.positionalSpans = call.positionalSpans;
        }
        return next(again);
    });
    bind("next-method-p", [hasNext](evaluator& /*e*/, callContext& /*call*/) { return boolean(hasNext); });
    return applyClosure(*m.fn, c, false, frame);
}

value evaluator::callAround(const std::shared_ptr<const effectiveMethod>& em, std::size_t i, callContext& c) {
    return callMethod(em->around[i], c, [this, em, i](callContext& next) {
        return i + 1 < em->around.size() ? callAround(em, i + 1, next) : callMain(em, next);
    });
}

// :before methods, most specific first; the primary methods; then :after
// methods, least specific first. The value is the primary methods'.
value evaluator::callMain(const std::shared_ptr<const effectiveMethod>& em, callContext& c) {
    for (const auto& m : em->before) callMethod(m, c, nullptr);
    value v = callPrimary(em, 0, c);
    for (const auto& m : std::views::reverse(em->after)) callMethod(m, c, nullptr);
    return v;
}

value evaluator::callPrimary(const std::shared_ptr<const effectiveMethod>& em, std::size_t i,
                             callContext& c) {
    nextMethod next;
    if (i + 1 < em->primary.size())
        next = [this, em, i](callContext& n) { return callPrimary(em, i + 1, n); };
    return callMethod(em->primary[i], c, std::move(next));
}

}  // namespace interpreter::detail
