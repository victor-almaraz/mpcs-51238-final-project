// Pages in the language (dsl-ui.md §4, §5): the controls, which add the
// osc-in a page sends to and return its pipe; the displays, which add the
// osc-out a page shows; the groups that lay them out; and the layout they
// make, written into the IR's page.layout with each control's address.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/interpreter/reader.h>
#include <espresso/metadata/graph.h>
#include <espresso/metadata/pageSpec.h>
#include <espresso/metadata/primitiveSpec.h>
#include <espresso/metadata/text.h>

#include "evaluator.h"

namespace interpreter::detail {

using metadata::dimension;
using metadata::pageKindSpec;
using metadata::pageRole;

namespace {

// The language's name for layout kind @p kind: `value` reads as `readout`,
// since programs name their values `value`, and `synth` as `synth-panel`.
std::string_view languageNameOf(std::string_view kind) {
    if (kind == "value") return "readout";
    if (kind == "synth") return "synth-panel";
    return kind;
}

// True when a group of kind @p kind prefixes the addresses it holds with its label.
bool prefixes(std::string_view kind) { return kind == "section" || kind == "pedal"; }

// The segment of an address that @p label gives: lowercase letters and
// digits, other runs as one hyphen, none at either end; @p fallback when
// nothing is left.
std::string slug(std::string_view label, std::string_view fallback) {
    std::string out;
    for (const char ch : label) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) != 0) {
            out += static_cast<char>(std::tolower(c));
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? std::string(fallback) : out;
}

// A name written for people: "max-time" as "Max time".
std::string spoken(std::string_view name) {
    std::string out(name);
    for (char& c : out) {
        if (c == '-') c = ' ';
    }
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

// The units a control can show its value in, by the dimension of its range.
std::string_view unitFor(dimension d) {
    switch (d) {
    case dimension::frequency: return "hz";
    case dimension::time: return "s";
    case dimension::level: return "db";
    default: return "";
    }
}

}  // namespace

std::string evaluator::pageName(const elementObject& e) { return std::string(languageNameOf(e.node.kind)); }

void evaluator::installPageBuiltins() {
    for (const auto& spec : metadata::pageKinds()) {
        builtinObject::fn call;
        switch (spec.role) {
        case pageRole::group:
            call = [&spec](evaluator& e, callContext& c) { return e.pageGroupCall(spec, c); };
            break;
        case pageRole::sends:
            call = [&spec](evaluator& e, callContext& c) { return e.pageControl(spec, c); };
            break;
        case pageRole::shows:
            call = [&spec](evaluator& e, callContext& c) { return e.pageDisplay(spec, c); };
            break;
        }
        value v = makeBuiltin(languageNameOf(spec.kind), std::move(call));
        static_cast<builtinObject&>(*v.obj).takes = {.keywords = true, .names = true};
        global_->vars[intern(languageNameOf(spec.kind))] = v;
    }
}

// The page parameter @p spec of a call to @p who, from the language value @p v.
metadata::paramValue evaluator::pageParam(std::string_view who, const metadata::paramSpec& spec,
                                          const value& v, const sourceSpan& span) {
    const std::string what = std::string(who) + " :" + std::string(spec.name);
    const auto items = [&](std::string_view expected) -> const std::vector<value>& {
        if (v.k != kind::vector) fail(span, what, " expects ", expected, ", got ", show(v));
        return static_cast<const vectorObject&>(*v.obj).items;
    };
    switch (spec.dim) {
    case dimension::choice:
        if (v.k != kind::keyword && v.k != kind::string)
            fail(span, what, " expects one of :", metadata::joinNames(spec.choices), ", got ", show(v));
        {
            // :percent names the unit written %, which a keyword cannot hold.
            std::string name = *v.text == "percent" && spec.name == "unit" ? std::string("%") : *v.text;
            if (std::ranges::find(spec.choices, name) == spec.choices.end()) {
                fail(span, what, " expects one of :", metadata::joinNames(spec.choices), ", got ", show(v));
            }
            return name;
        }
    case dimension::text:
        if (v.k != kind::string) fail(span, what, " expects a string, got ", show(v));
        return *v.text;
    case dimension::textList: {
        std::vector<std::string> out;
        for (const value& item : items("a vector of strings")) {
            if (item.k != kind::string && item.k != kind::keyword)
                fail(span, what, " expects a vector of strings, got ", show(item), " in it");
            out.push_back(*item.text);
        }
        return out;
    }
    case dimension::list: {
        // One number starts both of a pad's axes.
        if (spec.name == "initial" && v.isNumber()) return std::vector<double>{v.asDouble(), v.asDouble()};
        std::vector<double> out;
        for (const value& item : items("a vector of numbers")) {
            if (!item.isNumber())
                fail(span, what, " expects a vector of numbers, got ", show(item), " in it");
            out.push_back(item.asDouble());
        }
        return out;
    }
    default:
        if (v.k == kind::boolean) return v.flag ? 1.0 : 0.0;
        if (!v.isNumber()) fail(span, what, " expects a number, got ", show(v));
        return v.asDouble();
    }
}

// A symbol, keyword, or string value of @p text.
value evaluator::textValue(kind k, std::string_view text) {
    value v;
    v.k = k;
    v.text = intern(text);
    return v;
}

// Adds a node of primitive @p type, as a call written at @p at with the
// keyword arguments @p args would, under @p at's :key.
value evaluator::emitPrimitive(std::string_view type, const callContext& at,
                               const std::vector<std::pair<std::string_view, value>>& args) {
    const auto found = metadata::findPrimitive(type);
    if (!found) fail(at.span, "internal error: no primitive named ", type);
    callContext c;
    c.callee = at.callee;
    c.span = at.span;
    c.key = at.key;
    c.keySpan = at.keySpan;
    for (const auto& [name, v] : args) {
        c.keywords.push_back({.name = intern(name), .v = v, .nameSpan = at.span, .valueSpan = at.span});
    }
    return callPrimitive(*found, c);
}

// A new page element of @p spec, called as @p c: its label, a string
// first when the call gives one, its own parameters, and :address, :span,
// and :at.
std::shared_ptr<elementObject> evaluator::pageElement(const pageKindSpec& spec, callContext& c) {
    const std::string who(languageNameOf(spec.kind));
    if (!building_) fail(c.span, who, " adds to the page, so it must be called while a piece is built");
    if (!options_.pagePort) {
        fail(c.span, who,
             " is on the page, but the project has no page port; add :page-port to project.lisp, "
             "such as (project :name \"piece\" :page-port 8000)");
    }
    auto e = make<elementObject>();
    e->node.kind = std::string(spec.kind);
    e->node.src = toIrSpan(c.span, fileName());
    e->span = c.span;
    if (!c.positional.empty() && c.positional[0].k == kind::string) {
        e->node.label = *c.positional[0].text;
        e->labelled = true;
    }
    for (const auto& arg : c.keywords) {
        const std::string& name = *arg.name;
        if (name == "address" && spec.role != pageRole::group) {
            if (arg.v.k != kind::string || !arg.v.text->starts_with('/'))
                fail(arg.valueSpan, who, " :address expects a string starting with /, such as \"/cutoff\"");
            e->node.address = *arg.v.text;
            e->explicitAddress = true;
        } else if (const auto param = spec.param(name)) {
            e->node.params[name] = pageParam(who, *param, arg.v, arg.valueSpan);
            if (arg.v.k == kind::quantity) e->dims[name] = arg.v.dim;
        } else {
            fail(arg.nameSpan, who, " has no :", name);
        }
    }
    return e;
}

// A control's range: the quantities among :min, :max, and :initial share
// a dimension (plain numbers go with any), which names the unit it shows
// by default; a frequency's scale is exponential and a level's in
// decibels unless :scale says otherwise, on kinds that have a unit and a
// scale. Returns the dimension.
dimension evaluator::pageRange(elementObject& e, const callContext& c) {
    std::optional<dimension> dim;
    for (const std::string_view name : {"min", "max", "initial"}) {
        const auto it = e.dims.find(name);
        if (it == e.dims.end()) continue;
        if (dim && *dim != it->second) {
            fail(c.span, pageName(e), " :min, :max, and :initial must be of one kind: ", article(*dim),
                 " and ", article(it->second));
        }
        dim = it->second;
    }
    const dimension d = dim.value_or(dimension::number);
    // Only a kind that has the parameter takes it: a meter shows no unit.
    const auto has = [&](std::string_view name) {
        const auto spec = metadata::findPageKind(e.node.kind);
        return spec && spec->get().param(name);
    };
    if (!e.node.params.contains("unit") && !unitFor(d).empty() && has("unit"))
        e.node.params["unit"] = std::string(unitFor(d));
    if (!e.node.params.contains("scale") && (d == dimension::frequency || d == dimension::level) &&
        has("scale")) {
        e.node.params["scale"] = std::string(d == dimension::frequency ? "exp" : "db");
    }
    return d;
}

// knob, slider, toggle, button, select, xy, and keys: an osc-in on the page
// port at the control's address, which every compile fills in once the
// page is laid out; returns its pipe, or for keys the notes osc-notes
// makes of its messages.
value evaluator::pageControl(const pageKindSpec& spec, callContext& c) {
    const auto e = pageElement(spec, c);
    const std::string_view k = spec.kind;
    if (c.positional.size() > (e->labelled ? 1U : 0U)) {
        fail(c.span, pageName(*e), " takes a label and keyword arguments, such as (", pageName(*e),
             " \"Cutoff\" :min 20hz :max 20khz)");
    }
    const value page = textValue(kind::keyword, "page");
    // Filled in once the page is laid out (finishPage).
    const value pending = textValue(kind::string, e->node.address);
    std::vector<std::pair<std::string_view, value>> args = {{"port", page}, {"address", pending}};
    if (k == "keys") {
        const value messages = allocatePipe(metadata::pipeKind::event, 1, false);
        const value notes = allocatePipe(metadata::pipeKind::event, 1, false);
        args.emplace_back("out", messages);
        e->nodeIds.push_back(*emitPrimitive("oscIn", c, args).text);
        e->nodeIds.push_back(
            *emitPrimitive("oscNotes", c, {{"in", messages}, {"out", notes}, {"address", pending}}).text);
        e->pipe = notes.pipe;
    } else {
        // The value the pipe holds until the page sends one: the page's own
        // starting value, :initial or :min, so the piece plays as written
        // without the page.
        value initial = makeReal(0);
        if (spec.param("min")) {
            const dimension d = pageRange(*e, c);
            const auto min = e->node.params.find("min");
            auto& start =
                e->node.params.try_emplace("initial", min == e->node.params.end() ? 0.0 : min->second)
                    .first->second;
            const auto starting = [&](double x) {
                return d == dimension::number ? makeReal(x) : makeQuantity(x, d);
            };
            if (k == "xy") {
                // :min, the default, starts both axes.
                if (const auto both = metadata::as<double>(start))
                    start = std::vector<double>{both->get(), both->get()};
                const auto& xy = std::get<std::vector<double>>(start);
                if (xy.size() != 2 || xy[0] != xy[1]) {
                    fail(c.span, "xy :initial must be one number for both axes, such as :initial 0.5: osc-in "
                                 "starts every channel of its pipe at one value");
                }
                initial = starting(xy[0]);
            } else {
                initial = starting(std::get<double>(start));
            }
        } else if (const auto start = spec.param("initial")) {
            // A switch starts where the table says: a toggle off, a footswitch engaged.
            const auto& given =
                e->node.params.try_emplace("initial", start->get().defaultValue).first->second;
            initial = makeReal(std::get<double>(given));
        }
        const value out = allocatePipe(metadata::pipeKind::control, k == "xy" ? 2 : 1, false);
        args.emplace_back("value", out);
        args.emplace_back("initial", initial);
        e->nodeIds.push_back(*emitPrimitive("oscIn", c, args).text);
        e->pipe = out.pipe;
    }
    controls_.emplace(*e->pipe, e);
    pageElements_.push_back(e);
    value out;
    out.k = kind::pipe;
    out.pipe = *e->pipe;
    out.channels = k == "xy" ? 2 : 1;
    return out;
}

// meter, lamp, and readout: an osc-out on the page port sending the pipe
// given after the label to the display's address; returns the display.
value evaluator::pageDisplay(const pageKindSpec& spec, callContext& c) {
    const auto e = pageElement(spec, c);
    const std::size_t shown = e->labelled ? 1 : 0;
    if (c.positional.size() != shown + 1) {
        fail(c.span, pageName(*e), " takes a label and the pipe it shows, such as (", pageName(*e),
             " \"Level\" level)");
    }
    if (spec.param("min")) pageRange(*e, c);
    const value page = textValue(kind::keyword, "page");
    // Filled in once the page is laid out; osc-out needs one before.
    const value address =
        textValue(kind::string, e->node.address.empty() ? "/" + std::string(spec.kind) : e->node.address);
    e->nodeIds.push_back(
        *emitPrimitive("oscOut", c, {{"port", page}, {"address", address}, {"value", c.positional[shown]}})
             .text);
    pageElements_.push_back(e);
    return makeObject(kind::element, e);
}

// The elements @p v stands for, appended to @p out: an element, a
// control's pipe, or a vector of them.
void evaluator::pageItems(const value& v, const sourceSpan& span,
                          std::vector<std::shared_ptr<elementObject>>& out) {
    if (v.k == kind::element) {
        out.push_back(std::static_pointer_cast<elementObject>(v.obj));
    } else if (v.k == kind::pipe && controls_.contains(v.pipe)) {
        out.push_back(controls_.at(v.pipe));
    } else if (v.k == kind::vector) {
        for (const value& item : static_cast<const vectorObject&>(*v.obj).items) pageItems(item, span, out);
    } else {
        fail(span, "expected a control, a display, or a group of the page, got ", show(v));
    }
}

// A group of @p kind holding @p items, those it cannot hold wrapped in the
// group it holds (a page's controls in a panel's section): a group made
// without a call of its own, at @p span.
std::shared_ptr<elementObject> evaluator::pageGroup(std::string_view kind,
                                                    std::vector<std::shared_ptr<elementObject>> items,
                                                    const sourceSpan& span) {
    const auto found = metadata::findPageKind(kind);
    if (!found) fail(span, "internal error: no page kind named ", kind);
    const pageKindSpec& spec = *found;
    auto group = make<elementObject>();
    group->node.kind = std::string(kind);
    group->node.src = toIrSpan(span, fileName());
    group->span = span;
    // The kind an element this group cannot hold is wrapped in: the first
    // of its kinds that holds it, else its first kind that is a group.
    const auto wrapperFor = [&](std::string_view item) -> std::string_view {
        std::string_view first;
        for (const auto k : spec.children) {
            const auto inner = metadata::findPageKind(k);
            if (!inner || inner->get().role != pageRole::group) continue;
            if (std::ranges::find(inner->get().children, item) != inner->get().children.end()) return k;
            if (first.empty()) first = k;
        }
        return first;
    };
    std::vector<std::shared_ptr<elementObject>> pending;
    std::string_view wrapper;
    const auto flush = [&] {
        if (pending.empty()) return;
        group->children.push_back(pageGroup(wrapper, std::exchange(pending, {}), span));
    };
    for (auto& item : items) {
        if (std::ranges::find(spec.children, item->node.kind) != spec.children.end()) {
            flush();
            group->children.push_back(std::move(item));
            continue;
        }
        const std::string_view into = wrapperFor(item->node.kind);
        if (into.empty()) {
            fail(item->span, "a ", languageNameOf(kind), " holds ", metadata::joinNames(spec.children),
                 ", not a ", pageName(*item));
        }
        if (into != wrapper) flush();
        wrapper = into;
        pending.push_back(std::move(item));
    }
    flush();
    return group;
}

// The controls the slots of @p inst declare with :control (dsl-ui.md
// §5.2), appended to @p out in slot order: each slot's call evaluated
// where its defclass is written, labelled with the slot's name unless it
// gives a label, starting at the slot's value; the slot then holds the
// control's pipe. A slot that holds a pipe already is not drawn.
void evaluator::slotControls(instanceObject& inst, std::vector<std::shared_ptr<elementObject>>& out) {
    for (std::size_t i = 0; i < inst.cls->slots.size(); ++i) {
        const slotSpec& slot = inst.cls->slots[i];
        value& held = inst.slots[i];
        if (!slot.control || held.isSymbolic()) continue;
        const datum& form = slot.control->get();
        const fileGuard inFile(*this, slot.controlFile);
        const value f = eval(form.items[0], slot.controlEnv);
        const bool control =
            f.k == kind::builtin && std::ranges::any_of(metadata::pageKinds(), [&](const auto& k) {
                return k.role == pageRole::sends &&
                       languageNameOf(k.kind) == static_cast<const builtinObject&>(*f.obj).name;
            });
        if (!control) {
            fail(form.span, "slot ", *slot.name, "'s :control must make a page control, such as (knob), not ",
                 show(f));
        }
        callContext c = callArguments(form, slot.controlEnv);
        if (c.positional.empty() || c.positional[0].k != kind::string) {
            c.positional.insert(c.positional.begin(), textValue(kind::string, spoken(*slot.name)));
            c.positionalSpans.insert(c.positionalSpans.begin(), form.span);
        }
        const bool started =
            std::ranges::any_of(c.keywords, [](const keywordArg& k) { return *k.name == "initial"; });
        if (!started && (held.isNumber() || held.k == kind::boolean)) {
            c.keywords.push_back(
                {.name = intern("initial"), .v = held, .nameSpan = form.span, .valueSpan = form.span});
        }
        held = apply(f, c);
        out.push_back(controls_.at(held.pipe));
    }
}

// page, panel, section, synth-panel, pedalboard, and pedal: a group of the
// elements given after its label (a page has none), placing each once. A
// pedal or a synth panel given an instance draws its :control slots, and
// takes its class's name when it has no label.
value evaluator::pageGroupCall(const pageKindSpec& spec, callContext& c) {
    const auto e = pageElement(spec, c);
    if (spec.kind == metadata::pageRoot && e->labelled) {
        fail(c.positionalSpans[0], "page takes its panels, sections, and controls, not a label");
    }
    const bool draws = spec.kind == "pedal" || spec.kind == "synth";
    std::vector<std::shared_ptr<elementObject>> items;
    for (std::size_t i = e->labelled ? 1 : 0; i < c.positional.size(); ++i) {
        const value& v = c.positional[i];
        if (draws && v.k == kind::instance) {
            auto& inst = static_cast<instanceObject&>(*v.obj);
            if (!e->labelled) {
                e->node.label = spoken(*inst.cls->name);
                e->labelled = true;
            }
            slotControls(inst, items);
            continue;
        }
        pageItems(v, c.positionalSpans[i], items);
    }
    for (const auto& item : items) {
        if (item->placed) fail(item->span, pageName(*item), " is placed on the page twice");
        item->placed = true;
    }
    auto group = pageGroup(spec.kind, std::move(items), c.span);
    group->node.label = e->node.label;
    group->node.params = e->node.params;
    if (spec.kind == metadata::pageRoot) {
        if (page_) fail(c.span, "a piece has one page; this is its second (page ...)");
        page_ = group;
    }
    return makeObject(kind::element, group);
}

// The page's layout, written into the graph: the page the piece made, or
// one, with the controls and displays it placed nowhere in a last section,
// in the order they were made. Each element without an address takes one
// from its label under its sections' labels, a second of a name taking
// -2, and its nodes the same.
void evaluator::finishPage() {
    std::vector<std::shared_ptr<elementObject>> loose;
    for (const auto& e : pageElements_) {
        if (!e->placed) loose.push_back(e);
    }
    if (!page_ && loose.empty()) return;
    if (!page_) {
        page_ = pageGroup(metadata::pageRoot, loose, loose.front()->span);
    } else if (!loose.empty()) {
        auto section = pageGroup("section", loose, loose.front()->span);
        if (page_->children.empty()) {
            page_->children.push_back(pageGroup("panel", {section}, section->span));
        } else {
            page_->children.back()->children.push_back(section);
        }
    }
    // Addresses written by hand are taken first, so a derived one never
    // repeats one.
    std::set<std::string> taken;
    for (const auto& e : pageElements_) {
        if (e->explicitAddress) taken.insert(e->node.address);
    }
    const auto claim = [&](const std::string& base) {
        std::string name = base;
        for (int n = 2; taken.contains(name); ++n) name = base + '-' + std::to_string(n);
        taken.insert(name);
        return name;
    };
    std::map<std::string, std::string, std::less<>> addresses;  // node ID to address
    const std::function<metadata::pageNode(const elementObject&, const std::string&)> tree =
        [&](const elementObject& e, const std::string& prefix) {
            metadata::pageNode node = e.node;
            const auto& spec = metadata::findPageKind(e.node.kind)->get();
            if (spec.role == pageRole::group) {
                std::string inner = prefix;
                if (prefixes(e.node.kind) && !e.node.label.empty())
                    inner = claim(prefix + '/' + slug(e.node.label, e.node.kind));
                for (const auto& child : e.children) node.children.push_back(tree(*child, inner));
                return node;
            }
            if (!e.explicitAddress)
                node.address = claim(prefix + '/' + slug(e.node.label, languageNameOf(e.node.kind)));
            for (const auto& id : e.nodeIds) addresses[id] = node.address;
            return node;
        };
    graph_.layout = tree(*page_, "");
    for (auto& n : graph_.nodes) {
        if (const auto it = addresses.find(n.id); it != addresses.end()) n.params["address"] = it->second;
    }
}

}  // namespace interpreter::detail
