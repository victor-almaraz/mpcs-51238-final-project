// Every example's NAME/NAME.lisp, in a folder of its own with its samples
// and its project.lisp when it has one, compiles to the NAME.ir.json beside
// it (interpreter.md §14.1), and every project of several files, entered at
// main.lisp, compiles to valid IR. Set
// ESPRESSO_UPDATE_EXAMPLES=1 to rewrite the IR files after an intended
// change, then review the diff.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/interpreter/project.h>
#include <espresso/ir/ir.h>

#include "support/compile.h"
#include "support/files.h"
#include "support/unwrap.h"

namespace fs = std::filesystem;

TEST_CASE("examples compile to their IR files") {
    const fs::path examples = fs::path(ESPRESSO_SOURCE_DIR) / "examples";
    // The C library reports an environment value by pointer; only its
    // presence is read.
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing in the suite modifies the environment
    const bool update = std::getenv("ESPRESSO_UPDATE_EXAMPLES") != nullptr;
    std::size_t count = 0;
    // A folder's piece is the one with an IR file of its name; projects of
    // several files live in folders without one.
    std::vector<fs::path> pieces;
    for (const auto& entry : fs::directory_iterator(examples)) {
        const fs::path own = entry.path() / (entry.path().filename().string() + ".lisp");
        if (entry.is_directory() && fs::exists(fs::path(own).replace_extension(".ir.json")))
            pieces.push_back(own);
    }
    for (const fs::path& lisp : pieces) {
        {
            ++count;
            const fs::path irFile = fs::path(lisp).replace_extension(".ir.json");
            DYNAMIC_SECTION(fs::relative(lisp, examples).string()) {
                const auto source = support::readText(lisp);
                // A folder's project file gives its page port, as the editor's does.
                const fs::path projectPath = lisp.parent_path() / interpreter::projectFile;
                const auto projectText =
                    fs::exists(projectPath) ? std::optional(support::readText(projectPath)) : std::nullopt;
                // A piece of one file: no loader, so an import is an error.
                const auto compileIt = [&] {
                    return interpreter::compileProject(source, lisp.filename().string(), {}, projectText, {});
                };
                const auto compiled = compileIt();
                INFO(
                    (compiled.projectErrors.empty() ? std::string() : compiled.projectErrors.front().text()));
                const auto& result = compiled.piece;
                INFO(result.text());
                REQUIRE(result.ok());
                const auto& graph = support::unwrap(result.graph);
                CHECK(ir::write(graph) == ir::write(support::unwrap(compileIt().piece.graph)));
                if (update) {
                    std::ofstream(irFile, std::ios::binary) << ir::write(graph, 2) << '\n';
                }
                REQUIRE(fs::exists(irFile));
                const auto expected = ir::read(support::readText(irFile));
                INFO(expected.text());
                REQUIRE(expected.ok());
                CHECK(support::unwrap(expected.graph) == graph);
            }
        }
    }
    CHECK(count >= 3);
}

// A project of several files has no IR file to compare with. Its main.lisp,
// with its project file and imports from its folder, compiles to the same
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
