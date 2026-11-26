// Every example compiles to valid IR the same way every time
// (interpreter.md §14.1): each piece of one file, NAME/NAME.lisp with its
// project.lisp when it has one, and each project of several files, entered
// at main.lisp (§10.3: the same IDs every time). The server's tests play
// the pieces and check that each behaves as described. The GUI editor's
// templates, from which New Project starts, compile the same way. Each
// example's page starts its controls where its piece starts them.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <regex>
#include <string>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>

#include "support/compile.h"
#include "support/files.h"
#include "support/unwrap.h"

namespace fs = std::filesystem;

TEST_CASE("example pieces compile to the same valid IR every time") {
    const auto pieces = support::examplePieces();
    for (const fs::path& lisp : pieces) {
        DYNAMIC_SECTION(lisp.parent_path().filename().string()) {
            const auto& graph = support::pieceGraph(lisp);
            const auto again = support::compileExample(lisp);
            REQUIRE(again.piece.ok());
            const auto text = ir::write(graph);
            CHECK(text == ir::write(support::unwrap(again.piece.graph)));
            const auto problems = ir::validate(graph);
            CHECK(problems.empty());
            const auto back = ir::read(text);
            INFO(back.text());
            REQUIRE(back.ok());
            CHECK(support::unwrap(back.graph) == graph);
        }
    }
    CHECK(pieces.size() >= 14);
}

// A project of several files: its main.lisp, with its project file and
// imports from its folder, compiles to the same
// IR twice, and that IR passes the schema and the semantic checks and
// reads back as the same graph.
TEST_CASE("example projects of several files compile to valid IR") {
    const auto projects = support::exampleProjects();
    for (const fs::path& root : projects) {
        DYNAMIC_SECTION(root.filename().string()) {
            const auto graph = support::projectGraph(root);
            const auto text = ir::write(graph);
            CHECK(text == ir::write(support::projectGraph(root)));
            const auto back = ir::read(text);
            INFO(back.text());
            REQUIRE(back.ok());
            CHECK(support::unwrap(back.graph) == graph);
        }
    }
    CHECK(projects.size() >= 3);
}

// Each of the GUI editor's templates (patterns.md §27), a project entered at
// main.lisp, compiles to valid IR the same way every time.
TEST_CASE("the editor's templates compile to valid IR") {
    const auto templates = support::exampleProjects("src/apps/editor/templates");
    for (const fs::path& root : templates) {
        DYNAMIC_SECTION(root.filename().string()) {
            const auto graph = support::projectGraph(root);
            const auto text = ir::write(graph);
            CHECK(text == ir::write(support::projectGraph(root)));
            CHECK(ir::validate(graph).empty());
        }
    }
    CHECK(templates.size() >= 3);
}

namespace {

// A page's value controls (es-knob, es-slider, es-toggle, es-select): each one's address and the value it
// starts at and sends when the page connects (docs/language.md, Page controls); a toggle without a value
// starts off.
std::map<std::string, double> pageControls(const fs::path& page) {
    const std::string html = support::readText(page);
    const std::regex control(R"(<es-(knob|slider|toggle|select)\b([^>]*)>)");
    const std::regex address(R"rx(\baddress="([^"]*)")rx");
    const std::regex value(R"rx(\bvalue="([^"]*)")rx");
    std::map<std::string, double> out;
    for (auto it = std::sregex_iterator(html.begin(), html.end(), control); it != std::sregex_iterator();
         ++it) {
        const std::string attributes = (*it)[2];
        std::smatch a;
        std::smatch v;
        if (!std::regex_search(attributes, a, address)) continue;
        out[a[1]] = std::regex_search(attributes, v, value) ? std::stod(v[1]) : 0.0;
    }
    return out;
}

// The piece's page controls: each osc-in on the page port with an :initial, by address.
std::map<std::string, double> pieceControls(const metadata::graphSpec& g) {
    std::map<std::string, double> out;
    for (const auto& n : g.nodes) {
        if (n.type != "oscIn" || !n.params.contains("initial") || !g.pagePort) continue;
        if (std::get<double>(n.params.at("port")) != *g.pagePort) continue;
        out[std::get<std::string>(n.params.at("address"))] = std::get<double>(n.params.at("initial"));
    }
    return out;
}

}  // namespace

// A page and its piece agree (io.md §12.3): every osc-in with an :initial has a control at its address that
// starts at that value, and every value control has one, so the piece plays as written without the page and
// the page shows what the piece plays.
TEST_CASE("each example's page starts its controls where its piece starts them") {
    int pages = 0;
    for (const auto& e : fs::directory_iterator(fs::path(ESPRESSO_SOURCE_DIR) / "examples")) {
        const auto page = e.path() / "ui" / "index.html";
        if (!fs::exists(page)) continue;
        const bool project = fs::exists(e.path() / "main.lisp");
        const auto graph = project ? support::projectGraph(e.path())
                                   : support::pieceGraph(e.path() / (e.path().filename().string() + ".lisp"));
        const auto piece = pieceControls(graph);
        if (piece.empty()) continue;  // a page whose piece reads it without :initial (mini-clone, rack)
        ++pages;
        const auto controls = pageControls(page);
        DYNAMIC_SECTION(e.path().filename().string()) {
            for (const auto& [address, initial] : piece) {
                INFO(address);
                REQUIRE(controls.contains(address));
                CHECK(std::abs(controls.at(address) - initial) <= 1e-3 * std::max(1.0, std::abs(initial)));
            }
            for (const auto& [address, value] : controls) {
                INFO(address << " is a control of the page and no osc-in of the piece");
                CHECK(piece.contains(address));
            }
        }
    }
    CHECK(pages >= 2);
}
