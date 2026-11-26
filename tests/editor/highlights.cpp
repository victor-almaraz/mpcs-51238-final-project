// The sidecar's highlighting of every .lisp file under the given
// directories, for the GUI editor's conformance test (tauri-editor.md
// §5.4): each file's text goes to the sidecar (serve.h) as a `highlight`
// request, and its path, text, and the spans answered are written as JSON.
//
//   editor_highlights <output.json> <directory>...

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <espresso/workspace/interpreterLink.h>

#include "../arguments.h"  // src/apps/arguments.h, beside the client's directory
#include "serve.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

struct source {
    std::string path;  // the directory's name, then the path inside it
    std::string text;
};

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Every .lisp file under @p directory, sorted by path.
std::vector<source> lispFiles(const fs::path& directory) {
    std::vector<fs::path> paths;
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".lisp") paths.push_back(entry.path());
    }
    std::ranges::sort(paths);
    std::vector<source> files;
    files.reserve(paths.size());
    for (const auto& path : paths) {
        files.push_back({.path = (directory.filename() / path.lexically_relative(directory)).generic_string(),
                         .text = readFile(path)});
    }
    return files;
}

// The sidecar's answers to one `highlight` request a file, by request id
// (the file's index), or nothing when a request failed.
std::vector<json> highlightAll(const std::vector<source>& files) {
    std::string input;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const json request{
            {"jsonrpc", "2.0"}, {"id", i}, {"method", "highlight"}, {"params", {{"text", files[i].text}}}};
        input += request.dump() + "\n";
    }
    std::istringstream in(input);
    std::ostringstream out;
    // No run request comes, so the link never reaches for a server.
    const workspace::linkOptions link{.server = {.serverPort = 1, .timeoutMs = 5, .retries = 0},
                                      .clientVersion = "editor_highlights",
                                      .connectOnStart = false};
    if (client::serve(in, out, {.link = link}) != 0) return {};
    std::vector<json> spans(files.size());
    std::istringstream lines(out.str());
    for (std::string line; std::getline(lines, line);) {
        const json message = json::parse(line);
        if (!message.contains("id")) continue;  // a notification
        if (!message.contains("result")) {
            std::cerr << "editor_highlights: " << message.dump() << "\n";
            return {};
        }
        spans.at(message["id"].get<std::size_t>()) = message["result"]["spans"];
    }
    return spans;
}

}  // namespace

int main(int argc, char** argv) {
    const auto args = apps::arguments(argc, argv);
    if (args.size() < 3) {
        std::cerr << "usage: editor_highlights <output.json> <directory>...\n";
        return 2;
    }
    std::vector<source> files;
    for (const auto directory : std::span(args).subspan(2)) {
        std::ranges::move(lispFiles(fs::path(directory)), std::back_inserter(files));
    }
    const auto spans = highlightAll(files);
    if (spans.size() != files.size()) return 1;
    json written = json::array();
    for (std::size_t i = 0; i < files.size(); ++i) {
        written.push_back({{"path", files[i].path}, {"text", files[i].text}, {"spans", spans[i]}});
    }
    std::ofstream output{std::string(args[1])};
    output << written.dump() << "\n";
    if (!output) {
        std::cerr << "editor_highlights: cannot write " << args[1] << "\n";
        return 1;
    }
    return 0;
}
