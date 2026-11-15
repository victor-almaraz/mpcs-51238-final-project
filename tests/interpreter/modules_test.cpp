// Projects and modules: import, exports, cycles, and errors in imported
// files (interpreter.md §11, grammar.md §4.4).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <proj/interpreter/compiler.h>
#include <proj/metadata/graph.h>

#include "support/compile.h"
#include "support/unwrap.h"

using Catch::Matchers::ContainsSubstring;
using support::error;
using support::find;
using support::ok;

namespace {

using files = std::map<std::string, std::string, std::less<>>;

// Compiles main.lisp of the project @p project, path to contents.
interpreter::compileResult project(const files& project) {
    interpreter::compileOptions options;
    options.load = [project](const std::string& path) -> std::optional<std::string> {
        const auto it = project.find(path);
        return it == project.end() ? std::nullopt : std::optional(it->second);
    };
    return interpreter::compile(project.at("main.lisp"), "main.lisp", options);
}

// A piece that calls tone.
std::string_view tone() { return "(defpiece main () (tone (pipe :audio)))"; }

}  // namespace

TEST_CASE("import binds what a file exports (interpreter.md 11.2)") {
    const auto g = ok(project({
        {"voices.lisp", "(module voices (export tone))\n(define freq 330hz)\n(define (tone out) (sine-osc "
                        ":freq freq :out out))"},
        {"main.lisp", "(module main)\n(import \"voices\")\n" + std::string(tone())},
    }));
    const auto& osc = find(g, "sineOsc");
    CHECK(std::get<double>(osc.params.at("freq")) == 330.0);
    // The ID follows the entry piece; the source names the file the call is in.
    CHECK(osc.id == "main.lisp/main/tone@3:19#0/sine-osc@3:20#0");
    CHECK(support::unwrap(osc.src).file == "voices.lisp");
    CHECK_THAT(
        error(project({
            {"voices.lisp", "(module voices (export tone))\n(define freq 330hz)\n(define (tone out) 1)"},
            {"main.lisp", "(import \"voices\")\n(defpiece main () (sine-osc :freq freq :out (pipe :audio)))"},
        })),
        ContainsSubstring("unbound symbol freq"));
}

TEST_CASE("a file without an export clause exports what it defines, not what it imports") {
    const files base{
        {"util/scales.lisp", "(define root 110hz)"},
        {"voices.lisp", "(import \"util/scales\")\n(define (tone out) (sine-osc :freq root :out out))"},
    };
    auto p = base;
    p["main.lisp"] = "(import \"voices\")\n" + std::string(tone());
    CHECK(std::get<double>(find(ok(project(p)), "sineOsc").params.at("freq")) == 110.0);
    p["main.lisp"] = "(import \"voices\")\n(defpiece main () (sine-osc :freq root :out (pipe :audio)))";
    CHECK_THAT(error(project(p)), ContainsSubstring("unbound symbol root"));
}

TEST_CASE(":only and :as choose and rename the imported names") {
    const files lib{{"lib.lisp", "(define (tone out) (sine-osc :freq 220hz :out out))\n(define level 0.5)"}};
    auto p = lib;
    p["main.lisp"] = "(import \"lib\" :as lib)\n(defpiece main () (lib/tone (pipe :audio)))";
    CHECK(ok(project(p)).nodes.size() == 1);
    p["main.lisp"] = "(import \"lib\" :only (tone))\n(defpiece main () (tone (pipe :audio)) level)";
    CHECK_THAT(error(project(p)), ContainsSubstring("unbound symbol level"));
    p["main.lisp"] = "(import \"lib\" :only (gain))\n" + std::string(tone());
    CHECK_THAT(error(project(p)), ContainsSubstring("lib.lisp does not export gain"));
}

TEST_CASE("each file loads once, and classes and generic functions are shared") {
    const auto g = ok(project({
        {"voice.lisp",
         "(defclass voice () ((freq :initarg :freq :accessor freq)))\n(defgeneric play (v out))"},
        {"sine.lisp",
         "(import \"voice\")\n(defmethod play ((v voice) out) (sine-osc :freq (freq v) :out out))"},
        {"main.lisp", "(import \"voice\")\n(import \"sine\")\n(defpiece main () (play (make voice :freq "
                      "99hz) (pipe :audio)))"},
    }));
    CHECK(std::get<double>(find(g, "sineOsc").params.at("freq")) == 99.0);
}

TEST_CASE("import cycles and missing files are errors (interpreter.md 11.3)") {
    CHECK_THAT(error(project({{"a.lisp", "(import \"b\")"},
                              {"b.lisp", "(import \"a\")"},
                              {"main.lisp", "(import \"a\")\n" + std::string(tone())}})),
               ContainsSubstring("import cycle: a.lisp imports b.lisp imports a.lisp"));
    CHECK_THAT(error(project({{"main.lisp", "(import \"main\")\n" + std::string(tone())}})),
               ContainsSubstring("import cycle: main.lisp imports main.lisp"));
    CHECK_THAT(error(project({{"main.lisp", "(import \"drums\")\n" + std::string(tone())}})),
               ContainsSubstring("cannot import drums.lisp: no such file in the project"));
    CHECK_THAT(error(project({{"main.lisp", "(import \"../x\")\n" + std::string(tone())}})),
               ContainsSubstring("relative to the project root"));
    CHECK_THAT(error(project({{"main.lisp", "(defpiece main () (import \"x\"))"}})),
               ContainsSubstring("import is a top-level form"));
}

TEST_CASE("errors in an imported file name that file and the call sites") {
    const auto r = project({
        {"voices.lisp", "(define (tone out)\n  (sine-osc :freq 1s :out out))"},
        {"main.lisp", "(import \"voices\")\n" + std::string(tone())},
    });
    REQUIRE_FALSE(r.ok());
    CHECK(r.errors[0].file == "voices.lisp");
    CHECK(r.errors[0].span.line == 2);
    REQUIRE(r.errors[0].stack.size() == 2);
    CHECK(r.errors[0].stack[1].file == "main.lisp");
    CHECK(r.errors[0].stack[1].span.line == 2);
    const auto bad =
        project({{"voices.lisp", "(define x"}, {"main.lisp", "(import \"voices\")\n" + std::string(tone())}});
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.errors[0].file == "voices.lisp");
}
