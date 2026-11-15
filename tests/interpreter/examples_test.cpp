// Every examples/NAME.lisp compiles to examples/NAME.ir.json
// (interpreter.md §14.1). Set PROJ_UPDATE_EXAMPLES=1 to rewrite the IR
// files after an intended change, then review the diff.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>

#include <proj/interpreter/compiler.h>
#include <proj/ir/ir.h>

#include "support/unwrap.h"

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

} // namespace

TEST_CASE("examples compile to their IR files") {
    const fs::path dir = fs::path(PROJ_SOURCE_DIR) / "examples";
    // The C library reports an environment value by pointer; only its
    // presence is read.
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing in the suite modifies the environment
    const bool update = std::getenv("PROJ_UPDATE_EXAMPLES") != nullptr;
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".lisp") continue;
        ++count;
        const fs::path& lisp = entry.path();
        const fs::path irFile = fs::path(lisp).replace_extension(".ir.json");
        DYNAMIC_SECTION(lisp.filename().string()) {
            const auto source = slurp(lisp);
            const auto result = interpreter::compile(source, lisp.filename().string());
            INFO(result.text());
            REQUIRE(result.ok());
            const auto& graph = support::unwrap(result.graph);
            CHECK(ir::write(graph) ==
                  ir::write(support::unwrap(interpreter::compile(source, lisp.filename().string()).graph)));
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
    CHECK(count >= 3);
}
