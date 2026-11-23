// The project file and the file a project plays (interpreter.md §11.1,
// grammar.md §4.4.1).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <espresso/interpreter/compiler.h>
#include <espresso/interpreter/project.h>
#include <espresso/interpreter/reader.h>
#include <espresso/metadata/graph.h>

namespace interpreter {

namespace {

constexpr std::string_view shape = "(project :name \"piece\" :entry main)";
constexpr auto keys = std::to_array<std::string_view>({"name", "root", "entry", "seed", "page-port"});

// The value @p v of keyword @p key, one of keys, written into @p p; an
// error message when the value is not one the keyword takes.
std::string take(projectSpec& p, std::string_view key, const datum& v) {
    if (key == "name" || key == "root") {
        if (v.kind != datumKind::string) return ":" + std::string(key) + " takes a string";
        if (key == "name") {
            p.name = v.text;
        } else if (v.text != ".") {
            return ":root may only be \".\", the folder that holds project.lisp";
        }
        return {};
    }
    if (key == "entry") {
        if (v.kind != datumKind::symbol) return ":entry takes the name of a piece, such as main";
        p.entry = v.text;
        return {};
    }
    if (key == "seed") {
        if (v.kind != datumKind::integer || v.integer < 0) return ":seed takes a whole number, at least 0";
        p.seed = static_cast<std::uint64_t>(v.integer);
        return {};
    }
    if (v.kind != datumKind::integer || v.integer < metadata::minPagePort ||
        v.integer > metadata::maxPagePort) {
        return ":page-port takes a port from " + std::to_string(metadata::minPagePort) + " to " +
               std::to_string(metadata::maxPagePort);
    }
    p.pagePort = static_cast<int>(v.integer);
    return {};
}

}  // namespace

projectResult readProject(std::string_view source) {
    projectResult result;
    const auto refuse = [&](std::string message, const sourceSpan& span) {
        result.errors.push_back(
            {.message = std::move(message), .file = std::string(projectFile), .span = span, .stack = {}});
        return std::move(result);
    };
    const readResult text = read(source);
    if (text.error) return refuse(text.error->message, text.error->span);
    if (text.data.size() != 1 || text.data[0].kind != datumKind::list || text.data[0].items.empty() ||
        !text.data[0].items[0].isSymbol("project")) {
        return refuse("project.lisp holds one form, such as " + std::string(shape),
                      text.data.empty() ? sourceSpan{} : text.data.front().span);
    }
    const datum& form = text.data[0];
    projectSpec project;
    std::set<std::string_view> given;
    for (std::size_t i = 1; i < form.items.size(); i += 2) {
        const datum& key = form.items[i];
        if (key.kind != datumKind::keyword) {
            return refuse("project takes keyword arguments, such as " + std::string(shape), key.span);
        }
        if (std::ranges::find(keys, key.text) == keys.end()) {
            return refuse("project has no parameter :" + key.text +
                              "; it takes :name, :root, :entry, :seed, and :page-port",
                          key.span);
        }
        if (!given.insert(key.text).second) return refuse(":" + key.text + " is given twice", key.span);
        if (i + 1 == form.items.size()) return refuse(":" + key.text + " needs a value", key.span);
        const datum& value = form.items[i + 1];
        if (auto message = take(project, key.text, value); !message.empty()) {
            return refuse(std::move(message), value.span);
        }
    }
    result.project = std::move(project);
    return result;
}

projectOptionsResult projectOptions(compileOptions options, std::optional<std::string_view> projectText) {
    if (!projectText) return {.options = std::move(options), .errors = {}};
    auto read = readProject(*projectText);
    if (!read.project) return {.options = std::nullopt, .errors = std::move(read.errors)};
    options.entry = std::move(read.project->entry);
    options.seed = read.project->seed;
    options.pagePort = read.project->pagePort;
    return {.options = std::move(options), .errors = {}};
}

projectCompileResult compileProject(std::string_view source, std::string_view file, compileOptions options,
                                    std::optional<std::string_view> projectText,
                                    std::function<loadedFile(const std::string& path)> load,
                                    const projectOverrides& overrides) {
    auto project = projectOptions(std::move(options), projectText);
    if (!project.options) return {.projectErrors = std::move(project.errors), .piece = {}};
    compileOptions& o = *project.options;
    if (overrides.entry) o.entry = *overrides.entry;
    if (overrides.seed) o.seed = *overrides.seed;
    o.load = std::move(load);
    return {.projectErrors = {}, .piece = compile(source, file, o)};
}

std::filesystem::path entryFile(const std::map<std::filesystem::path, std::string>& sources,
                                const std::filesystem::path& current) {
    // The project file is not a piece (§11.1).
    const auto isPiece = [](const auto& source) { return source.first != projectFile; };
    if (sources.contains("main.lisp")) return "main.lisp";
    if (!current.empty() && current != projectFile && sources.contains(current)) return current;
    if (std::ranges::count_if(sources, isPiece) != 1) return {};
    return std::ranges::find_if(sources, isPiece)->first;
}

}  // namespace interpreter
