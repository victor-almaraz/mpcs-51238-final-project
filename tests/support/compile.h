// Compiles pieces and example projects in tests and reads the results.

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/interpreter/project.h>
#include <espresso/metadata/graph.h>

#include "support/files.h"
#include "support/unwrap.h"

namespace support {

/// The graph of the project in folder @p root: its main.lisp compiled with
/// its project file, when it has one, and imports from the folder, as
/// `espresso-client --compile` does; fails the test when it does not
/// compile.
inline metadata::graphSpec projectGraph(const std::filesystem::path& root) {
    const auto read = [&root](const std::string& path) {
        if (!std::filesystem::exists(root / path))
            return interpreter::loadedFile{.text = std::nullopt, .error = {}};
        return interpreter::loadedFile{.text = readText(root / path), .error = {}};
    };
    const auto compiled = interpreter::compileProject(unwrap(read("main.lisp").text), "main.lisp", {},
                                                      read(std::string(interpreter::projectFile)).text, read);
    INFO((compiled.projectErrors.empty() ? std::string() : compiled.projectErrors.front().text()));
    INFO(compiled.piece.text());
    REQUIRE(compiled.piece.ok());
    return unwrap(compiled.piece.graph);
}

/// Every example folder that is a project of several files, entered at
/// main.lisp, sorted.
inline std::vector<std::filesystem::path> exampleProjects() {
    std::vector<std::filesystem::path> out;
    for (const auto& e :
         std::filesystem::directory_iterator(std::filesystem::path(ESPRESSO_SOURCE_DIR) / "examples")) {
        if (e.is_directory() && std::filesystem::exists(e.path() / "main.lisp")) out.push_back(e.path());
    }
    std::ranges::sort(out);
    return out;
}

/// The source of the piece in @p lisp, NAME/NAME.lisp, compiled as
/// `espresso-client --compile` does: with its folder's project file when it
/// has one, and no loader, so an import is an error.
inline interpreter::projectCompileResult compileExample(const std::filesystem::path& lisp) {
    const auto projectPath = lisp.parent_path() / interpreter::projectFile;
    const auto projectText =
        std::filesystem::exists(projectPath) ? std::optional(readText(projectPath)) : std::nullopt;
    return interpreter::compileProject(readText(lisp), lisp.filename().string(), {}, projectText, {});
}

/// Every example that is a piece of one file, NAME/NAME.lisp in a folder
/// without a main.lisp, sorted.
inline std::vector<std::filesystem::path> examplePieces() {
    std::vector<std::filesystem::path> out;
    for (const auto& e :
         std::filesystem::directory_iterator(std::filesystem::path(ESPRESSO_SOURCE_DIR) / "examples")) {
        const auto own = e.path() / (e.path().filename().string() + ".lisp");
        if (e.is_directory() && std::filesystem::exists(own) &&
            !std::filesystem::exists(e.path() / "main.lisp"))
            out.push_back(own);
    }
    std::ranges::sort(out);
    return out;
}

/// The graph of the piece in @p lisp (examplePieces()), compiled once per
/// test process; fails the test when it does not compile. Several cases
/// play the same pieces, and a Debug compile of the largest (straintest)
/// takes a second or more.
inline const metadata::graphSpec& pieceGraph(const std::filesystem::path& lisp) {
    static std::map<std::filesystem::path, metadata::graphSpec> compiled;
    if (const auto it = compiled.find(lisp); it != compiled.end()) return it->second;
    const auto result = compileExample(lisp);
    INFO((result.projectErrors.empty() ? std::string() : result.projectErrors.front().text()));
    INFO(result.piece.text());
    REQUIRE(result.piece.ok());
    return compiled.emplace(lisp, unwrap(result.piece.graph)).first->second;
}

/// Compiles a piece whose body is @p body, after @p before; the body
/// starts on line 3.
inline interpreter::compileResult piece(const std::string& body, const std::string& before = "") {
    return interpreter::compile(before + "\n(defpiece main ()\n" + body + ")\n", "test.lisp");
}

/// The graph @p r compiled to; fails the test when it has errors.
inline metadata::graphSpec ok(const interpreter::compileResult& r) {
    INFO(r.text());
    REQUIRE(r.ok());
    return unwrap(r.graph);
}

/// The errors of @p r; fails the test when it compiled.
inline std::string error(const interpreter::compileResult& r) {
    REQUIRE_FALSE(r.ok());
    return r.text();
}

/// Node @p which (0 first) of type @p type in @p g.
inline const metadata::nodeSpec& find(const metadata::graphSpec& g, const std::string& type,
                                      std::size_t which = 0) {
    for (const auto& n : g.nodes) {
        if (n.type == type && which-- == 0) return n;
    }
    FAIL("no node of type " + type);
    return g.nodes.front();
}

/// The plain number @p expr evaluates to in a piece, after @p before: the
/// offset of an lfo node, which takes any number.
inline double valueOf(const std::string& expr, const std::string& before = "") {
    const auto g = ok(piece("(lfo :offset " + expr + " :out (pipe :control))", before));
    return std::get<double>(find(g, "lfo").params.at("offset"));
}

/// Nodes of type @p type in @p g.
inline std::ptrdiff_t count(const metadata::graphSpec& g, const std::string& type) {
    return std::ranges::count_if(g.nodes, [&](const metadata::nodeSpec& n) { return n.type == type; });
}

}  // namespace support
