// The pieces the documentation shows compile: every `lisp` code block of
// docs/, and every code sample of the landing page, that defines a piece
// (`defpiece`) and elides nothing (`...`). A block of several files marks
// each with a comment line `;; NAME.lisp` before its text; the piece is in
// the last, or in main.lisp.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/interpreter/project.h>

#include "support/compile.h"
#include "support/files.h"

namespace {

namespace fs = std::filesystem;

struct snippet {
    std::string where;  // file:line of the block's opening fence
    std::string text;
};

// Every lisp block of the Markdown files under docs/.
std::vector<snippet> lispBlocks() {
    std::vector<snippet> out;
    const fs::path docs = fs::path(ESPRESSO_SOURCE_DIR) / "docs";
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(docs)) {
        if (e.path().extension() == ".md") files.push_back(e.path());
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

// The code samples of the landing page, site/index.html: each
// `<pre class="code">`, its highlighting spans removed.
std::vector<snippet> siteBlocks() {
    static const std::regex block(R"(<pre class="code">([\s\S]*?)</pre>)");
    static const std::regex tag("<[^>]+>");
    const std::string page = support::readText(fs::path(ESPRESSO_SOURCE_DIR) / "site" / "index.html");
    std::vector<snippet> out;
    for (auto it = std::sregex_iterator(page.begin(), page.end(), block); it != std::sregex_iterator();
         ++it) {
        std::string text = std::regex_replace((*it)[1].str(), tag, "");
        for (const auto& [entity, character] :
             {std::pair{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&amp;", "&"}}) {
            text = std::regex_replace(text, std::regex(entity), character);
        }
        out.push_back({.where = "site/index.html", .text = text});
    }
    return out;
}

// A block's files by name, and the file that holds its piece: main.lisp
// when it does, or else the file the last `;; NAME.lisp` line opens.
struct pieceFiles {
    std::map<std::string, std::string> files;
    std::string entry = "main.lisp";
};

pieceFiles filesOf(const std::string& text) {
    static const std::regex marker(R"(^;; ([\w./-]+\.lisp)$)");
    pieceFiles out;
    std::istringstream in(text);
    std::string line;
    std::string current = out.entry;
    std::smatch m;
    while (std::getline(in, line)) {
        if (std::regex_match(line, m, marker)) {
            current = m[1].str();
            continue;
        }
        out.files[current] += line + "\n";
    }
    const auto main = out.files.find("main.lisp");
    if (main == out.files.end() || main->second.find("(defpiece") == std::string::npos) out.entry = current;
    return out;
}

}  // namespace

TEST_CASE("every piece the documentation shows compiles") {
    int compiled = 0;
    std::vector<snippet> blocks = lispBlocks();
    std::ranges::move(siteBlocks(), std::back_inserter(blocks));
    for (const snippet& s : blocks) {
        if (s.text.find("(defpiece") == std::string::npos || s.text.find("...") != std::string::npos)
            continue;
        DYNAMIC_SECTION(s.where) {
            const auto [files, entry] = filesOf(s.text);
            // A page port, so that pieces with controls and chains compile.
            const auto result = interpreter::compileProject(
                files.at(entry), entry, {}, "(project :page-port 8000)", support::mapLoader(files));
            INFO(result.piece.text());
            CHECK(result.piece.ok());
        }
        ++compiled;
    }
    CHECK(compiled >= 9);
}
