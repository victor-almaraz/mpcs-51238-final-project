// Every example compiles to valid IR the same way every time
// (interpreter.md §14.1): each piece of one file, NAME/NAME.lisp with its
// project.lisp when it has one, and each project of several files, entered
// at main.lisp (§10.3: the same IDs every time). The server's tests play
// the pieces and check that each behaves as described.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/ir/ir.h>

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
