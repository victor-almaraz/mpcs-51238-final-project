// The project file and the page port (interpreter.md §11.1, io.md §9.5,
// §12.3).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

#include <espresso/interpreter/compiler.h>
#include <espresso/interpreter/project.h>

#include "support/compile.h"
#include "support/unwrap.h"

using Catch::Matchers::ContainsSubstring;
using namespace interpreter;
using support::unwrap;

namespace {

// The one error of reading @p text, as `line:column: message`.
std::string refusal(const std::string& text) {
    const auto r = readProject(text);
    REQUIRE_FALSE(r.project);
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].file == "project.lisp");
    return r.errors[0].text();
}

// A piece that reads OSC on @p port, compiled with @p options.
compileResult oscPiece(const std::string& port, const compileOptions& options) {
    return compile("(defpiece main ()\n  (osc-in :port " + port +
                       " :value (pipe :control))\n"
                       "  (osc-out :port " +
                       port + " :in (pipe :event)))\n",
                   "main.lisp", options);
}

}  // namespace

TEST_CASE("a project file names its entry seed and page port") {
    const auto r = readProject(R"((project :name "drift" :root "." :entry intro :seed 7 :page-port 8000))");
    INFO((r.errors.empty() ? "" : r.errors[0].text()));
    const auto& p = unwrap(r.project);
    CHECK(p.name == "drift");
    CHECK(p.entry == "intro");
    CHECK(p.seed == 7);
    CHECK(p.pagePort == 8000);

    const auto options = withProject({}, p);
    CHECK(options.entry == "intro");
    CHECK(options.seed == 7);
    CHECK(options.pagePort == 8000);
}

TEST_CASE("a project file without arguments keeps the defaults") {
    const auto& p = unwrap(readProject(";; the defaults\n(project)\n").project);
    CHECK(p.name.empty());
    CHECK(p.entry == "main");
    CHECK(p.seed == 0);
    CHECK_FALSE(p.pagePort);
}

TEST_CASE("a project file refuses what it does not take at the offending text") {
    CHECK(refusal("") ==
          "project.lisp:1:1: project.lisp holds one form, such as (project :name \"piece\" :entry main)");
    CHECK_THAT(refusal("(project) (project)"), ContainsSubstring("holds one form"));
    CHECK_THAT(refusal("(defpiece main ())"), ContainsSubstring("holds one form"));
    CHECK_THAT(refusal("(project"), ContainsSubstring("project.lisp:1:1:"));
    CHECK(refusal("(project :name \"a\"\n  :tempo 120)") ==
          "project.lisp:2:3: project has no parameter :tempo; it takes :name, :root, :entry, :seed, and "
          ":page-port");
    CHECK(refusal("(project :seed 1 :seed 2)") == "project.lisp:1:18: :seed is given twice");
    CHECK(refusal("(project :name)") == "project.lisp:1:10: :name needs a value");
    CHECK(
        refusal("(project \"drift\")") ==
        "project.lisp:1:10: project takes keyword arguments, such as (project :name \"piece\" :entry main)");
    CHECK(refusal("(project :name drift)") == "project.lisp:1:16: :name takes a string");
    CHECK(refusal("(project :root \"..\")") ==
          "project.lisp:1:16: :root may only be \".\", the folder that holds project.lisp");
    CHECK(refusal("(project :entry \"main\")") ==
          "project.lisp:1:17: :entry takes the name of a piece, such as main");
    for (const std::string seed : {"-1", "1.5", "7s"}) {
        CHECK_THAT(refusal("(project :seed " + seed + ")"),
                   ContainsSubstring(":seed takes a whole number, at least 0"));
    }
    for (const std::string port : {"1023", "65536", "8000.0", "\"8000\""}) {
        INFO(port);
        CHECK_THAT(refusal("(project :page-port " + port + ")"),
                   ContainsSubstring(":page-port takes a port from 1024 to 65535"));
    }
    CHECK(readProject("(project :page-port 1024)").project);
    CHECK(readProject("(project :page-port 65535)").project);
}

TEST_CASE("the page port goes into the graph and :port :page stands for it") {
    compileOptions options;
    options.pagePort = 8000;
    const auto g = support::ok(oscPiece(":page", options));
    CHECK(g.pagePort == 8000);
    CHECK(std::get<double>(support::find(g, "oscIn").params.at("port")) == 8000.0);
    CHECK(std::get<double>(support::find(g, "oscOut").params.at("port")) == 8000.0);

    // A number stays a number in a project with a page.
    const auto udp = support::ok(oscPiece("9000", options));
    CHECK(udp.pagePort == 8000);
    CHECK(std::get<double>(support::find(udp, "oscIn").params.at("port")) == 9000.0);
}

TEST_CASE("a project without a page port has none in its graph and refuses :port :page") {
    CHECK_FALSE(support::ok(oscPiece("9000", {})).pagePort);
    CHECK_THAT(
        support::error(oscPiece(":page", {})),
        ContainsSubstring("main.lisp:2:17: parameter :port is :page, but the project has no page port; add "
                          ":page-port to project.lisp"));
    // Only a port takes :page.
    compileOptions options;
    options.pagePort = 8000;
    CHECK_THAT(
        support::error(compile("(defpiece main () (osc-in :port 9000 :address :page :value (pipe :control)))",
                               "main.lisp", options)),
        ContainsSubstring("parameter :address expects a string, got :page"));
    CHECK_THAT(support::error(compile("(defpiece main () (sine-osc :freq :page :out (pipe :audio)))",
                                      "main.lisp", options)),
               ContainsSubstring("parameter :freq expects a number, got :page"));
}
