// Pages in the language (dsl-ui.md §4, §5): controls and displays compile
// to the osc-in and osc-out nodes a page talks to, the groups lay them out,
// and the layout reaches the IR with every address filled in.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>

#include "support/unwrap.h"

using Catch::Matchers::ContainsSubstring;
using interpreter::compileOptions;
using interpreter::compileResult;
using metadata::graphSpec;
using metadata::nodeSpec;
using metadata::pageNode;
using metadata::paramValue;
using support::unwrap;

namespace {

// @p body as the body of a piece, in a project on page port 8000 unless
// @p port is false.
compileResult build(const std::string& body, bool port = true) {
    compileOptions options;
    if (port) options.pagePort = 8000;
    return interpreter::compile("(defpiece main ()\n" + body + ")\n", "main.lisp", options);
}

graphSpec graph(const std::string& body) {
    const auto r = build(body);
    INFO(r.text());
    REQUIRE(r.ok());
    return unwrap(r.graph);
}

std::string errorOf(const std::string& body, bool port = true) {
    const auto r = build(body, port);
    REQUIRE_FALSE(r.ok());
    return r.text();
}

const pageNode& layout(const graphSpec& g) { return unwrap(g.layout); }

// The page's first section's elements.
const std::vector<pageNode>& firstSection(const graphSpec& g) {
    const pageNode& page = layout(g);
    REQUIRE(page.children.size() >= 1);
    REQUIRE(page.children[0].children.size() >= 1);
    return page.children[0].children[0].children;
}

// The nodes of @p type, in order.
std::vector<std::reference_wrapper<const nodeSpec>> nodes(const graphSpec& g, const std::string& type) {
    std::vector<std::reference_wrapper<const nodeSpec>> out;
    for (const auto& n : g.nodes) {
        if (n.type == type) out.emplace_back(n);
    }
    return out;
}

std::string text(const nodeSpec& n, const std::string& name) {
    return std::get<std::string>(n.params.at(name));
}
double number(const nodeSpec& n, const std::string& name) { return std::get<double>(n.params.at(name)); }

// A sine at frequency @p freq, so a control has a reader.
std::string tone(const std::string& freq = "pitch") {
    return "(sine-osc :freq " + freq + " :out (pipe :audio))\n";
}

}  // namespace

TEST_CASE("a knob is an osc-in on the page port at its address, from its starting value",
          "[interpreter][page]") {
    const auto g = graph("(let ((pitch (knob \"Pitch\" :min 55hz :max 440hz :initial 110hz)))\n" + tone() +
                         R"((page (panel "Synth" (section "Oscillators" pitch)))))");
    const auto ins = nodes(g, "oscIn");
    REQUIRE(ins.size() == 1);
    CHECK(text(ins[0], "address") == "/oscillators/pitch");
    CHECK(number(ins[0], "port") == 8000);
    CHECK(number(ins[0], "initial") == 110);

    const pageNode& knob = firstSection(g).at(0);
    CHECK(knob.kind == "knob");
    CHECK(knob.label == "Pitch");
    CHECK(knob.address == "/oscillators/pitch");
    CHECK(knob.params.at("min") == paramValue(55.0));
    CHECK(knob.params.at("initial") == paramValue(110.0));
    // A frequency shows in hertz, on an exponential scale.
    CHECK(knob.params.at("unit") == paramValue(std::string("hz")));
    CHECK(knob.params.at("scale") == paramValue(std::string("exp")));
    CHECK(layout(g).children[0].label == "Synth");
    CHECK(layout(g).children[0].children[0].label == "Oscillators");
}

TEST_CASE("a level shows in decibels; a control without :initial starts at :min", "[interpreter][page]") {
    const auto g = graph("(let ((pitch (knob \"Volume\" :max 0db)) (q (slider \"Q\" :min 1 :max 10)))\n" +
                         tone() + "(sine-osc :freq q :out (pipe :audio)))");
    const auto& section = firstSection(g);
    CHECK(section.at(0).params.at("scale") == paramValue(std::string("db")));
    CHECK(section.at(0).params.at("unit") == paramValue(std::string("db")));
    CHECK(section.at(0).params.at("initial") == paramValue(0.0));
    CHECK(section.at(1).params.at("initial") == paramValue(1.0));
    CHECK_FALSE(section.at(1).params.contains("unit"));
    CHECK(number(nodes(g, "oscIn")[1], "initial") == 1);
}

TEST_CASE("a meter's range in hertz takes the exponential scale and no unit, which a meter has not",
          "[interpreter][page]") {
    const auto g = graph(R"((let ((f (pipe :control)))
  (page (meter "Travel" f :min 200hz :max 4khz))))");
    const pageNode& meter = firstSection(g).at(0);
    CHECK(meter.params.at("scale") == paramValue(std::string("exp")));
    CHECK(meter.params.at("max") == paramValue(4000.0));
    CHECK_FALSE(meter.params.contains("unit"));
}

TEST_CASE("addresses come from labels under their sections' labels, a repeated one taking -2",
          "[interpreter][page]") {
    const auto g =
        graph("(let ((pitch (knob \"Cut-off Freq.\")) (b (knob \"Cut-off Freq.\")) (c (knob \"Rate\"))"
              "      (d (knob \"Rate\")) (e (knob \"Gain\" :address \"/sweep/rate\")) (f (knob)))\n" +
              tone() +
              "(sine-osc :freq b :out (pipe :audio)) (sine-osc :freq c :out (pipe :audio))"
              "(sine-osc :freq d :out (pipe :audio)) (sine-osc :freq e :out (pipe :audio))"
              "(sine-osc :freq f :out (pipe :audio))"
              "(page (panel (section pitch b f) (section \"Sweep\" c) (section \"Sweep\" d e))))");
    const auto& panel = layout(g).children[0];
    CHECK(panel.children[0].children[0].address == "/cut-off-freq");
    CHECK(panel.children[0].children[1].address == "/cut-off-freq-2");
    CHECK(panel.children[0].children[2].address == "/knob");
    // A derived address never repeats one written by hand.
    CHECK(panel.children[1].children[0].address == "/sweep/rate-2");
    CHECK(panel.children[2].children[0].address == "/sweep-2/rate");
    CHECK(panel.children[2].children[1].address == "/sweep/rate");
    std::vector<std::string> read;
    for (const nodeSpec& n : nodes(g, "oscIn")) read.push_back(text(n, "address"));
    CHECK(read == std::vector<std::string>{"/cut-off-freq", "/cut-off-freq-2", "/sweep/rate-2",
                                           "/sweep-2/rate", "/sweep/rate", "/knob"});
}

TEST_CASE("a piece's controls make a page without (page ...), in the order made", "[interpreter][page]") {
    const auto g = graph("(let ((pitch (knob \"Pitch\")) (level (pipe :control)))\n" + tone() +
                         "(meter \"Level\" level) (smoother :in pitch :time 10ms :out level))");
    const pageNode& page = layout(g);
    CHECK(page.kind == "page");
    REQUIRE(page.children.size() == 1);
    CHECK(page.children[0].kind == "panel");
    const auto& section = page.children[0].children.at(0);
    CHECK(section.kind == "section");
    CHECK(section.label.empty());
    REQUIRE(section.children.size() == 2);
    CHECK(section.children[0].address == "/pitch");
    CHECK(section.children[1].kind == "meter");
    CHECK(text(nodes(g, "oscOut").at(0), "address") == "/level");
}

TEST_CASE("a group wraps what it cannot hold in what it holds", "[interpreter][page]") {
    const auto g = graph("(let ((pitch (knob \"Pitch\")) (q (knob \"Q\")))\n" + tone() +
                         "(sine-osc :freq q :out (pipe :audio)) (page pitch (section \"Filter\" q)))");
    const pageNode& panel = layout(g).children.at(0);
    REQUIRE(panel.children.size() == 2);
    CHECK(panel.children[0].label.empty());
    CHECK(panel.children[0].children.at(0).label == "Pitch");
    CHECK(panel.children[1].children.at(0).address == "/filter/q");
}

TEST_CASE("controls placed nowhere join the last panel as a last section", "[interpreter][page]") {
    const auto g =
        graph("(let ((pitch (knob \"Pitch\")) (q (knob \"Q\")))\n" + tone() +
              R"((sine-osc :freq q :out (pipe :audio)) (page (panel "Synth" (section "A" pitch)))))");
    const pageNode& panel = layout(g).children.at(0);
    REQUIRE(panel.children.size() == 2);
    CHECK(panel.children[1].label.empty());
    CHECK(panel.children[1].children.at(0).address == "/q");
}

TEST_CASE("a list of controls is placed as its items", "[interpreter][page]") {
    const auto g =
        graph("(let ((bars (map (fn (n) (slider (string-append \"Bar \" n) :max 8)) [\"1\" \"2\" \"3\"])))\n"
              "(sine-osc :freq (nth bars 0) :out (pipe :audio))"
              "(sine-osc :freq (nth bars 1) :out (pipe :audio))"
              "(sine-osc :freq (nth bars 2) :out (pipe :audio))"
              "(page (section \"Drawbars\" bars :columns 6)))");
    const auto& section = firstSection(g);
    REQUIRE(section.size() == 3);
    CHECK(section[2].address == "/drawbars/bar-3");
    CHECK(layout(g).children[0].children[0].params.at("columns") == paramValue(6.0));
}

TEST_CASE("each kind of control compiles to its nodes", "[interpreter][page]") {
    const auto g = graph(
        "(let ((on (toggle \"On\" :initial #t)) (wave (select \"Wave\" :options [\"sine\" \"saw\"] :initial "
        "1))"
        "      (tap (button \"Tap\")) (pad (xy \"Pad\" :initial 0.5)) (notes (keys :low 60 :high 72)))\n"
        "(gain :in (pipe :audio) :amount on :out (pipe :audio))"
        "(gain :in (pipe :audio) :amount wave :out (pipe :audio))"
        "(gain :in (pipe :audio) :amount tap :out (pipe :audio))"
        "(smoother :in pad :time 10ms :out (pipe :control :channels 2))"
        "(midi-out :in notes))");
    const auto& section = firstSection(g);
    REQUIRE(section.size() == 5);
    CHECK(section[0].params.at("initial") == paramValue(1.0));
    CHECK(section[1].params.at("options") == paramValue(std::vector<std::string>{"sine", "saw"}));
    CHECK(section[3].params.at("initial") == paramValue(std::vector<double>{0.5, 0.5}));
    const auto ins = nodes(g, "oscIn");
    REQUIRE(ins.size() == 5);
    CHECK(number(ins[0], "initial") == 1);
    CHECK(number(ins[1], "initial") == 1);
    CHECK(number(ins[2], "initial") == 0);
    // An xy pad's pipe has a channel for each axis.
    const auto pipe = ins[3].get().outputs.at("value").at(0);
    CHECK(unwrap(metadata::findPipe(g, pipe)).get().channels == 2);
    // A keyboard's messages become notes at its address.
    CHECK(ins[4].get().outputs.contains("out"));
    CHECK(text(ins[4], "address") == "/keys");
    CHECK(text(nodes(g, "oscNotes").at(0), "address") == "/keys");
    CHECK(section[4].params.at("low") == paramValue(60.0));
}

TEST_CASE("a layout compiles the same every time", "[interpreter][page]") {
    const std::string body = "(let ((pitch (knob \"Pitch\" :min 20hz :max 2khz)) (level (pipe :control)))\n" +
                             tone() +
                             "(smoother :in pitch :time 10ms :out level)"
                             "(page (panel \"A\" (section \"B\" pitch (meter \"Level\" level)))))";
    CHECK(ir::write(graph(body)) == ir::write(graph(body)));
}

TEST_CASE("the page's mistakes are compile errors at their calls", "[interpreter][page]") {
    CHECK_THAT(errorOf("(knob \"Pitch\")", false),
               ContainsSubstring("main.lisp:2:1: knob is on the page, but the project has no page port"));
    CHECK_THAT(errorOf("(let ((pitch (knob \"Pitch\")))" + tone() + "(page pitch) (page))"),
               ContainsSubstring("a piece has one page"));
    CHECK_THAT(
        errorOf("(let ((pitch (knob \"Pitch\")))" + tone() + "(page (section pitch) (section pitch)))"),
        ContainsSubstring("knob is placed on the page twice"));
    CHECK_THAT(errorOf("(page (section \"A\" 5))"),
               ContainsSubstring("expected a control, a display, or a group"));
    CHECK_THAT(errorOf("(page (section \"A\" (panel)))"), ContainsSubstring("a section holds knob,"));
    CHECK_THAT(errorOf("(knob \"Pitch\" :colour 1)"), ContainsSubstring("knob has no :colour"));
    CHECK_THAT(errorOf("(knob \"Pitch\" :scale :log)"),
               ContainsSubstring("knob :scale expects one of :linear, exp, db"));
    CHECK_THAT(
        errorOf("(knob \"Pitch\" :min 20hz :max 2s)"),
        ContainsSubstring("knob :min, :max, and :initial must be of one kind: a frequency and a time"));
    CHECK_THAT(errorOf("(knob \"Pitch\" 5)"), ContainsSubstring("knob takes a label and keyword arguments"));
    CHECK_THAT(errorOf("(meter \"Level\")"), ContainsSubstring("meter takes a label and the pipe it shows"));
    CHECK_THAT(errorOf("(knob \"Pitch\" :address \"pitch\")"), ContainsSubstring("starting with /"));
    CHECK_THAT(errorOf("(xy \"Pad\" :initial [0.2 0.8])"),
               ContainsSubstring("xy :initial must be one number"));
    CHECK_THAT(errorOf("(page \"Title\")"),
               ContainsSubstring("page takes its panels, sections, and controls"));
    // What the IR's checks find points at the control.
    CHECK_THAT(
        errorOf("(let ((pitch (knob \"Pitch\" :initial 2)))" + tone() + ")"),
        ContainsSubstring("main.lisp:2:14: layout: page > panel > section > knob \"Pitch\": initial 2 is "
                          "outside 0 to 1"));
}

TEST_CASE("synth panels, pedals, and pedalboards hold the kits' controls (dsl-ui.md 6)",
          "[interpreter][page]") {
    const auto g = graph("(let ((pitch (drawbar \"16'\" :initial 8)) (gain (knob \"Gain\")) (on "
                         "(footswitch)) (time (knob \"Time\")))\n" +
                         tone() +
                         "(sine-osc :freq gain :out (pipe :audio)) (sine-osc :freq on :out (pipe :audio))"
                         "(sine-osc :freq time :out (pipe :audio))"
                         "(page (synth-panel \"Organ\" (section \"Bars\" pitch) :maker \"Nine bars\")"
                         "      (pedal \"Fuzz\" gain on) (pedal \"Fuzz\" time)))");
    const pageNode& page = layout(g);
    REQUIRE(page.children.size() == 2);
    const pageNode& synth = page.children[0];
    CHECK(synth.kind == "synth");
    CHECK(synth.params.at("maker") == paramValue(std::string("Nine bars")));
    CHECK(synth.children.at(0).children.at(0).params.at("initial") == paramValue(8.0));
    // Pedals given to the page are placed on one pedalboard, each its
    // label's address prefix, a second of a name taking -2.
    const pageNode& board = page.children[1];
    CHECK(board.kind == "pedalboard");
    REQUIRE(board.children.size() == 2);
    CHECK(board.children[0].children.at(0).address == "/fuzz/gain");
    CHECK(board.children[0].children.at(1).kind == "footswitch");
    CHECK(board.children[0].children.at(1).address == "/fuzz/footswitch");
    CHECK(board.children[1].children.at(0).address == "/fuzz-2/time");
    // A footswitch starts engaged.
    CHECK(board.children[0].children.at(1).params.at("initial") == paramValue(1.0));
    CHECK(number(nodes(g, "oscIn").at(2), "initial") == 1);
}

TEST_CASE("a pedal holds controls, not groups", "[interpreter][page]") {
    CHECK_THAT(errorOf("(page (pedal \"Fuzz\" (section \"A\")))"), ContainsSubstring("a pedal holds knob,"));
}

namespace {

// A project of @p body after std/effects' import and @p classes.
graphSpec withEffects(const std::string& classes, const std::string& body) {
    compileOptions options;
    options.pagePort = 8000;
    const auto r =
        interpreter::compile("(import \"std/effects\")\n" + classes + "\n(defpiece main ()\n" + body + ")\n",
                             "main.lisp", options);
    INFO(r.text());
    REQUIRE(r.ok());
    return unwrap(r.graph);
}

std::vector<std::string> labels(const pageNode& group) {
    std::vector<std::string> out;
    out.reserve(group.children.size());
    for (const auto& c : group.children) out.push_back(c.label);
    return out;
}

}  // namespace

TEST_CASE("chain runs every standard effect in series as pedals on a pedalboard (dsl-ui.md 5.3)",
          "[interpreter][page]") {
    const auto g =
        withEffects("", "(let ((in (pipe :audio)) (out (pipe :audio)))"
                        "(audio-in :channel 0 :out in) (audio-out :in out :channel 0)"
                        "(page (chain in out (make echo :feedback 0.7) (make chorus) (make reverb)"
                        "  (make overdrive) (make granular-delay) (make spectral-delay) (make freeze))))");
    const pageNode& board = layout(g).children.at(0);
    CHECK(board.kind == "pedalboard");
    CHECK(labels(board) == std::vector<std::string>{"Echo", "Chorus", "Reverb", "Overdrive", "Granular delay",
                                                    "Spectral delay", "Freeze"});
    const pageNode& echo = board.children[0];
    CHECK(labels(echo) == std::vector<std::string>{"Feedback", "Damping", "Mix", "On"});
    // A slot's value starts its knob.
    CHECK(echo.children[0].params.at("initial") == paramValue(0.7));
    CHECK(echo.children[0].address == "/echo/feedback");
    CHECK(echo.children[3].kind == "footswitch");
    CHECK(labels(board.children[3]) == std::vector<std::string>{"Drive", "Tone", "Level", "Mix", "On"});
    CHECK(labels(board.children[6]) == std::vector<std::string>{"Hold", "Mix", "On"});
}

TEST_CASE("a class's :control slots are its pedal's controls, inherited as initforms are (dsl-ui.md 5.2)",
          "[interpreter][page]") {
    const std::string classes =
        "(defclass fuzz (effect) ((drive :initarg :drive :initform 0.6 :accessor fuzz-drive"
        "                          :control (knob \"Gain\" :unit :percent))"
        "                         (mix :initform 1 :control (knob \"Blend\"))))\n"
        "(defclass loud-fuzz (fuzz) ())\n"
        "(defmethod process ((f fuzz) in out) (gain :in in :amount (fuzz-drive f) :out out))";
    const auto g =
        withEffects(classes, "(let ((in (pipe :audio)) (out (pipe :audio)) (wet (pipe :control)))"
                             "(audio-in :channel 0 :out in) (audio-out :in out :channel 0)"
                             "(page (pedal (make loud-fuzz :drive 0.2)) (pedal \"Odd\" (make fuzz :mix wet)))"
                             "(process (make fuzz) in out))");
    const pageNode& board = layout(g).children.at(0);
    const pageNode& loud = board.children.at(0);
    CHECK(loud.label == "Loud fuzz");
    CHECK(labels(loud) == std::vector<std::string>{"Gain", "Blend"});
    CHECK(loud.children[0].params.at("initial") == paramValue(0.2));
    // A slot that holds a pipe is not drawn.
    CHECK(labels(board.children.at(1)) == std::vector<std::string>{"Gain"});
    CHECK(board.children[1].label == "Odd");
}

TEST_CASE("a synth panel given an instance draws its :control slots in a section", "[interpreter][page]") {
    const auto g = withEffects("", "(let ((in (pipe :audio)) (out (pipe :audio)) (e (make echo)))"
                                   "(audio-in :channel 0 :out in) (audio-out :in out :channel 0)"
                                   "(page (synth-panel e)) (process e in out))");
    const pageNode& synth = layout(g).children.at(0);
    CHECK(synth.kind == "synth");
    CHECK(synth.label == "Echo");
    CHECK(labels(synth.children.at(0)) == std::vector<std::string>{"Feedback", "Damping", "Mix"});
}

TEST_CASE("a slot's :control is a page control's call", "[interpreter][page]") {
    compileOptions options;
    options.pagePort = 8000;
    const auto bad =
        interpreter::compile("(defclass a () ((x :control 5)))\n(defpiece main ())\n", "main.lisp", options);
    CHECK_THAT(bad.text(), ContainsSubstring(":control takes a page control's call"));
    const auto other = interpreter::compile(
        "(defclass a () ((x :initform 1 :control (list 1))))\n(defpiece main () (page (pedal (make a))))\n",
        "main.lisp", options);
    CHECK_THAT(other.text(), ContainsSubstring("slot x's :control must make a page control"));
}

TEST_CASE("a page takes a theme and an accent", "[interpreter][page]") {
    const auto g =
        graph("(let ((pitch (knob \"Pitch\")))" + tone() + "(page pitch :theme :paper :accent \"#ff8800\"))");
    CHECK(layout(g).params.at("theme") == paramValue(std::string("paper")));
    CHECK(layout(g).params.at("accent") == paramValue(std::string("#ff8800")));
}

TEST_CASE("a section given an instance draws its :control slots, as a modulator's (modulators.md 6.1)",
          "[interpreter][page]") {
    const auto g = withEffects("(import \"std/modulators\")",
                               "(let ((wobble (make lfo-mod)) (g (knob \"Gain\"))"
                               "      (in (pipe :audio)) (out (pipe :audio)))"
                               "(audio-in :channel 0 :out in) (audio-out :in out :channel 0)"
                               "(page (panel \"Mods\" (section \"LFO\" wobble) (section g)))"
                               "(gain :in in :amount (modulated g wobble 0.5) :out out))");
    const pageNode& section = layout(g).children.at(0).children.at(0);
    CHECK(section.label == "LFO");
    CHECK(labels(section) == std::vector<std::string>{"Rate"});
    CHECK(section.children.at(0).address == "/lfo/rate");
    // The LFO's rate is the knob's pipe, not a constant.
    const auto rateIn = std::ranges::find_if(g.nodes, [](const nodeSpec& n) {
        return n.type == "oscIn" && std::get<std::string>(n.params.at("address")) == "/lfo/rate";
    });
    REQUIRE(rateIn != g.nodes.end());
    const auto lfo = nodes(g, "lfo");
    REQUIRE(lfo.size() == 1);
    CHECK(lfo[0].get().params.at("rate") == paramValue(rateIn->outputs.at("value").at(0)));
}

TEST_CASE("modulate needs its slot drawn on the page, and says so", "[interpreter][page]") {
    compileOptions options;
    options.pagePort = 8000;
    const auto r =
        interpreter::compile("(import \"std/effects\")\n(import \"std/modulators\")\n(defpiece main ()\n"
                             "(let ((in (pipe :audio)) (out (pipe :audio)))"
                             "(audio-in :channel 0 :out in) (audio-out :in out :channel 0)"
                             "(process (modulate (make reverb) 'mix (make lfo-mod) 0.2) in out)))\n",
                             "main.lisp", options);
    REQUIRE_FALSE(r.ok());
    CHECK_THAT(r.text(), ContainsSubstring("a modulated slot must be one a page draws"));
}

TEST_CASE("a modulated knob carries the span its modulations reach (modulators.md 6.2)",
          "[interpreter][page]") {
    const auto g =
        withEffects("(import \"std/modulators\")",
                    "(let ((wobble (make lfo-mod)) (g (knob \"Gain\" :initial 0.5))"
                    "      (in (pipe :audio)) (out (pipe :audio)))"
                    "(audio-in :channel 0 :out in) (audio-out :in out :channel 0)"
                    "(page (section g))"
                    "(gain :in in :amount (modulated g wobble 0.5) :out out)"
                    "(gain :in in :amount (modulated g wobble 0.2 :polarity ':unipolar) :out out))");
    const pageNode& knob = firstSection(g).at(0);
    REQUIRE(knob.params.contains("mod"));
    CHECK(knob.params.at("mod") == paramValue(std::vector<double>{-0.25, 0.25}));
}

namespace {

// The error of a project of @p body after the library imports.
std::string libraryError(const std::string& body) {
    compileOptions options;
    options.pagePort = 8000;
    const auto r = interpreter::compile("(import \"std/instruments\")\n(import \"std/effects\")\n(import "
                                        "\"std/modulators\")\n(defpiece main ()\n" +
                                            body + ")\n",
                                        "main.lisp", options);
    REQUIRE_FALSE(r.ok());
    return r.text();
}

}  // namespace

TEST_CASE("groups the page never takes join it, and controls after a pedalboard get a panel of their own",
          "[interpreter][page]") {
    // A chain with no page: its pedals are the page.
    const auto alone = withEffects("", "(let ((in (pipe :audio)) (out (pipe :audio)))"
                                       "(audio-in :channel 0 :out in) (audio-out :in out :channel 0)"
                                       "(chain in out (make reverb)))");
    CHECK(layout(alone).children.at(0).kind == "pedalboard");
    for (const auto& n : nodes(alone, "oscIn"))
        CHECK_FALSE(std::get<std::string>(n.get().params.at("address")).empty());
    // A loose knob and a page that ends in a pedalboard: the knob gets a panel.
    const auto after = withEffects("", "(let ((in (pipe :audio)) (out (pipe :audio)))"
                                       "(saw-osc :freq (knob \"Freq\" :min 50hz :max 500hz) :out in)"
                                       "(audio-out :in out :channel 0) (page (chain in out (make reverb))))");
    REQUIRE(layout(after).children.size() == 2);
    CHECK(layout(after).children.at(1).kind == "panel");
    // A section defined and never placed joins the page with its knob.
    const auto g = graph("(let ((gain (knob \"Gain\")) (pitch (knob \"Pitch\")))\n" + tone() + tone("gain") +
                         R"((define lost (section "Lost" gain)) (page (panel "P" (section "S" pitch)))))");
    CHECK(layout(g).children.size() == 2);
}

TEST_CASE("no derived address is the server's reload address", "[interpreter][page]") {
    const auto g = graph(R"((let ((h (pipe :control))) (page (section "Espresso" (meter "Reload" h)))))");
    CHECK(text(nodes(g, "oscOut").at(0), "address") == "/espresso/reload-2");
    CHECK_THAT(errorOf("(osc-out :port :page :address \"/espresso/reload\" :value (pipe :control))"),
               ContainsSubstring("is the server's, for its own messages to pages"));
}

TEST_CASE("drawing or modulating an instance after its nodes are built is an error, not a dead knob",
          "[interpreter][page]") {
    CHECK_THAT(
        libraryError("(let ((s (make subtractive)) (n (keys \"K\")) (o (pipe :audio)))"
                     "(play s n o) (page (synth-panel \"S\" s)))"),
        ContainsSubstring("synth-panel draws the controls of a Subtractive whose nodes are built already"));
    CHECK_THAT(libraryError(
                   "(let ((l (make lfo-mod)) (k (knob \"K\")) (o (pipe :audio)))"
                   "(gain :in o :amount (modulated k l 1) :out (pipe :audio)) (page (section \"LFO\" l) k))"),
               ContainsSubstring("section draws the controls of a Lfo mod whose nodes are built already"));
    CHECK_THAT(
        libraryError("(let ((d (pipe :audio)) (o (pipe :audio)) (r (make reverb))) (page (chain d o r r)))"),
        ContainsSubstring("pedal draws the controls of a Reverb whose nodes are built already"));
    CHECK_THAT(libraryError("(let ((d (pipe :audio)) (o (pipe :audio)) (r (make reverb)))"
                            "(page (chain d o r)) (modulate r 'mix (make lfo-mod) 0.2))"),
               ContainsSubstring("modulate moves mix of an instance whose nodes are built already"));
    CHECK_THAT(libraryError("(let ((d (pipe :audio)) (o (pipe :audio))) (page (chain d o)))"),
               ContainsSubstring("chain needs at least one effect"));
    CHECK_THAT(
        libraryError("(let ((k (knob \"K\")) (o (pipe :audio)))"
                     "(gain :in o :amount (modulated k (make step-mod :steps []) 1) :out (pipe :audio)))"),
        ContainsSubstring("step-mod needs at least one step"));
}

TEST_CASE("a slot given a beat value keeps it and has no knob; a step source's rate can be its knob",
          "[interpreter][page]") {
    const auto g =
        withEffects("(import \"std/modulators\")",
                    "(let ((d (pipe :audio)) (o (pipe :audio)) (k (knob \"K\")) (s (make step-mod)))"
                    "(audio-in :channel 0 :out d) (audio-out :in o :channel 0)"
                    "(page (chain d o (make spectral-delay :time 0.5beat)) (section \"Steps\" s) k)"
                    "(gain :in d :amount (modulated k s 1) :out (pipe :audio)))");
    const pageNode& pedal = layout(g).children.at(0).children.at(0);
    CHECK(labels(pedal) == std::vector<std::string>{"Spread", "Feedback", "Mix", "On"});
}

TEST_CASE("a switch modulates over 0 to 1, landing on its steps; one LFO moves another's rate",
          "[interpreter][page]") {
    const auto g =
        withEffects("(import \"std/modulators\")",
                    "(let ((d (pipe :audio)) (o (pipe :audio)) (slow (make lfo-mod)) (fast (make lfo-mod)))"
                    "(audio-in :channel 0 :out d) (audio-out :in o :channel 0)"
                    "(modulate fast 'rate slow 0.5)"
                    "(page (section \"Slow\" slow) (section \"Fast\" fast)"
                    "  (chain d o (modulate (make freeze) 'hold fast 1 :polarity ':unipolar))))");
    // The freeze's hold reads a pipe, not the constant 0 a range of 0 to 0 gave.
    const auto freeze = nodes(g, "pvFreeze");
    REQUIRE(freeze.size() == 1);
    CHECK_FALSE(std::holds_alternative<double>(freeze[0].get().params.at("hold")));
    // Two LFOs: the fast one's rate is a pipe, moved by the slow one.
    const auto lfos = nodes(g, "lfo");
    REQUIRE(lfos.size() == 2);
    CHECK(std::ranges::count_if(lfos, [](const auto& n) {
              return !std::holds_alternative<double>(n.get().params.at("rate"));
          }) == 2);
}

TEST_CASE("a slot's value outside its knob's range widens the range", "[interpreter][page]") {
    compileOptions options;
    options.pagePort = 8000;
    const auto r =
        interpreter::compile("(import \"std/instruments\")\n(defpiece main ()\n"
                             "(let ((s (make pluck :feedback 0.8)) (n (keys \"K\")) (o (pipe :audio)))"
                             "(page (synth-panel \"P\" s)) (play s n o)))\n",
                             "main.lisp", options);
    INFO(r.text());
    REQUIRE(r.ok());
    const pageNode& feedback = layout(unwrap(r.graph)).children.at(0).children.at(0).children.at(0);
    CHECK(feedback.label == "Feedback");
    CHECK(feedback.params.at("min") == paramValue(0.8));
    CHECK(feedback.params.at("initial") == paramValue(0.8));
}

TEST_CASE("a group given a size too small for what it holds is an error at its call (canvas.md 2.3)",
          "[interpreter][page]") {
    CHECK_THAT(
        errorOf(R"((let ((k (knob "K"))) (sine-osc :freq k :out (pipe :audio))
  (page (panel "P" (section "S" k :size [2 2])))))"),
        ContainsSubstring("main.lisp:3:20: layout: page > panel \"P\" > section \"S\": size is 2 × 2 units; "
                          "what it holds needs 3 × 4 units"));
    // A size large enough is kept; the controls take their places.
    const auto g = graph(R"((let ((k (knob "K"))) (sine-osc :freq k :out (pipe :audio))
  (page (panel "P" (section "S" k :size [6 4])))))");
    const pageNode& section = layout(g).children.at(0).children.at(0);
    CHECK(section.params.at("size") == paramValue(std::vector<double>{6, 4}));
    CHECK(firstSection(g).at(0).params.at("at") == paramValue(std::vector<double>{1, 1}));
}

TEST_CASE("a screen is an area of its span for a page's script, with no address", "[interpreter][page]") {
    const auto g = graph(R"((let ((k (knob "K"))) (sine-osc :freq k :out (pipe :audio))
  (page (panel "P" (section "S" (screen "Tables" :span [6 2]) k)))))");
    const pageNode& screen = firstSection(g).at(0);
    CHECK(screen.kind == "screen");
    CHECK(screen.address.empty());
    CHECK(screen.params.at("span") == paramValue(std::vector<double>{6, 2}));
    CHECK(firstSection(g).at(1).params.at("at") == paramValue(std::vector<double>{7, 1}));
    CHECK_THAT(errorOf(R"((page (screen "T" :address "/t")))"), ContainsSubstring("screen has no address"));
}

TEST_CASE("tabs show the tabs their audio comes from and goes to (canvas.md 4.2)", "[interpreter][page]") {
    compileOptions options;
    options.pagePort = 8000;
    const auto r = interpreter::compile(R"((import "std/instruments")
(import "std/effects")
(defpiece main ()
  (let ((bass (make pluck)) (n (keys "Keys" :low 36 :high 48))
        (b (pipe :audio)) (bus (pipe :audio)) (out (pipe :audio)))
    (define panel (synth-panel "Bass" bass n))
    (play bass n b)
    (define bass-chain (chain b bus (make echo)))
    (define master (chain bus out (make overdrive)))
    (audio-out :in out :channel 0)
    (page (tab "Bass" panel bass-chain) (tab "Master" master)))))",
                                        "main.lisp", options);
    INFO(r.text());
    REQUIRE(r.ok());
    const pageNode& page = layout(unwrap(r.graph));
    REQUIRE(page.children.size() == 2);
    const pageNode& bass = page.children[0];
    const pageNode& master = page.children[1];
    CHECK(bass.kind == "tab");
    CHECK_FALSE(bass.params.contains("from"));
    CHECK(bass.params.at("to") == paramValue(std::vector<std::string>{"Master"}));
    CHECK(master.params.at("from") == paramValue(std::vector<std::string>{"Bass"}));
    CHECK(master.params.at("to") == paramValue(std::vector<std::string>{"output"}));
}

TEST_CASE("a tab has a label of its own, and goes on the page", "[interpreter][page]") {
    CHECK_THAT(errorOf(R"((page (tab (panel "P" (section "S" (knob "K"))))))"),
               ContainsSubstring("a tab takes a label"));
    CHECK_THAT(errorOf(R"((page (tab "A" (panel "P" (section "S" (knob "K"))))
      (tab "A" (panel "Q" (section "T" (knob "J"))))))"),
               ContainsSubstring("a page has one tab named \"A\""));
    CHECK_THAT(errorOf(R"((page (panel "P" (tab "X" (panel "Q" (section "S" (knob "K")))))))"),
               ContainsSubstring("a tab is a part of the page"));
    CHECK_THAT(errorOf(R"((page (tab "A" (panel "P" (section "S" (knob "K")))))
  (tab "B" (panel "Q" (section "T" (knob "J")))))"),
               ContainsSubstring("this tab is not on the page"));
}

TEST_CASE("a page with tabs holds only tabs, and every group is in one", "[interpreter][page]") {
    CHECK_THAT(errorOf(R"((let ((k (knob "K"))) (sine-osc :freq k :out (pipe :audio))
  (page (tab "A" (panel "P" (section "S" k))) (panel "Q"))))"),
               ContainsSubstring("a page with tabs holds only tabs"));
    CHECK_THAT(errorOf(R"((let ((k (knob "K")) (j (knob "J"))) (sine-osc :freq k :out (pipe :audio))
  (sine-osc :freq j :out (pipe :audio))
  (page (tab "A" (panel "P" (section "S" k))))))"),
               ContainsSubstring("is in no tab"));
}

TEST_CASE("a clips row plays its bank and reports what plays, stacking with other rows (canvas.md 6)",
          "[interpreter][page]") {
    const auto g = graph(R"((let ((bass (clips "Bass" :slots 4 :grid 8beat)) (pad (clips "Pad" :bank "pads")))
  (page (panel "P" (section "Loops" bass pad)))))");
    const auto players =
        std::ranges::count_if(g.nodes, [](const nodeSpec& n) { return n.type == "clipPlayer"; });
    CHECK(players == 2);
    const nodeSpec& bass =
        *std::ranges::find_if(g.nodes, [](const nodeSpec& n) { return n.type == "clipPlayer"; });
    CHECK(bass.params.at("bank") == paramValue(std::string("bass")));
    CHECK(bass.params.at("slots") == paramValue(4.0));
    CHECK(bass.params.at("grid") == paramValue(8.0));
    // The row's address takes the slot chosen; the slot playing and the
    // slot queued come back under it.
    std::vector<std::string> sent;
    for (const auto& n : g.nodes) {
        if (n.type == "oscOut") sent.push_back(std::get<std::string>(n.params.at("address")));
    }
    CHECK(sent == std::vector<std::string>{"/loops/bass/playing", "/loops/bass/queued", "/loops/pad/playing",
                                           "/loops/pad/queued"});
    const auto& rows = firstSection(g);
    CHECK(rows.at(0).address == "/loops/bass");
    CHECK(rows.at(1).params.at("bank") == paramValue(std::string("pads")));
    // Rows stack, one a line.
    CHECK(rows.at(0).params.at("at") == paramValue(std::vector<double>{1, 1}));
    CHECK(rows.at(1).params.at("at") == paramValue(std::vector<double>{1, 3}));
    CHECK_THAT(errorOf(R"((clips :slots 4))"), ContainsSubstring("clips takes a label"));
}

TEST_CASE("a clips row's bank, slots, and grid are checked as written", "[interpreter][page]") {
    const auto row = [](const std::string& call) {
        return errorOf(R"((page (panel "P" (section "S" )" + call + ")))");
    };
    CHECK_THAT(row(R"((clips "Bass" :grid 250ms))"), ContainsSubstring("clips :grid is in beats"));
    CHECK_THAT(row(R"((clips "Bass" :slots -1))"), ContainsSubstring("clips :slots is from 1 to 64, got -1"));
    CHECK_THAT(row(R"((clips "Bass" :slots 2.5))"), ContainsSubstring("clips :slots is a whole number"));
    CHECK_THAT(row(R"((clips "Bass" :initial 1.5))"), ContainsSubstring("clips :initial is a whole number"));
    CHECK_THAT(row(R"((clips ""))"), ContainsSubstring("a label with a letter or digit, or a :bank"));
    // A row's reports take addresses of their own: one written by hand
    // moves the row's.
    const auto g =
        graph(R"((page (panel "P" (section "S" (clips "Bass") (knob "K" :address "/s/bass/playing")))))");
    CHECK(firstSection(g).at(0).address == "/s/bass-2");
}

TEST_CASE("a pedal places its own controls, and a size is two whole numbers", "[interpreter][page]") {
    CHECK_THAT(errorOf(R"((page (pedalboard (pedal "Echo" (knob "K" :at [1 1])))))"),
               ContainsSubstring("a pedal places its controls itself"));
    CHECK_THAT(errorOf(R"((page (panel "P" (section "S" (knob "K")) :size [2.5 30])))"),
               ContainsSubstring("size must be two whole numbers from 1 to 256"));
    CHECK_THAT(errorOf(R"((page (panel "P" (section "S" (knob "K" :span [3 4000])))))"),
               ContainsSubstring("span must be two whole numbers from 1 to 256"));
}
