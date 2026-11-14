// The compiler: evaluation, units, graph building, mappers, node IDs,
// ordering, and errors (interpreter.md §5–§12).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <proj/interpreter/compiler.h>
#include <proj/ir/ir.h>

#include <algorithm>

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace interpreter;
using metadata::pipeKind;
using metadata::pipeRef;

namespace {

// Compiles a piece whose body is @p body.
compileResult piece(const std::string& body, const std::string& before = "") {
    return compile(before + "\n(defpiece main ()\n" + body + ")\n", "test.lisp");
}

metadata::graphSpec ok(const compileResult& r) {
    INFO(r.text());
    REQUIRE(r.ok());
    return *r.graph;
}

const metadata::nodeSpec& find(const metadata::graphSpec& g, const std::string& type, std::size_t which = 0) {
    for (const auto& n : g.nodes) {
        if (n.type == type && which-- == 0) return n;
    }
    FAIL("no node of type " + type);
    return g.nodes.front();
}

std::string error(const compileResult& r) {
    REQUIRE_FALSE(r.ok());
    return r.text();
}

} // namespace

TEST_CASE("a piece compiles to a validated graph") {
    const auto g = ok(piece(R"(
  (let ((osc (pipe :audio))
        (out (pipe :audio :channels 2)))
    (sine-osc :freq 440hz :out osc)
    (pan :in osc :position -0.5 :out out)
    (audio-out :in out :channel 0)))"));
    REQUIRE(g.nodes.size() == 3);
    CHECK(g.nodes[0].type == "sineOsc");
    CHECK(std::get<double>(g.nodes[0].params.at("freq")) == 440.0);
    CHECK(g.nodes[0].outputs.at("out") == std::vector<pipeRef>{{pipeKind::audio, 0}});
    CHECK(g.pipes[1].channels == 2);
    CHECK(g.nodes[0].id == "test.lisp/main/sine-osc@6:5#0");
    CHECK(g.nodes[0].src->line == 6);
    CHECK(ir::validate(g).empty());
}

TEST_CASE("unit literals become canonical values") {
    const auto g = ok(piece(R"(
  (let ((a (pipe :audio)) (b (pipe :audio)))
    (saw-osc :freq 1.5khz :out a)
    (adsr :in a :attack 250ms :decay 1s :release 500us :sustain -6db :out b)
    (delay-line :in b :time 480smp :max-time 2s :out (pipe :audio))))"));
    CHECK(std::get<double>(find(g, "sawOsc").params.at("freq")) == 1500.0);
    const auto& env = find(g, "adsr");
    CHECK(std::get<double>(env.params.at("attack")) == Approx(0.25));
    CHECK(std::get<double>(env.params.at("release")) == Approx(0.0005));
    CHECK(std::get<double>(env.params.at("sustain")) == Approx(0.501187).epsilon(1e-5));
    CHECK(std::get<double>(find(g, "delayLine").params.at("time")) == Approx(0.01));  // 480 samples at 48 kHz
}

TEST_CASE("arithmetic follows dimensions") {
    const auto g = ok(piece(R"(
  (let ((a (pipe :audio)) (b (pipe :audio)))
    (sine-osc :freq (* 2 220hz) :phase (/ 1 4) :out a)
    (delay-line :in a :time (+ 1s 250ms) :max-time (/ 1 0.5hz) :out b)
    (gain :in b :amount (+ -6db -6db) :out (pipe :audio))))"));
    CHECK(std::get<double>(find(g, "sineOsc").params.at("freq")) == 440.0);
    CHECK(std::get<double>(find(g, "sineOsc").params.at("phase")) == 0.25);
    CHECK(std::get<double>(find(g, "delayLine").params.at("time")) == 1.25);
    CHECK(std::get<double>(find(g, "delayLine").params.at("max-time")) == 2.0);
    CHECK(std::get<double>(find(g, "gain").params.at("amount")) == Approx(0.251189).epsilon(1e-5));  // -12 dB
    CHECK_THAT(error(piece("(+ 1s 440hz)")), ContainsSubstring("cannot add a frequency to a time"));
    CHECK_THAT(error(piece("(+ 1 440hz)")), ContainsSubstring("cannot add a frequency to a plain number"));
    CHECK_THAT(error(piece("(* 1s 2s)")), ContainsSubstring("cannot multiply two quantities"));
}

TEST_CASE("parameters are checked against their dimension and type") {
    CHECK_THAT(error(piece("(sine-osc :freq 2s :out (pipe :audio))")),
               ContainsSubstring("test.lisp:3:11: parameter :freq expects a frequency, got a time"));
    CHECK_THAT(error(piece("(biquad :in (pipe :audio) :mode :sideways :out (pipe :audio))")),
               ContainsSubstring("expects one of :low-pass"));
    CHECK_THAT(error(piece("(sine-osc :frequency 2 :out (pipe :audio))")),
               ContainsSubstring("sine-osc has no parameter or port :frequency"));
    CHECK_THAT(error(piece("(sine-osc 440 :out (pipe :audio))")), ContainsSubstring("takes keyword arguments"));
    CHECK_THAT(error(piece("(audio-out :in (pipe :audio) :channel (pipe :control))")),
               ContainsSubstring("takes a constant, not a pipe"));
    CHECK_THAT(error(piece("(delay-line :in (pipe :audio) :time 1beat :out (pipe :audio))")),
               ContainsSubstring("beat values for time parameters are not supported yet"));
    // Ranges and required ports come from IR validation, mapped to the call.
    CHECK_THAT(error(piece("(sine-osc :freq -5 :out (pipe :audio))")),
               ContainsSubstring("test.lisp:3:1: parameter freq is -5"));
    CHECK_THAT(error(piece("(sine-osc :freq 5)")), ContainsSubstring("missing output out"));
}

TEST_CASE("closures, let, if, cond, set!, and vectors evaluate") {
    const auto g = ok(piece(R"(
  (let ((out (pipe :audio))
        (n 3))
    (set! n (+ n 1))
    (osc (if (> n 3) 880hz 110hz) out)
    (osc (cond ((= n 0) 1hz) (else 2hz)) out)
    (audio-out :in out :channel 0)))",
                            R"(
(define (osc f out) (sine-osc :freq f :out out))
(define unused (list 1 2 3)))"));
    CHECK(std::get<double>(find(g, "sineOsc", 0).params.at("freq")) == 880.0);
    CHECK(std::get<double>(find(g, "sineOsc", 1).params.at("freq")) == 2.0);
    CHECK(find(g, "sineOsc", 0).id == "test.lisp/main/osc@9:5#0/sine-osc@2:21#0");
}

TEST_CASE("node IDs: loops get occurrence indices; keys override them") {
    const std::string voice = "(define (voice f out) (sine-osc :freq f :out out))";
    const auto looped = ok(piece(R"(
  (let ((out (pipe :audio)))
    (map (fn (f) (voice f out)) [100hz 200hz 300hz])))",
                                 voice));
    REQUIRE(looped.nodes.size() == 3);
    CHECK(looped.nodes[0].id == "test.lisp/main/voice@5:18#0/sine-osc@1:23#0");
    CHECK(looped.nodes[2].id == "test.lisp/main/voice@5:18#2/sine-osc@1:23#0");

    // With keys, reordering the items keeps each node's ID.
    auto keyed = [&](const std::string& items) {
        return ok(piece("(let ((out (pipe :audio))) (map (fn (k) (voice 100hz out :key k)) " + items + "))", voice));
    };
    const auto a = keyed("['low 'high]");
    const auto b = keyed("['high 'low]");
    auto ids = [](const metadata::graphSpec& g) {
        std::vector<std::string> out;
        for (const auto& n : g.nodes) out.push_back(n.id);
        std::sort(out.begin(), out.end());
        return out;
    };
    CHECK(ids(a) == ids(b));
    CHECK_THAT(ids(a)[0], ContainsSubstring("voice@"));
    CHECK_THAT(ids(a)[0], ContainsSubstring("#high/"));

    CHECK_THAT(error(piece("(let ((out (pipe :audio))) (voice 1 out :key 'x) (voice 2 out :key 'x))", voice)),
               ContainsSubstring("use :key x"));
}

TEST_CASE("compiling twice gives identical IR") {
    const std::string src = R"(
  (let ((a (pipe :audio)) (c (pipe :control)))
    (lfo :rate 2hz :out c)
    (saw-osc :freq (mapper c (fn (x) (scale-exp x -1 1 100hz 1khz))) :out a)
    (audio-out :in a :channel 0)))";
    CHECK(ir::write(ok(piece(src))) == ir::write(ok(piece(src))));
}

TEST_CASE("mappers lower to generator nodes") {
    const auto g = ok(piece(R"(
  (let ((knob (pipe :control)) (a (pipe :audio)))
    (saw-osc :freq (mapper knob (fn (x) (+ 100hz (* x 2)))) :out a)
    (audio-out :in a :channel 0)))"));
    // (* x 2) is a gain; adding a constant is subtracting its negation.
    const auto& gain = find(g, "gain");
    CHECK(std::get<double>(gain.params.at("amount")) == 2.0);
    CHECK(gain.inputs.at("in") == std::vector<pipeRef>{{pipeKind::control, 1}});
    const auto& sub = find(g, "subtract");
    CHECK(std::get<double>(sub.params.at("b")) == -100.0);
    CHECK(sub.inputs.at("in") == gain.outputs.at("out"));
    CHECK(std::get<pipeRef>(find(g, "sawOsc").params.at("freq")) == sub.outputs.at("out").front());
    CHECK_THAT(gain.id, ContainsSubstring("test.lisp/main/mapper@5:20#0/*@5:50#0"));

    // Pipes cannot be transformed outside a mapper.
    CHECK_THAT(error(piece("(let ((c (pipe :control))) (* c 2))")), ContainsSubstring("only inside a mapper"));
}

TEST_CASE("mappers on audio pipes produce audio pipes; two pipes combine") {
    const auto g = ok(piece(R"(
  (let ((a (pipe :audio :channels 2)) (b (pipe :audio)) (out (pipe :audio :channels 2)))
    (saw-osc :out a)
    (sine-osc :out b)
    (gain :in (mapper a (fn (x) (* (tanh x) (- 1 x)))) :amount 0.5 :out out)
    (audio-out :in out :channel 0)))"));
    const auto& tanhNode = find(g, "tanh");
    CHECK(tanhNode.outputs.at("out").front().kind == pipeKind::audio);
    const auto out = tanhNode.outputs.at("out").front();
    const auto decl = std::find_if(g.pipes.begin(), g.pipes.end(), [&](const auto& d) { return d.pipe == out; });
    CHECK(decl->channels == 2);
    CHECK(find(g, "neg").inputs.at("in") == std::vector<pipeRef>{{pipeKind::audio, 0}});
    CHECK(find(g, "multiply").inputs.at("in").size() == 2);
}

TEST_CASE("nodes are sorted by data flow; cycles need a feedback pipe") {
    const auto g = ok(piece(R"(
  (let ((a (pipe :audio)) (b (pipe :audio)))
    (audio-out :in b :channel 0)
    (gain :in a :amount 0.5 :out b)
    (sine-osc :out a)))"));
    CHECK(g.nodes[0].type == "sineOsc");
    CHECK(g.nodes[1].type == "gain");
    CHECK(g.nodes[2].type == "audioOut");

    const std::string loop = R"(
  (let ((a (pipe :audio FEEDBACK)) (b (pipe :audio)))
    (audio-in :out b)
    (add :in [a b] :out a)))";
    // A node reading its own output is a cycle; a feedback pipe closes it.
    CHECK_THAT(error(piece(std::string(loop).replace(loop.find("FEEDBACK"), 8, ""))),
               ContainsSubstring("these nodes form a cycle: add (6:5)"));
    ok(piece(std::string(loop).replace(loop.find("FEEDBACK"), 8, ":feedback #t")));
}

TEST_CASE("a cycle error names only the nodes on the cycle") {
    const auto text = error(piece(R"(
  (let ((a (pipe :audio)) (b (pipe :audio)) (c (pipe :audio)))
    (saw-osc :out a)
    (gain :in a :out b)
    (gain :in b :out a)
    (audio-out :in b :channel 0)))"));
    CHECK_THAT(text, ContainsSubstring("cycle: gain (6:5), gain (7:5)."));
    CHECK_THAT(text, !ContainsSubstring("audio-out"));
    CHECK_THAT(text, !ContainsSubstring("saw-osc"));
}

TEST_CASE("a two-node cycle without feedback is an error") {
    CHECK_THAT(error(piece(R"(
  (let ((a (pipe :audio)) (b (pipe :audio)))
    (gain :in a :out b)
    (gain :in b :out a)))")),
               ContainsSubstring("gain (5:5), gain (6:5)"));
}

TEST_CASE("tempo, seed, and the entry piece") {
    compileOptions o;
    o.seed = 9;
    o.entry = "other";
    const auto r = compile("(defpiece other () (tempo 96bpm) (noise :out (pipe :audio)))", "x.lisp", o);
    const auto g = ok(r);
    CHECK(g.tempo == Approx(96.0));
    CHECK(g.seed == 9);
    CHECK_THAT(error(compile("(define main 1)", "x.lisp")), ContainsSubstring("no piece named main"));
}

TEST_CASE("errors carry the span and the call stack") {
    const auto r = piece("(helper 1)", "(define (helper x)\n  (sine-osc :freq 1s :out (pipe :audio)))");
    REQUIRE_FALSE(r.ok());
    CHECK(r.errors[0].span.line == 2);
    REQUIRE(r.errors[0].stack.size() == 2);  // the piece, then helper
    CHECK(r.errors[0].stack[1].line == 4);
    CHECK_THAT(error(piece("(undefined-thing)")), ContainsSubstring("unbound symbol undefined-thing"));
    CHECK_THAT(error(piece("(sine-osc :freq)")), ContainsSubstring(":freq needs a value"));
    CHECK_THAT(error(compile("(import \"voices\")", "x.lisp")), ContainsSubstring("import is not supported yet"));
    CHECK_THAT(error(compile("(sine-osc :out 1", "x.lisp")), ContainsSubstring("x.lisp:1:1"));
    CHECK_THAT(error(compile("(pipe :audio)", "x.lisp")), ContainsSubstring("while a piece is built"));
    CHECK_THAT(error(piece("(pipe :audio :channels 9)")), ContainsSubstring("1 to 8"));
}

TEST_CASE("language names are kebab-case") {
    CHECK(languageName("sineOsc") == "sine-osc");
    CHECK(languageName("beatsToSeconds") == "beats-to-seconds");
    CHECK(languageName("add") == "add");
}

TEST_CASE("constant math matches the mapper operations") {
    const auto g = ok(piece(R"(
  (let ((a (pipe :audio)))
    (sine-osc :freq (scale-exp 0.5 0 1 100hz 400hz) :phase (clamp 3 0 1) :out a)
    (audio-out :in a :channel (max 0 (min 1 (round 0.4))))))"));
    CHECK(std::get<double>(find(g, "sineOsc").params.at("freq")) == Approx(200.0));
    CHECK(std::get<double>(find(g, "sineOsc").params.at("phase")) == 1.0);
    CHECK(std::get<double>(find(g, "audioOut").params.at("channel")) == 0.0);
}

TEST_CASE("review fixes: keys, mappers, equality, limits") {
    SECTION(":key works on a function bound with define and fn") {
        const auto src = [](const std::string& items) {
            return piece("(let ((a (pipe :audio))) (map (fn (k) (v 220 a :key k)) " + items + "))",
                         "(define v (fn (f o) (sine-osc :freq f :out o)))");
        };
        auto ids = [](const metadata::graphSpec& g) {
            std::vector<std::string> out;
            for (const auto& n : g.nodes) out.push_back(n.id);
            std::sort(out.begin(), out.end());
            return out;
        };
        const auto one = ids(ok(src("['bass 'lead]")));
        CHECK(one == ids(ok(src("['lead 'bass]"))));
        CHECK_THAT(one[0], ContainsSubstring("/v@"));
        CHECK_THAT(one[0], ContainsSubstring("#bass/"));
    }
    SECTION(":key on a builtin is an error") {
        CHECK_THAT(error(piece("(+ 1 2 :key 'x)")), ContainsSubstring(":key names a node"));
    }
    SECTION("large gains in mappers are valid") {
        ok(piece("(let ((k (pipe :control)) (a (pipe :audio))) (saw-osc :freq (mapper k (fn (x) (* x 2khz))) :out a))"));
    }
    SECTION("mapper IDs follow creation order, not use order") {
        // mk is anonymous, so both mappers share a parent and a call site.
        const auto g = ok(piece(R"(
  (let ((mk (fn (g k) (mapper k (fn (x) (* x g)))))
        (k (pipe :control)) (m1 (mk 2 k)) (m2 (mk 3 k)) (a (pipe :audio)))
    (saw-osc :freq m2 :out a)
    (sine-osc :freq m1 :out a)))"));
        const auto& first = find(g, "gain", 0);  // m2's gain, lowered first
        CHECK(std::get<double>(first.params.at("amount")) == 3.0);
        CHECK_THAT(first.id, ContainsSubstring("#1/"));  // m2 was made second
        CHECK_THAT(find(g, "gain", 1).id, ContainsSubstring("#0/"));
    }
    SECTION("equality compares pipes and is symmetric") {
        ok(piece("(let ((a (pipe :audio)) (b (pipe :audio))) (if (= a b) (undefined) (if (= a a) nil (undefined))))"));
        ok(piece("(if (= 1 'a) (undefined) nil)"));
        ok(piece("(if '() (undefined) nil)"));
    }
    SECTION("overflow, negated levels, and non-finite values are errors") {
        CHECK_THAT(error(piece("(* 3037000500 3037000500)")), ContainsSubstring("integer overflow"));
        CHECK_THAT(error(piece("(- 6db)")), ContainsSubstring("write the level in decibels"));
        CHECK_THAT(error(piece("(sequencer :in (pipe :event) :values [(pow -1 0.5)] :out (pipe :control))")),
                   ContainsSubstring("finite numbers"));
    }
    SECTION("deep nesting is a read error") {
        CHECK_THAT(error(compile(std::string(2000, '(') + std::string(2000, ')'), "x.lisp")),
                   ContainsSubstring("nested too deeply"));
    }
}

TEST_CASE("warnings flag likely audio hazards") {
    auto warnings = [](const compileResult& r) {
        INFO(r.text());
        REQUIRE(r.ok());
        std::string out;
        for (const auto& w : r.warnings) out += w.text() + "\n";
        return out;
    };
    const std::string loop = R"(
  (let ((fb (pipe :audio :feedback #t)) (s (pipe :audio)) (x (pipe :audio)))
    (noise :out s)
    (add :in [s fb] :out x)
    (gain :in x :amount AMOUNT :out fb)
    (audio-out :in x :channel 0)))";
    auto withAmount = [&](const std::string& a) { return std::string(loop).replace(loop.find("AMOUNT"), 6, a); };
    CHECK_THAT(warnings(piece(withAmount("1.5"))),
               ContainsSubstring("test.lisp:7:5: the loop through feedback audio pipe 0 has a gain of 1.5"));
    CHECK(warnings(piece(withAmount("0.5"))).empty());

    // Mix levels count along the loop.
    CHECK_THAT(warnings(piece(R"(
  (let ((fb (pipe :audio :feedback #t)) (s (pipe :audio)) (x (pipe :audio)))
    (noise :out s)
    (mix :in [s fb] :levels [1 0.9] :out x)
    (gain :in x :amount 1.2 :out fb)
    (audio-out :in x :channel 0)))")),
               ContainsSubstring("has a gain of 1.08"));

    CHECK_THAT(warnings(piece("(let ((a (pipe :audio))) (sine-osc :freq 30khz :out a) (audio-out :in a))")),
               ContainsSubstring("parameter :freq is 30000 Hz, above half the 48000 Hz sample rate"));
    CHECK_THAT(warnings(piece("(let ((a (pipe :audio)) (b (pipe :audio))) (sine-osc :out a) (gain :in a :amount 1000 :out b))")),
               ContainsSubstring("a gain of 1000 is 60 dB"));
    // Large gains on control pipes are ranges, not levels.
    CHECK(warnings(piece("(let ((k (pipe :control)) (a (pipe :audio))) (saw-osc :freq (mapper k (fn (x) (* x 2khz))) :out a))")).empty());
}
