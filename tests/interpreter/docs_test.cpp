// The pieces the documentation shows compile: every `lisp` code block of
// docs/ that defines a piece (`defpiece`) and elides nothing (`...`). A
// block of several files marks each with a comment line `;; NAME.lisp`
// before its text; the piece is in the last, or in main.lisp.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/interpreter/project.h>

#include "support/files.h"

namespace {

namespace fs = std::filesystem;

struct snippet {
    std::string where;  // file:line of the block's opening fence
    std::string text;
};

// Every lisp block of the Markdown files under docs/, except docs/api/.
std::vector<snippet> lispBlocks() {
    std::vector<snippet> out;
    const fs::path docs = fs::path(ESPRESSO_SOURCE_DIR) / "docs";
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(docs)) {
        const auto relative = fs::relative(e.path(), docs);
        if (e.path().extension() == ".md" && *relative.begin() != "api") files.push_back(e.path());
    }
    std::ranges::sort(files);
    for (const fs::path& file : files) {
        std::istringstream in(support::readText(file));
        std::string line;
        int number = 0;
        std::optional<snippet> open;
        while (std::getline(in, line)) {
            ++number;
            if (open) {
                if (line == "```") {
                    out.push_back(*open);
                    open.reset();
                } else {
                    open->text += line + "\n";
                }
            } else if (line == "```lisp") {
                open = snippet{.where = fs::relative(file, ESPRESSO_SOURCE_DIR).string() + ":" +
                                        std::to_string(number),
                               .text = {}};
            }
        }
    }
    return out;
}

// The block's files by name: one main.lisp, or one file a `;; NAME.lisp` line opens.
std::map<std::string, std::string> filesOf(const std::string& text, std::string& entry) {
    static const std::regex marker(R"(^;; ([\w./-]+\.lisp)$)");
    std::map<std::string, std::string> files;
    std::istringstream in(text);
    std::string line;
    std::string current = "main.lisp";
    entry = current;
    std::smatch m;
    while (std::getline(in, line)) {
        if (std::regex_match(line, m, marker)) {
            current = m[1].str();
            entry = current;
            continue;
        }
        files[current] += line + "\n";
    }
    if (files.contains("main.lisp") && files.size() > 1 &&
        files["main.lisp"].find("(defpiece") != std::string::npos) {
        entry = "main.lisp";
    }
    return files;
}

}  // namespace

TEST_CASE("every piece the documentation shows compiles") {
    int compiled = 0;
    for (const snippet& s : lispBlocks()) {
        if (s.text.find("(defpiece") == std::string::npos || s.text.find("...") != std::string::npos)
            continue;
        DYNAMIC_SECTION(s.where) {
            std::string entry;
            const auto files = filesOf(s.text, entry);
            const auto load = [&files](const std::string& path) {
                const auto it = files.find(path);
                return it == files.end() ? interpreter::loadedFile{.text = std::nullopt, .error = {}}
                                         : interpreter::loadedFile{.text = it->second, .error = {}};
            };
            // A page port, so that pieces with controls and chains compile.
            const auto result =
                interpreter::compileProject(files.at(entry), entry, {}, "(project :page-port 8000)", load);
            INFO(result.piece.text());
            CHECK(result.piece.ok());
        }
        ++compiled;
    }
    CHECK(compiled >= 9);
}
