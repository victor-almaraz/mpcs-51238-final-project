// Every example compiles to valid IR the same way every time
// (interpreter.md §14.1): each piece of one file, NAME/NAME.lisp with its
// project.lisp when it has one, and each project of several files, entered
// at main.lisp (§10.3: the same IDs every time). The server's tests play
// the pieces and check that each behaves as described. The GUI editor's
// templates, from which New Project starts, compile the same way. Each
// example's page is drawn from its layout or is its own.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>

#include "support/compile.h"
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

// The addresses of a layout's elements, at any depth.
void addresses(const metadata::pageNode& node, std::set<std::string>& out) {
    if (!node.address.empty()) out.insert(node.address);
    for (const auto& child : node.children) addresses(child, out);
}

}  // namespace

// An example with a page draws it from its layout (dsl-ui.md §7.1) or has
// its own ui/index.html, never both, since the project's page would hide
// the drawn one. A drawn page shows every control of the piece: each
// osc-in on the page port with an :initial is a control of its layout.
TEST_CASE("each example's page is drawn from its layout or is its own, and shows every control") {
    int drawn = 0;
    for (const auto& e : fs::directory_iterator(fs::path(ESPRESSO_SOURCE_DIR) / "examples")) {
        if (!fs::is_directory(e.path())) continue;
        const bool project = fs::exists(e.path() / "main.lisp");
        const auto single = e.path() / (e.path().filename().string() + ".lisp");
        if (!project && !fs::exists(single)) continue;
        const auto graph = project ? support::projectGraph(e.path()) : support::pieceGraph(single);
        if (!graph.pagePort) continue;
        if (graph.layout) ++drawn;
        DYNAMIC_SECTION(e.path().filename().string()) {
            const bool own = fs::exists(e.path() / "ui" / "index.html");
            CHECK(own != graph.layout.has_value());
            std::set<std::string> shown;
            if (graph.layout) addresses(*graph.layout, shown);
            for (const auto& n : graph.nodes) {
                if (own || n.type != "oscIn" || !n.params.contains("initial")) continue;
                if (std::get<double>(n.params.at("port")) != *graph.pagePort) continue;
                const auto& address = std::get<std::string>(n.params.at("address"));
                INFO(address << " is read by the piece and no control of its page");
                CHECK(shown.contains(address));
            }
        }
    }
    CHECK(drawn >= 13);
}
