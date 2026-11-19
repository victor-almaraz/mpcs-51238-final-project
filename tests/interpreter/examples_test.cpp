// Every NAME.lisp in examples/ and examples/misc/, and every NAME/NAME.lisp
// of a folder of its own (with its samples), compiles to the NAME.ir.json
// beside it (interpreter.md §14.1). Set
// ESPRESSO_UPDATE_EXAMPLES=1 to rewrite the IR files after an intended
// change, then review the diff.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/ir/ir.h>

#include "support/unwrap.h"

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

}  // namespace

TEST_CASE("examples compile to their IR files") {
    const fs::path examples = fs::path(ESPRESSO_SOURCE_DIR) / "examples";
    // The C library reports an environment value by pointer; only its
    // presence is read.
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing in the suite modifies the environment
    const bool update = std::getenv("ESPRESSO_UPDATE_EXAMPLES") != nullptr;
    std::size_t count = 0;
    // Single-file pieces, and folders that hold a piece of their name with
    // its sample files; projects of several files live in folders without one.
    std::vector<fs::path> pieces;
    for (const fs::path& dir : {examples, examples / "misc"}) {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() == ".lisp") pieces.push_back(entry.path());
            // A folder's piece is the one with an IR file of its name.
            const fs::path own = entry.path() / (entry.path().filename().string() + ".lisp");
            if (entry.is_directory() && fs::exists(fs::path(own).replace_extension(".ir.json")))
                pieces.push_back(own);
        }
    }
    for (const fs::path& lisp : pieces) {
        {
            ++count;
            const fs::path irFile = fs::path(lisp).replace_extension(".ir.json");
            DYNAMIC_SECTION(fs::relative(lisp, examples).string()) {
                const auto source = slurp(lisp);
                const auto result = interpreter::compile(source, lisp.filename().string());
                INFO(result.text());
                REQUIRE(result.ok());
                const auto& graph = support::unwrap(result.graph);
                CHECK(ir::write(graph) == ir::write(support::unwrap(
                                              interpreter::compile(source, lisp.filename().string()).graph)));
                if (update) {
                    std::ofstream(irFile, std::ios::binary) << ir::write(graph, 2) << '\n';
                }
                REQUIRE(fs::exists(irFile));
                const auto expected = ir::read(slurp(irFile));
                INFO(expected.text());
                REQUIRE(expected.ok());
                CHECK(support::unwrap(expected.graph) == graph);
            }
        }
    }
    CHECK(count >= 3);
}
