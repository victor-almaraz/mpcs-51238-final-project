// The object system: classes, slots, generic functions, C3, and method
// combination (interpreter.md §7, grammar.md §3).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <string_view>

#include <proj/metadata/graph.h>

#include "support/compile.h"

using Catch::Matchers::ContainsSubstring;
using support::error;
using support::find;
using support::ok;
using support::piece;
using support::valueOf;

namespace {

// Classes that record the order their methods run in: each method
// appends its digit to trace.
std::string_view tracing() {
    return R"(
(define trace 0)
(define (note n) (set! trace (+ (* trace 10) n)))
)";
}

}  // namespace

TEST_CASE("make fills slots from initargs and initforms (grammar.md 3)") {
    const std::string defs = R"(
(define made 0)
(defclass voice ()
  ((freq :initarg :freq :initform 220 :accessor freq)
   (level :initarg :level :initform (do (set! made (+ made 1)) 0.5))
   (spare)))
)";
    CHECK(valueOf("(freq (make voice :freq 110))", defs) == 110.0);
    CHECK(valueOf("(freq (make voice))", defs) == 220.0);
    CHECK(valueOf("(slot-value (make voice :level 0.25) 'level)", defs) == 0.25);
    // An initform runs for each make that leaves its slot unset.
    CHECK(valueOf("(do (make voice) (make voice :level 1) (make voice) made)", defs) == 2.0);
    CHECK(valueOf("(if (slot-value (make voice) 'spare) 1 0)", defs) == 0.0);
}

TEST_CASE("set! writes a slot through its accessor or slot-value") {
    const std::string defs = "(defclass voice () ((freq :initarg :freq :accessor freq)))";
    CHECK(valueOf("(let ((v (make voice :freq 1))) (set! (freq v) 2) (freq v))", defs) == 2.0);
    CHECK(valueOf("(let ((v (make voice :freq 1))) (set! (slot-value v 'freq) 3) (freq v))", defs) == 3.0);
}

TEST_CASE("a subclass inherits slots; the most specific initform wins and initargs add up") {
    const std::string defs = R"(
(defclass voice () ((freq :initarg :freq :initform 220 :accessor freq)))
(defclass bass (voice) ((freq :initarg :pitch :initform 55)))
)";
    CHECK(valueOf("(freq (make bass))", defs) == 55.0);
    CHECK(valueOf("(freq (make bass :pitch 40))", defs) == 40.0);
    CHECK(valueOf("(freq (make bass :freq 30))", defs) == 30.0);
}

TEST_CASE("the class precedence list is the C3 linearization (interpreter.md 7.4)") {
    // A diamond: C3 visits c before the shared a; a depth-first walk
    // would visit a right after b.
    const std::string defs = std::string(tracing()) + R"(
(defclass a ()) (defclass b (a)) (defclass c (a)) (defclass d (b c))
(defgeneric who (x))
(defmethod who ((x a)) (note 1))
(defmethod who ((x b)) (note 2) (call-next-method))
(defmethod who ((x c)) (note 3) (call-next-method))
(defmethod who ((x d)) (note 4) (call-next-method))
)";
    CHECK(valueOf("(do (who (make d)) trace)", defs) == 4231.0);
    CHECK(valueOf("(do (who (make c)) trace)", defs) == 31.0);
}

TEST_CASE("a hierarchy with no consistent C3 order is an error") {
    CHECK_THAT(error(piece("1", R"(
(defclass x ()) (defclass y ())
(defclass p (x y)) (defclass q (y x))
(defclass z (p q))
)")),
               ContainsSubstring("superclasses of z have no consistent order"));
}

TEST_CASE("methods dispatch on every required argument, leftmost first (interpreter.md 7.2)") {
    const std::string defs = std::string(tracing()) + R"(
(defclass drum ()) (defclass stick ())
(defgeneric hit (a b))
(defmethod hit ((a drum) (b stick)) (note 1) (call-next-method))
(defmethod hit ((a drum) b) (note 2) (call-next-method))
(defmethod hit (a (b stick)) (note 3) (if (next-method-p) (note 4) (note 9)))
(defmethod hit (a b) (note 5))
)";
    CHECK(valueOf("(do (hit (make drum) (make stick)) trace)", defs) == 1234.0);
    CHECK(valueOf("(do (hit (make drum) 0) trace)", defs) == 25.0);
    CHECK(valueOf("(do (hit 0 0) trace)", defs) == 5.0);
}

TEST_CASE("standard method combination runs around, before, primary, and after methods in order (7.3)") {
    const std::string defs = std::string(tracing()) + R"(
(defclass base ()) (defclass leaf (base))
(defgeneric run (x))
(defmethod run :around ((x leaf)) (note 1) (call-next-method))
(defmethod run :around ((x base)) (note 2) (call-next-method))
(defmethod run :before ((x leaf)) (note 3))
(defmethod run :before ((x base)) (note 4))
(defmethod run ((x leaf)) (note 5) (call-next-method) 42)
(defmethod run ((x base)) (note 6))
(defmethod run :after ((x base)) (note 7))
(defmethod run :after ((x leaf)) (note 8))
)";
    CHECK(valueOf("(do (run (make leaf)) trace)", defs) == 12345678.0);
    CHECK(valueOf("(run (make leaf))", defs) == 42.0);  // the primary's value, through the arounds
}

TEST_CASE("call-next-method passes new arguments when given them") {
    const std::string defs = R"(
(defclass base ()) (defclass leaf (base))
(defmethod scaled ((x base) n) n)
(defmethod scaled ((x leaf) n) (call-next-method x (* n 2)))
)";
    CHECK(valueOf("(scaled (make leaf) 4)", defs) == 8.0);
}

TEST_CASE("a new method or class replaces cached dispatch (interpreter.md 7.5)") {
    const std::string defs = R"(
(defclass voice ())
(defmethod level ((v voice)) 1)
(define first (level (make voice)))
(defmethod level ((v voice)) 2)
(defclass voice () ((x :initform 3)))
(defmethod level ((v voice)) (slot-value v 'x))
)";
    CHECK(valueOf("(+ (* first 100) (level (make voice)))", defs) == 103.0);
}

TEST_CASE("an instance made before its class was redefined keeps its class's methods") {
    const std::string defs = R"(
(defclass a ()) (defclass b ()) (defclass c (a))
(define old (make c))
(defclass c (b))
(defmethod f ((o b)) 1)
(defmethod f ((o t)) 2)
)";
    // Dispatch on the new c first, so a cache keyed by class name would answer for old too.
    CHECK(valueOf("(+ (* 10 (f (make c))) (f old))", defs) == 12.0);
}

TEST_CASE("an error in a method shows each call once on the call stack") {
    const auto r = piece("(play-voice (make voice))", R"(
(defclass voice ())
(defmethod play-voice :around ((v voice)) (call-next-method))
(defmethod play-voice ((v voice)) (sine-osc :freq 1s :out (pipe :audio)))
)");
    REQUIRE_FALSE(r.ok());
    // The piece, the generic call, and the call-next-method.
    REQUIRE(r.errors[0].stack.size() == 3);
    CHECK(r.errors[0].stack[1].span.line == 7);
    CHECK(r.errors[0].stack[2].span.line == 3);
}

TEST_CASE("methods build subgraphs with one instance path step per generic call (interpreter.md 10.2)") {
    const auto g = ok(piece(R"(
  (let ((out (pipe :audio)))
    (play-voice (make voice :freq 110hz) out)
    (play-voice (make voice :freq 220hz) out :key 'high)
    (audio-out :in out :channel 0)))",
                            R"(
(defclass voice () ((freq :initarg :freq :accessor freq)))
(defgeneric play-voice (v out))
(defmethod play-voice ((v voice) out) (sine-osc :freq (freq v) :out out))
(defmethod play-voice :around ((v voice) out) (call-next-method))
)"));
    CHECK(find(g, "sineOsc", 0).id == "test.lisp/main/play-voice@10:5#0/sine-osc@4:39#0");
    CHECK(find(g, "sineOsc", 1).id == "test.lisp/main/play-voice@11:5#high/sine-osc@4:39#0");
    CHECK(std::get<double>(find(g, "sineOsc", 1).params.at("freq")) == 220.0);
}

TEST_CASE("object system errors name what is wrong") {
    const std::string voice = "(defclass voice () ((freq :initarg :freq)))";
    CHECK_THAT(error(piece("(make drum)")), ContainsSubstring("no class named drum"));
    CHECK_THAT(error(piece("(make voice :pitch 1)", voice)),
               ContainsSubstring("class voice has no initarg :pitch"));
    CHECK_THAT(error(piece("(slot-value (make voice) 'gain)", voice)),
               ContainsSubstring("class voice has no slot gain"));
    CHECK_THAT(error(piece("(slot-value 3 'freq)")), ContainsSubstring("expected an instance, got 3"));
    CHECK_THAT(error(piece("(f 1)", voice + "(defmethod f ((v voice)) 1)")),
               ContainsSubstring("no method of f applies to 1"));
    CHECK_THAT(error(piece("1", "(defgeneric f (a b)) (defmethod f (a) 1)")),
               ContainsSubstring("f takes 2 arguments to dispatch on, not 1"));
    CHECK_THAT(error(piece("1", "(defmethod gain ((x t)) 1)")),
               ContainsSubstring("gain is already the builtin gain"));
    CHECK_THAT(error(piece("1", "(defclass a (b))")), ContainsSubstring("no class named b"));
    CHECK_THAT(error(piece("1", "(defmethod f :later (x) 1)")),
               ContainsSubstring("unknown qualifier :later"));
    CHECK_THAT(error(piece("1", voice + "(defmethod f :before ((v voice)) (call-next-method)) (defmethod f "
                                        "((v voice)) 1) (define x (f (make voice)))")),
               ContainsSubstring("there is no next method"));
}
