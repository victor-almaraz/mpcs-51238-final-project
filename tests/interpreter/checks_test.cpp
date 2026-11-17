// Checks at the call, in language terms: repeated keywords, keys, arity,
// dimensions of constant and mapper math, parameter ranges, pipe kinds,
// and primitive constraints (interpreter.md §5.4, §8, §9, §10.2).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <string>
#include <string_view>

#include <espresso/interpreter/compiler.h>

#include "support/compile.h"
#include "support/unwrap.h"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using namespace interpreter;
using support::error;
using support::piece;

namespace {

std::string warnings(const compileResult& r) {
    INFO(r.text());
    REQUIRE(r.ok());
    std::string out;
    for (const auto& w : r.warnings) out += w.text() + "\n";
    return out;
}

// A body with pipes a (audio), b (audio), c (control), e (event), and
// s (stereo audio), followed by @p rest.
std::string withPipes(const std::string& rest) {
    return "(let ((a (pipe :audio)) (b (pipe :audio)) (c (pipe :control)) (e (pipe :event))"
           " (s (pipe :audio :channels 2)))\n" +
           rest + ")";
}

// 8 instruments of 8 voices (3 nodes each) and 2 effects: the v2 load
// piece (AGENTS.md "Testing"), 208 nodes.
constexpr std::string_view largePiece = R"(
(define (voice f out)
  (let ((o (pipe :audio)) (v (pipe :audio)))
    (saw-osc :freq f :out o)
    (biquad :in o :freq (* 4 f) :out v)
    (gain :in v :amount -12db :out out)))
(define (instrument base out)
  (let ((bus (pipe :audio)) (wet (pipe :audio)))
    (map (fn (i) (voice (* base (+ i 1)) bus :key i)) (range 8))
    (comb :in bus :time 30ms :out wet)
    (delay-line :in wet :time 250ms :out out)))
(defpiece main ()
  (let ((out (pipe :audio)))
    (map (fn (k) (instrument (* 55hz (+ k 1)) out :key k)) (range 8))
    (audio-out :in out)))
)";

}  // namespace

TEST_CASE("a repeated keyword fails at the second one") {
    // Columns: "(sine-osc " is 1-10, ":freq 220hz " 11-22, the second :freq 23.
    CHECK_THAT(error(piece("(sine-osc :freq 220hz :freq 440hz :out (pipe :audio))")),
               StartsWith("test.lisp:3:23: :freq is given twice"));
    // Columns: "(pipe :audio " is 1-13, ":channels 2 " 14-25, the second :channels 26.
    CHECK_THAT(error(piece("(pipe :audio :channels 2 :channels 1)")),
               StartsWith("test.lisp:3:26: :channels is given twice"));
    CHECK_THAT(error(piece("(f 1 :key 'a :key 'b)", "(define (f x) x)")),
               ContainsSubstring(":key is given twice"));
    CHECK_THAT(error(piece("(f :x 1 :x 2)", "(define (f &key x) x)")),
               ContainsSubstring(":x is given twice"));
}

TEST_CASE(":key is checked where it is written") {
    SECTION("outside a piece build") {
        CHECK_THAT(error(compile("(define (f x) x)\n(define y (f 1 :key \"k\"))\n(defpiece main () 1)",
                                 "test.lisp")),
                   ContainsSubstring(":key names calls while a piece is built"));
    }
    SECTION("keys are symbols, strings, keywords, or integers") {
        const std::string v = "(define (v x) (sine-osc :freq x :out (pipe :audio)))";
        CHECK_THAT(error(piece("(v 100hz :key [1 2]) (v 200hz :key [3 4])", v)),
                   ContainsSubstring(":key must be a symbol, string, keyword, or integer, got a vector"));
        CHECK_THAT(error(piece("(v 100hz :key 1.5)", v)), ContainsSubstring(":key must be a symbol"));
        const auto r = piece("(v 100hz :key 1) (v 200hz :key 2) (v 300hz :key 'x) (v 400hz :key \"y\")", v);
        INFO(r.text());
        CHECK(r.ok());
    }
}

TEST_CASE("arity messages state the accepted counts") {
    CHECK_THAT(error(piece("(f 1 2 3)", "(define (f a &optional b) a)")),
               ContainsSubstring("f expects 1 to 2 arguments, got 3"));
    CHECK_THAT(error(piece("(f)", "(define (f a &optional b) a)")),
               ContainsSubstring("f expects 1 to 2 arguments, got 0"));
    CHECK_THAT(error(piece("(f 1 2)", "(define (f a) a)")), ContainsSubstring("f expects 1 argument, got 2"));
    CHECK_THAT(error(piece("(f)", "(define (f a b &rest r) a)")),
               ContainsSubstring("f expects at least 2 arguments, got 0"));
}

TEST_CASE("constant math checks dimensions") {
    CHECK_THAT(error(piece("(neg -6db)")), ContainsSubstring("neg of a level is ambiguous"));
    CHECK_THAT(error(piece("(abs -6db)")), ContainsSubstring("abs of a level is ambiguous"));
    CHECK_THAT(error(piece("(min 100ms 1hz)")),
               ContainsSubstring("min expects one dimension: got a frequency after a time"));
    CHECK_THAT(error(piece("(max 1 1hz)")), ContainsSubstring("max expects one dimension"));
    CHECK_THAT(error(piece("(clamp 1s 2hz 3st)")), ContainsSubstring("clamp expects one dimension"));
    CHECK_THAT(error(piece("(quantize 1.3s 3)")), ContainsSubstring("quantize expects one dimension"));
    CHECK_THAT(error(piece("(scale 0.5 0 1s 100hz 1khz)")), ContainsSubstring("scale expects one dimension"));
    CHECK_THAT(error(piece("(scale-exp 0.5 0 1 100hz 1s)")),
               ContainsSubstring("scale-exp expects one dimension"));
    CHECK_THAT(error(piece("(sin 1s)")), ContainsSubstring("sin expects a plain number, got a time"));
    CHECK_THAT(error(piece("(pow 2 2s)")), ContainsSubstring("pow expects a plain number, got a time"));
    // The same rules accept matching dimensions.
    const auto r = piece(R"(
  (let ((a (pipe :audio)))
    (sine-osc :freq (scale 0.5 0 1 100hz (max 1khz (min 2khz 3khz))) :phase (floor 0.5) :out a)
    (delay-line :in a :time (clamp 2s 10ms 1s) :max-time (quantize 1.3s 500ms) :out (pipe :audio))))");
    INFO(r.text());
    CHECK(r.ok());
}

TEST_CASE("mapper constants check dimensions and zero divisors") {
    auto mapped = [](const std::string& expression) {
        return piece(
            withPipes("(lfo :rate 1hz :out c) (sine-osc :out a) (delay-line :in a :time (mapper c (fn (x) " +
                      expression + ")) :out b)"));
    };
    CHECK_THAT(error(mapped("(+ x 1beat)")),
               ContainsSubstring("beat values are not supported in mappers yet"));
    CHECK_THAT(error(mapped("(* x 1beat)")),
               ContainsSubstring("beat values are not supported in mappers yet"));
    CHECK_THAT(error(mapped("(+ x -6db)")), ContainsSubstring("adding a level is ambiguous"));
    CHECK_THAT(error(mapped("(- x -6db)")), ContainsSubstring("subtracting a level is ambiguous"));
    CHECK_THAT(error(mapped("(+ x 1s 1hz)")), ContainsSubstring("cannot add a frequency to a time"));
    CHECK_THAT(error(mapped("(* x 2s 3hz)")), ContainsSubstring("cannot multiply two quantities"));
    CHECK_THAT(error(mapped("(pow x 2s)")), ContainsSubstring("pow expects a plain number, got a time"));
    CHECK_THAT(error(mapped("(clamp x 1s 2hz)")), ContainsSubstring("clamp expects one dimension"));
    CHECK_THAT(error(mapped("(scale x 0 1 1s 2hz)")), ContainsSubstring("scale expects one dimension"));
    CHECK_THAT(error(mapped("(/ x 0)")), ContainsSubstring("division by zero"));
    CHECK_THAT(error(mapped("(/ x 2 0)")), ContainsSubstring("division by zero"));
    CHECK_THAT(error(mapped("(/ 1 0 x)")), ContainsSubstring("division by zero"));
    // Quantities that mean something in a mapper are accepted.
    for (const auto* ok :
         {"(+ x 10ms)", "(* x -6db)", "(/ x 2)", "(scale x 0 1 10ms 1s)", "(clamp x 0s 1s)", "(min x 1s)"}) {
        const auto r = mapped(ok);
        INFO(ok << ": " << r.text());
        CHECK(r.ok());
    }
}

TEST_CASE("parameter values are checked at the value, in the user's units") {
    // Columns: "(audio-out :in o :channel " is 1-26; the value starts at 27.
    CHECK_THAT(error(piece("(let ((o (pipe :audio)))\n(audio-out :in o :channel 300))")),
               StartsWith("test.lisp:4:27: parameter :channel is 300; expected 0 to 255"));
    CHECK_THAT(error(piece("(let ((o (pipe :audio)))\n(audio-out :in o :ceiling 6db))")),
               ContainsSubstring("parameter :ceiling is 6db; expected at most 0db"));
    CHECK_THAT(error(piece("(sine-osc :freq -5 :out (pipe :audio))")),
               StartsWith("test.lisp:3:17: parameter :freq is -5hz; expected 0hz to 100000hz"));
    CHECK_THAT(error(piece("(delay-line :in (pipe :audio) :max-time 5000s :out (pipe :audio))")),
               ContainsSubstring("parameter :max-time is 5000s; expected 0s to 3600s"));
}

TEST_CASE("pipe kinds are checked at the value") {
    SECTION("ports") {
        // Line 4 is the rest; "(biquad :in " is 1-12, so c is at 13.
        CHECK_THAT(error(piece(withPipes("(biquad :in c :out b)"))),
                   StartsWith("test.lisp:4:13: :in expects an audio pipe, got a control pipe"));
        CHECK_THAT(error(piece(withPipes("(sine-osc :out e)"))),
                   ContainsSubstring(":out expects an audio pipe, got an event pipe"));
        CHECK_THAT(error(piece(withPipes("(audio-out :in [a b])"))),
                   ContainsSubstring(":in takes one pipe, got 2"));
        CHECK_THAT(error(piece(withPipes("(add :in [a e] :out b)"))),
                   ContainsSubstring(":in expects an audio or control pipe, got an event pipe"));
    }
    SECTION("parameters") {
        CHECK_THAT(error(piece(withPipes("(biquad :in a :q b :out s)"))),
                   ContainsSubstring("parameter :q expects a number or a control pipe, got an audio pipe"));
        CHECK_THAT(error(piece(withPipes("(sine-osc :freq e :out a)"))),
                   ContainsSubstring(
                       "parameter :freq expects a frequency or an audio or control pipe, got an event pipe"));
    }
    SECTION("required ports") {
        CHECK_THAT(error(piece("(sine-osc :freq 5)")), StartsWith("test.lisp:3:1: sine-osc needs :out"));
    }
}

TEST_CASE("primitive constraints are checked at the call") {
    CHECK_THAT(error(piece(withPipes("(mix :in [a b] :levels [0.5] :out s)"))),
               ContainsSubstring("mix: :levels has 1 entry for 2 pipes on :in"));
    CHECK_THAT(error(piece(withPipes("(metro :out e) (sequencer :in e :values [] :out c)"))),
               ContainsSubstring("sequencer: :values must not be empty"));
    CHECK_THAT(error(piece(withPipes("(metro :out e) (sequencer :in e :out c)"))),
               ContainsSubstring("sequencer: :values must not be empty"));
    CHECK_THAT(error(piece(withPipes("(svf :in a)"))),
               ContainsSubstring("svf: needs at least one of :out, :low, :band, :high, :notch"));
    CHECK_THAT(error(piece(withPipes("(osc-in :port 9000)"))),
               ContainsSubstring("osc-in: needs at least one of :out, :value"));
    CHECK_THAT(error(piece(withPipes("(osc-out :port 9000)"))),
               ContainsSubstring("osc-out: needs at least one of :in, :value"));
    CHECK_THAT(error(piece(withPipes("(lfo :out c) (osc-out :value c :address \"\")"))),
               ContainsSubstring("osc-out: with :value, :address must not be empty"));
    CHECK_THAT(error(piece(withPipes("(sine-osc :out a) (pan :in a :out b)"))),
               ContainsSubstring("pan: :out needs a pipe with at least 2 channels"));
    CHECK_THAT(error(piece(withPipes("(sine-osc :out a) (adsr :in a :out c)"))),
               ContainsSubstring("adsr: with :in, :out must be an audio pipe"));
    // The same primitives with valid arguments compile.
    const auto r = piece(withPipes(R"(
    (sine-osc :out a) (sine-osc :freq 3hz :out b)
    (mix :in [a b] :levels [0.5 0.5] :out s)
    (metro :out e) (sequencer :in e :values [1 2] :out c)
    (svf :in a :low (pipe :audio))
    (osc-in :port 9000 :value (pipe :control))
    (osc-out :port 9001 :value c)
    (pan :in a :out (pipe :audio :channels 2))
    (adsr :in a :gate e :out (pipe :audio))
    (audio-out :in s))"));
    INFO(r.text());
    CHECK(r.ok());
}

TEST_CASE("loop gain follows every path back to the feedback pipe") {
    SECTION("through several feedback pipes") {
        const auto text = warnings(piece(R"(
  (let ((f1 (pipe :audio :feedback #t))
        (f2 (pipe :audio :feedback #t))
        (f3 (pipe :audio :feedback #t)))
    (gain :in f1 :amount 2 :out f2)
    (gain :in f3 :amount 2 :out f1)
    (gain :in f2 :amount 2 :out f3)
    (audio-out :in f1)))"));
        CHECK_THAT(text,
                   ContainsSubstring("the loop through feedback audio pipes 0, 1, and 2 has a gain of 8"));
        CHECK(std::count(text.begin(), text.end(), '\n') == 1);  // one loop, one warning
    }
    SECTION("through a parameter bound to a pipe") {
        CHECK_THAT(warnings(piece(R"(
  (let ((fb (pipe :audio :feedback #t)) (src (pipe :audio)) (o (pipe :audio)))
    (sine-osc :freq 220hz :out src)
    (subtract :in src :b fb :out o)
    (gain :in o :amount 3 :out fb)
    (audio-out :in o)))")),
                   ContainsSubstring("the loop through feedback audio pipe 0 has a gain of 3"));
        CHECK_THAT(warnings(piece(R"(
  (let ((fb (pipe :audio :feedback #t)) (src (pipe :audio)) (o (pipe :audio)))
    (sine-osc :freq 220hz :out src)
    (gain :in (mapper src (fn (x) (- x (* fb 3)))) :amount 1 :out o)
    (gain :in o :amount 1 :out fb)
    (audio-out :in o)))")),
                   ContainsSubstring("the loop through feedback audio pipe 0 has a gain of 3"));
    }
    SECTION("a pipe given twice to mix counts each level once") {
        CHECK(warnings(piece(R"(
  (let ((fb (pipe :audio :feedback #t)) (src (pipe :audio)) (o (pipe :audio)))
    (sine-osc :freq 220hz :out src)
    (mix :in [src fb fb] :levels [1 0.3 0.3] :out o)
    (gain :in o :amount 1 :out fb)
    (audio-out :in o)))"))
                  .empty());
        CHECK_THAT(warnings(piece(R"(
  (let ((fb (pipe :audio :feedback #t)) (src (pipe :audio)) (o (pipe :audio)))
    (sine-osc :freq 220hz :out src)
    (mix :in [src fb fb] :levels [1 0.3 0.8] :out o)
    (gain :in o :amount 1 :out fb)
    (audio-out :in o)))")),
                   ContainsSubstring("has a gain of 1.1"));
    }
}

TEST_CASE("deep nesting and recursion fail cleanly") {
    // Syntactically shallow, but each call nests evaluation on the C++
    // stack; the depth limit stops it with an error, not a crash.
    CHECK_THAT(
        error(compile(
            "(define (f n) (if (= n 0) 0 (+ 1 (f (- n 1)))))\n(define x (f 100000))\n(defpiece main () 1)",
            "test.lisp")),
        ContainsSubstring("nested too deeply (more than 2000 levels)"));
    // A chain of mappers lowers recursively.
    CHECK_THAT(error(piece(R"(
  (let ((c (pipe :control)) (a (pipe :audio)) (m c))
    (lfo :out c)
    (map (fn (i) (set! m (mapper m (fn (x) x)))) (range 100000))
    (sine-osc :freq m :out a)))")),
               ContainsSubstring("nested too deeply"));
    // Below the limit, recursion works.
    const auto r =
        compile("(define (f n) (if (= n 0) 0 (+ 1 (f (- n 1)))))\n(define x (f 300))\n(defpiece main () 1)",
                "test.lisp");
    INFO(r.text());
    CHECK(r.ok());
}

TEST_CASE("an evaluation budget ends programs that do not finish") {
    compileOptions options;
    options.maxSteps = 100000;
    const std::string spin = "(define big (range 100000))\n"
                             "(define x (map (fn (i) (map (fn (j) j) big)) big))\n"
                             "(defpiece main () 1)";
    CHECK_THAT(error(compile(spin, "test.lisp", options)),
               ContainsSubstring("the program did not finish within 100000 evaluation steps"));
    CHECK(compileOptions{}.maxSteps == compileOptions::defaultMaxSteps);
    // The default leaves a margin of 10 over a large piece.
    options.maxSteps = compileOptions::defaultMaxSteps / 10;
    const auto large = compile(largePiece, "test.lisp", options);
    INFO(large.text());
    REQUIRE(large.ok());
    CHECK(support::unwrap(large.graph).nodes.size() == 8 * (8 * 3 + 2) + 1);
}

TEST_CASE("integer arithmetic reports overflow") {
    CHECK_THAT(error(piece("(+ 9223372036854775807 1)")), ContainsSubstring("integer overflow"));
    CHECK_THAT(error(piece("(- (- 9223372036854775807) 2)")), ContainsSubstring("integer overflow"));
    CHECK_THAT(error(piece("(* -3037000500 3037000500)")), ContainsSubstring("integer overflow"));
    CHECK_THAT(error(piece("(- 0 (- (- 9223372036854775807) 1))")), ContainsSubstring("integer overflow"));
    const auto r = piece("(if (= (* -1 9223372036854775807) (- 9223372036854775807)) nil (undefined))");
    INFO(r.text());
    CHECK(r.ok());
}

TEST_CASE("node handles compare by identity") {
    const auto r = piece(R"(
  (let ((n1 (sine-osc :out (pipe :audio))) (n2 (sine-osc :out (pipe :audio))))
    (if (= n1 n2) (undefined) nil)
    (if (= n1 n1) nil (undefined))))");
    INFO(r.text());
    CHECK(r.ok());
}
