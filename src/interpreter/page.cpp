// Pages in the language (dsl-ui.md §4, §5): the controls, which add the
// osc-in a page sends to and return its pipe; the displays, which add the
// osc-out a page shows; the groups that lay them out; and the layout they
// make, written into the IR's page.layout with each control's address.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/interpreter/reader.h>
#include <espresso/metadata/graph.h>
#include <espresso/metadata/pageLayout.h>
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
        case pageRole::space:
            call = [&spec](evaluator& e, callContext& c) { return e.pageSpace(spec, c); };
            break;
        }
        value v = makeBuiltin(languageNameOf(spec.kind), std::move(call));
        static_cast<builtinObject&>(*v.obj).takes = {.keywords = true, .names = true};
        global_->vars[intern(languageNameOf(spec.kind))] = v;
    }
    global_->vars[intern("control-range")] =
        makeBuiltin("control-range", [](evaluator& e, callContext& c) { return e.controlRange(c); });
    global_->vars[intern("route")] =
        makeBuiltin("route", [](evaluator& e, callContext& c) { return e.route(c); });
    global_->vars[intern("show-modulation")] =
        makeBuiltin("show-modulation", [](evaluator& e, callContext& c) { return e.showModulation(c); });
}

// (route instance in out): the audio pipes the instrument or effect
// @p instance reads (@p in, or #f) and writes (@p out), recorded on the
// group that drew its controls, for the routing between a page's tabs
// (canvas.md §4.2). An instance no group drew records nothing. Returns nil.
value evaluator::route(callContext& c) {
    if (c.positional.size() != 3 || c.positional[0].k != kind::instance)
        fail(c.span, "route takes an instance, the pipe it reads (or #f), and the pipe it writes");
    const auto& inst = static_cast<const instanceObject&>(*c.positional[0].obj);
    if (!inst.drawnIn) return {};
    if (c.positional[1].k == kind::pipe) inst.drawnIn->reads.push_back(c.positional[1].pipe);
    if (c.positional[2].k == kind::pipe) inst.drawnIn->writes.push_back(c.positional[2].pipe);
    return {};
}

void evaluator::raiseAt(const std::optional<metadata::sourceSpan>& src, const sourceSpan& span,
                        const std::string& message) {
    std::size_t file = current_;
    for (std::size_t i = 0; i < files_.size(); ++i) {
        if (src && files_[i]->path == src->file) file = i;
    }
    const fileGuard inFile(*this, file);
    raise(span, message);
}

// Each tab's routing, from the pipes its groups' instruments and effects
// read and write: the tabs it reads from, and those it writes to, with
// "output" where a device output reads what it writes (canvas.md §4.2).
// A page with tabs holds only tabs.
void evaluator::routeTabs() {
    if (!page_) return;
    const auto& tabs = page_->children;
    const bool tabbed = std::ranges::any_of(tabs, [](const auto& t) { return t->node.kind == "tab"; });
    if (!tabbed) return;
    std::set<std::string, std::less<>> labels;
    for (const auto& t : tabs) {
        if (t->node.kind != "tab")
            failAt(*t, "a page with tabs holds only tabs; put this ", pageName(*t),
                   " in one (tab \"Name\" ...)");
        if (t->node.label.empty())
            failAt(*t, "a tab takes a label, its name on the strip, such as (tab \"Bass\" ...)");
        if (!labels.insert(t->node.label).second)
            failAt(*t, "a page has one tab named \"", t->node.label, "\"; this is its second");
    }
    struct pipes {
        std::set<metadata::pipeRef> reads;
        std::set<metadata::pipeRef> writes;
    };
    std::vector<pipes> of(tabs.size());
    const std::function<void(const elementObject&, pipes&)> gather = [&](const elementObject& e, pipes& p) {
        p.reads.insert(e.reads.begin(), e.reads.end());
        p.writes.insert(e.writes.begin(), e.writes.end());
        for (const auto& child : e.children) gather(*child, p);
    };
    for (std::size_t i = 0; i < tabs.size(); ++i) gather(*tabs[i], of[i]);
    std::set<metadata::pipeRef> toDevice;
    for (const auto& n : graph_.nodes) {
        if (n.type != "audioOut") continue;
        for (const auto& [port, refs] : n.inputs) toDevice.insert(refs.begin(), refs.end());
    }
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        std::vector<std::string> from;
        std::vector<std::string> to;
        for (std::size_t j = 0; j < tabs.size(); ++j) {
            // A pipe a tab writes and reads itself (a chain's links) routes nowhere.
            if (i == j) continue;
            const auto meets = [](const std::set<metadata::pipeRef>& a,
                                  const std::set<metadata::pipeRef>& b) {
                return std::ranges::any_of(a, [&](const auto& r) { return b.contains(r); });
            };
            if (meets(of[j].writes, of[i].reads)) from.push_back(tabs[j]->node.label);
            if (meets(of[i].writes, of[j].reads)) to.push_back(tabs[j]->node.label);
        }
        if (std::ranges::any_of(of[i].writes, [&](const auto& r) { return toDevice.contains(r); }))
            to.emplace_back("output");
        if (!from.empty()) tabs[i]->node.params["from"] = from;
        if (!to.empty()) tabs[i]->node.params["to"] = to;
    }
}

// (show-modulation pipe low high): the page control whose pipe @p pipe is
// shows the span its modulations reach, from low to high in its travel
// (modulators.md §6.2), on kinds that draw it (a knob's mod); a second
// call widens it. Returns nil.
value evaluator::showModulation(callContext& c) {
    if (c.positional.size() != 3 || !c.keywords.empty())
        fail(c.span, "show-modulation takes a control's pipe, a low, and a high");
    const value& v = c.positional[0];
    if (v.k != kind::pipe || !controls_.contains(v.pipe))
        fail(c.positionalSpans[0], "show-modulation expects a page control's pipe; got ", show(v));
    for (std::size_t i = 1; i < 3; ++i) {
        if (!c.positional[i].isPlainNumber())
            fail(c.positionalSpans[i], "show-modulation expects plain numbers; got ", show(c.positional[i]));
    }
    elementObject& control = *controls_.at(v.pipe);
    const auto kind = metadata::findPageKind(control.node.kind);
    if (!kind || !kind->get().param("mod")) return {};
    double low = c.positional[1].asDouble();
    double high = c.positional[2].asDouble();
    if (const auto it = control.node.params.find("mod"); it != control.node.params.end()) {
        const auto& was = std::get<std::vector<double>>(it->second);
        low = std::min(low, was.at(0));
        high = std::max(high, was.at(1));
    }
    control.node.params["mod"] = std::vector<double>{low, high};
    return {};
}

// (control-range pipe [what]): the range of the page control whose pipe
// @p pipe is, as [min max scale step]: min and max in canonical units
// without them, scale :linear, :exp, or :db, and step 0 or the spacing of
// its values (modulators.md §5.3), for std/modulators to move a knob's
// value through its travel. A switch runs 0 to 1, a select over its
// options, and a stepped knob over its steps. @p what names the slot in
// the error when @p pipe is no control's.
value evaluator::controlRange(callContext& c) {
    if (c.positional.empty() || c.positional.size() > 2 || !c.keywords.empty())
        fail(c.span, "control-range takes a control's pipe and, optionally, what it is");
    const value& v = c.positional[0];
    if (v.k != kind::pipe || !controls_.contains(v.pipe)) {
        const std::string what = c.positional.size() == 2 ? show(c.positional[1]) + " " : std::string();
        fail(c.span, "the slot ", what, "holds ", show(v),
             ", not a page control: a modulated slot must be one a page draws (it has a :control) and be "
             "drawn, by "
             "synth-panel, pedal, section, or chain, before the instance is played or processed");
    }
    const elementObject& control = *controls_.at(v.pipe);
    const auto kind = metadata::findPageKind(control.node.kind);
    if (!kind) fail(c.span, "control-range: a control of no known kind, ", control.node.kind);
    const auto& spec = kind->get();
    const auto& params = control.node.params;
    const auto number = [&](std::string_view name) {
        if (const auto it = params.find(std::string(name)); it != params.end())
            return std::get<double>(it->second);
        const auto param = spec.param(name);
        return param ? param->get().defaultValue : 0.0;
    };
    const auto listSize = [&](std::string_view name) -> std::size_t {
        const auto it = params.find(std::string(name));
        if (it == params.end()) return 0;
        if (const auto names = metadata::as<std::vector<std::string>>(it->second)) return names->get().size();
        if (const auto numbers = metadata::as<std::vector<double>>(it->second)) return numbers->get().size();
        return 0;
    };
    const auto range = [&](double lo, double hi, std::string_view scale, double step) {
        return makeVector({makeReal(lo), makeReal(hi), textValue(kind::keyword, scale), makeReal(step)});
    };
    const std::string& k = control.node.kind;
    if (k == "toggle" || k == "footswitch" || k == "button") return range(0, 1, "linear", 1);
    if (k == "select") {
        return range(0, static_cast<double>(std::max<std::size_t>(listSize("options"), 1) - 1), "linear", 1);
    }
    if (k == "knob" && listSize("angles") >= 2) {
        return range(0, static_cast<double>(listSize("angles") - 1), "linear", 1);
    }
    if (k == "knob" && number("steps") >= 2) return range(0, number("steps") - 1, "linear", 1);
    std::string scale = "linear";
    if (const auto it = params.find("scale"); it != params.end()) scale = std::get<std::string>(it->second);
    return range(number("min"), number("max"), scale, spec.param("step") ? number("step") : 0.0);
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
    default: {
        if (v.k == kind::boolean) return v.flag ? 1.0 : 0.0;
        if (!v.isNumber()) fail(span, what, " expects a number, got ", show(v));
        // A parameter of a dimension takes a plain number or a quantity of
        // it, before its range is checked: :grid 2ms is not in beats.
        if (v.k == kind::quantity && spec.dim != dimension::number && v.dim != spec.dim) {
            fail(span, what, " takes ",
                 spec.dim == dimension::beats ? std::string("beats") : article(spec.dim), ", not ",
                 article(v.dim));
        }
        const double x = v.asDouble();
        // Written as a test for inclusion so that NaN fails it.
        const bool inRange = x >= spec.min && x <= spec.max;
        if (!inRange) {
            fail(span, what, " is from ", show(makeReal(spec.min)), " to ", show(makeReal(spec.max)),
                 ", got ", show(v));
        }
        return x;
    }
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

// knob, slider, toggle, button, select, xy, keys, and clips: an osc-in on
// the page port at the control's address, which every compile fills in
// once the page is laid out; returns its pipe, for keys the notes
// osc-notes makes of its messages, and for clips the notes of the
// clipPlayer that plays its bank.
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
    } else if (k == "clips") {
        // A clip bank's row (canvas.md §6): the slot the page chooses, from
        // none, the bank's clipPlayer, and the slots it plays and queues,
        // sent back to the row's address under /playing and /queued.
        auto& params = e->node.params;
        if (!params.contains("bank")) {
            const std::string bank = slug(e->node.label, "");
            if (bank.empty()) {
                fail(c.span,
                     "clips takes a label with a letter or digit, or a :bank, to name its bank, such as "
                     "(clips \"Bass\")");
            }
            params["bank"] = bank;
        }
        const double slots = std::get<double>(params.try_emplace("slots", 8.0).first->second);
        const double grid = std::get<double>(params.try_emplace("grid", 16.0).first->second);
        const double initial = std::get<double>(params.try_emplace("initial", -1.0).first->second);
        for (const auto& [name, x] : {std::pair{"slots", slots}, std::pair{"initial", initial}}) {
            if (x != std::floor(x) || e->dims.contains(name))
                fail(c.span, "clips :", name, " is a whole number, got ", show(makeReal(x)));
        }
        if (initial >= slots) fail(c.span, "clips :initial is a slot from 0, below :slots");
        const value select = allocatePipe(metadata::pipeKind::control, 1, false);
        args.emplace_back("value", select);
        args.emplace_back("initial", makeReal(initial));
        e->nodeIds.push_back(*emitPrimitive("oscIn", c, args).text);
        const value notes = allocatePipe(metadata::pipeKind::event, 1, false);
        const value playing = allocatePipe(metadata::pipeKind::control, 1, false);
        const value queued = allocatePipe(metadata::pipeKind::control, 1, false);
        emitPrimitive("clipPlayer", c,
                      {{"bank", textValue(kind::string, std::get<std::string>(params.at("bank")))},
                       {"slots", makeReal(slots)},
                       {"grid", makeQuantity(grid, dimension::beats)},
                       {"select", select},
                       {"out", notes},
                       {"playing", playing},
                       {"queued", queued}});
        for (const auto& [suffix, pipe] : {std::pair{"/playing", playing}, std::pair{"/queued", queued}}) {
            const value address = textValue(kind::string, e->node.address + suffix);
            e->reports.emplace_back(
                *emitPrimitive("oscOut", c, {{"port", page}, {"address", address}, {"value", pipe}}).text,
                suffix);
        }
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

// screen: an area of its :span, labelled so a page's script finds it, with
// no address and no node (canvas.md §2).
value evaluator::pageSpace(const pageKindSpec& spec, callContext& c) {
    const auto e = pageElement(spec, c);
    if (c.positional.size() != (e->labelled ? 1U : 0U)) {
        fail(c.span, pageName(*e), " takes a label and its :span, such as (", pageName(*e),
             " \"Tables\" :span [9 3])");
    }
    if (e->explicitAddress) fail(c.span, pageName(*e), " has no address");
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
    // A pedal, written or one a pedalboard wraps its controls in, places
    // its controls itself.
    if (kind == "pedal") {
        for (const auto& item : items) {
            if (item->node.params.contains("at"))
                failAt(*item, "a pedal places its controls itself, in rows; :at places a section's");
        }
    }
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
        // At the call that was given it, which is the call being evaluated.
        if (item->node.kind == "tab")
            fail(span, "a tab is a part of the page: give it to (page ...), not to a ", languageNameOf(kind));
        if (into.empty()) {
            fail(span, "a ", languageNameOf(kind), " holds ", metadata::joinNames(spec.children), ", not a ",
                 pageName(*item));
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
void evaluator::slotControls(instanceObject& inst, std::vector<std::shared_ptr<elementObject>>& out,
                             std::string_view who, const sourceSpan& span) {
    const std::string name = spoken(*inst.cls->name);
    if (inst.sealed) {
        fail(span, who, " draws the controls of a ", name,
             " whose nodes are built already, so they would move nothing: draw it before (play ...), "
             "(process ...), "
             "(chain ...), or the first use of a modulator's signal");
    }
    if (inst.drawn) fail(span, who, " draws the controls of a ", name, " that are on the page already");
    inst.drawn = true;
    for (std::size_t i = 0; i < inst.cls->slots.size(); ++i) {
        const slotSpec& slot = inst.cls->slots[i];
        value& held = inst.slots[i];
        if (!slot.control || held.isSymbolic()) continue;
        // A beat value follows the tempo, which a knob's range in seconds
        // cannot hold: the slot keeps it and has no control.
        if (held.k == kind::quantity && held.dim == dimension::beats) continue;
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
        // A value given at make outside the control's range widens it, so a
        // valid slot value is never refused for its knob's sake.
        elementObject& drawn = *controls_.at(held.pipe);
        const auto spec = metadata::findPageKind(drawn.node.kind);
        auto& params = drawn.node.params;
        const auto initial = params.find("initial");
        if (spec && initial != params.end() && std::holds_alternative<double>(initial->second)) {
            const double start = std::get<double>(initial->second);
            for (const auto& [end, below] : {std::pair{"min", true}, std::pair{"max", false}}) {
                const auto param = spec->get().param(end);
                if (!param) continue;
                const auto it = params.find(end);
                const double bound =
                    it != params.end() ? std::get<double>(it->second) : param->get().defaultValue;
                if (below ? start < bound : start > bound) params[end] = start;
            }
        }
        out.push_back(controls_.at(held.pipe));
    }
}

// page, panel, section, synth-panel, pedalboard, and pedal: a group of the
// elements given after its label (a page has none), placing each once. A
// pedal, a synth panel, or a section given an instance draws its :control
// slots, and takes its class's name when it has no label.
value evaluator::pageGroupCall(const pageKindSpec& spec, callContext& c) {
    const auto e = pageElement(spec, c);
    if (spec.kind == metadata::pageRoot && e->labelled) {
        fail(c.positionalSpans[0], "page takes its panels, sections, and controls, not a label");
    }
    const bool draws = spec.kind == "pedal" || spec.kind == "synth" || spec.kind == "section";
    std::vector<std::shared_ptr<elementObject>> items;
    std::vector<std::reference_wrapper<instanceObject>> drawn;
    for (std::size_t i = e->labelled ? 1 : 0; i < c.positional.size(); ++i) {
        const value& v = c.positional[i];
        if (draws && v.k == kind::instance) {
            auto& inst = static_cast<instanceObject&>(*v.obj);
            if (!e->labelled) {
                e->node.label = spoken(*inst.cls->name);
                e->labelled = true;
            }
            slotControls(inst, items, languageNameOf(spec.kind), c.positionalSpans[i]);
            drawn.emplace_back(inst);
            continue;
        }
        pageItems(v, c.positionalSpans[i], items);
    }
    for (const auto& item : items) {
        if (item->placed) failAt(*item, pageName(*item), " is placed on the page twice");
        item->placed = true;
    }
    auto group = pageGroup(spec.kind, std::move(items), c.span);
    group->node.label = e->node.label;
    group->node.params = e->node.params;
    for (instanceObject& inst : drawn) inst.drawnIn = group;
    if (spec.kind == metadata::pageRoot) {
        if (page_) fail(c.span, "a piece has one page; this is its second (page ...)");
        page_ = group;
    } else {
        // A group the page never takes joins it at the end (finishPage),
        // so its controls are not lost.
        pageElements_.push_back(group);
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
    // A tab the page never takes is not on it, whether or not the page has tabs.
    for (const auto& e : loose) {
        if (e->node.kind == "tab") failAt(*e, "this tab is not on the page: give it to (page ...)");
    }
    // On a page with tabs, groups the page never takes have no tab to join.
    const bool tabbed =
        page_ && std::ranges::any_of(page_->children, [](const auto& t) { return t->node.kind == "tab"; });
    if (tabbed && !loose.empty()) {
        const auto& first = *loose.front();
        failAt(first, pageName(first),
               " is in no tab: on a page with tabs, place it in one (tab \"Name\" ...)");
    }
    if (!page_ && loose.empty()) return;
    if (!page_) {
        page_ = pageGroup(metadata::pageRoot, loose, loose.front()->span);
    } else if (!loose.empty()) {
        // Controls and displays join the last panel as a last section when
        // it holds sections; the rest, and controls after a pedalboard, are
        // wrapped as page wraps what it is given.
        const bool controlsOnly = std::ranges::all_of(loose, [](const auto& e) {
            const auto kind = metadata::findPageKind(e->node.kind);
            return kind && kind->get().role != pageRole::group;
        });
        const auto last = page_->children.empty() ? nullptr : page_->children.back();
        const bool holdsSections = last && (last->node.kind == "panel" || last->node.kind == "synth");
        if (controlsOnly && holdsSections) {
            last->children.push_back(pageGroup("section", loose, loose.front()->span));
        } else {
            for (auto& child : pageGroup(metadata::pageRoot, loose, loose.front()->span)->children)
                page_->children.push_back(std::move(child));
        }
    }
    routeTabs();
    // Addresses written by hand are taken first, so a derived one never
    // repeats one.
    // The server's reload address is never derived.
    std::set<std::string> taken{std::string(metadata::pageReloadAddress),
                                std::string(metadata::pageClipsAddress)};
    // An element's reports (clips' /playing and /queued) are addresses of
    // its own, claimed with it.
    for (const auto& e : pageElements_) {
        if (!e->explicitAddress) continue;
        taken.insert(e->node.address);
        for (const auto& report : e->reports) taken.insert(e->node.address + report.second);
    }
    using reportList = std::span<const std::pair<std::string, std::string>>;
    const auto claim = [&](const std::string& base, reportList reports = {}) {
        const auto free = [&](const std::string& name) {
            return !taken.contains(name) && std::ranges::none_of(reports, [&](const auto& report) {
                return taken.contains(name + report.second);
            });
        };
        std::string name = base;
        for (int n = 2; !free(name); ++n) name = base + '-' + std::to_string(n);
        taken.insert(name);
        for (const auto& report : reports) taken.insert(name + report.second);
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
            if (spec.role == pageRole::space) return node;
            if (!e.explicitAddress)
                node.address =
                    claim(prefix + '/' + slug(e.node.label, languageNameOf(e.node.kind)), e.reports);
            for (const auto& id : e.nodeIds) addresses[id] = node.address;
            for (const auto& [id, suffix] : e.reports) addresses[id] = node.address + suffix;
            return node;
        };
    graph_.layout = tree(*page_, "");
    // The page is named for its project, so pages of projects served on one
    // port keep what they save apart (espresso-ui.js).
    graph_.layout->label = options_.project;
    // Sizes and places in units (canvas.md §2): a group given a size too
    // small for what it holds is an error at its call.
    const auto problems = metadata::layOut(*graph_.layout);
    if (!problems.empty()) {
        const auto& p = problems.front();
        raiseAt(p.src, fromIrSpan(p.src), "layout: " + p.where + ": " + p.message);
    }
    for (auto& n : graph_.nodes) {
        if (const auto it = addresses.find(n.id); it != addresses.end()) n.params["address"] = it->second;
    }
}

}  // namespace interpreter::detail
