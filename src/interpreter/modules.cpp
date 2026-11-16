// Projects and modules: files, module forms, and import
// (interpreter.md §11, grammar.md §4.4).

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <proj/interpreter/reader.h>

#include "evaluator.h"
#include "standardLibrary.h"

namespace interpreter {

bool isLibraryPath(std::string_view path) noexcept { return path.starts_with("std/"); }

namespace detail {

std::size_t evaluator::loadFile(const std::string& path, std::string_view source) {
    auto f = std::make_unique<sourceFile>();
    f->path = path;
    f->read = interpreter::read(source);
    f->env = make<environment>();
    f->env->parent = global_;
    files_.push_back(std::move(f));
    return files_.size() - 1;
}

// The text of the file an import names: under std/, the standard
// library's, which a project file cannot replace; otherwise the loader's.
std::string evaluator::sourceOf(const std::string& path, const sourceSpan& span) {
    if (isLibraryPath(path)) {
        const auto library = standardLibrary();
        const auto file = std::ranges::find(library, std::string_view(path), &libraryFile::path);
        if (file == library.end())
            fail(span, "cannot import ", path, ": the standard library has no such file");
        return std::string(file->text);
    }
    if (!options_.load) fail(span, "cannot import ", path, ": this compilation has no project files");
    auto loaded = options_.load(path);
    if (!loaded.text)
        fail(span, "cannot import ", path, ": ",
             loaded.error.empty() ? "no such file in the project" : loaded.error);
    return std::move(*loaded.text);
}

// Evaluates every top-level form of file @p index, once (§11.3).
void evaluator::evaluateFile(std::size_t index) {
    const fileGuard inFile(*this, index);
    sourceFile& f = *files_[index];
    if (f.read.error) fail(f.read.error->span, f.read.error->message);
    for (const auto& form : f.read.data) eval(form, f.env);
    f.loaded = true;
}

// (module name [(export names...)]): without an export clause, a file
// exports every top-level binding it defines.
value evaluator::formModule(const datum& d, const std::shared_ptr<environment>& env) {
    sourceFile& f = *files_[current_];
    const auto& items = d.items;
    if (env != f.env) fail(d.span, "module is a top-level form of a file");
    if (f.declared) fail(d.span, "a file has one module form");
    if (items.size() < 2 || items.size() > 3 || items[1].kind != datumKind::symbol)
        fail(d.span, "expected (module name [(export names...)])");
    f.declared = true;
    if (items.size() == 2) return {};
    const datum& clause = items[2];
    if (clause.kind != datumKind::list || clause.items.empty() || !clause.items[0].isSymbol("export"))
        fail(clause.span, "expected (export names...)");
    f.exportsAll = false;
    for (const auto& name : std::span(clause.items).subspan(1)) {
        if (name.kind != datumKind::symbol) fail(name.span, "export lists names");
        f.exports.push_back(intern(name.text));
    }
    return {};
}

// (import "path" [:only (names...)] [:as prefix]): loads the file at
// path, relative to the project root, with .lisp implied, and binds what
// it exports here; with :as, each name as prefix/name.
value evaluator::formImport(const datum& d, const std::shared_ptr<environment>& env) {
    const auto& items = d.items;
    if (items.size() < 2 || items[1].kind != datumKind::string)
        fail(d.span, "expected (import \"path\" [:only (names...)] [:as prefix])");
    if (env != files_[current_]->env) fail(d.span, "import is a top-level form of a file");
    std::optional<std::span<const datum>> only;
    std::string prefix;
    for (std::size_t i = 2; i < items.size(); i += 2) {
        const datum& option = items[i];
        if (option.kind != datumKind::keyword || i + 1 >= items.size())
            fail(option.span, "expected :only (names...) or :as prefix");
        const datum& arg = items[i + 1];
        if (option.text == "only" && !only && arg.kind == datumKind::list) {
            only = std::span(arg.items);
        } else if (option.text == "as" && prefix.empty() && arg.kind == datumKind::symbol) {
            prefix = arg.text;
        } else {
            fail(option.span, "expected :only (names...) or :as prefix, each at most once");
        }
    }
    std::string path = items[1].text;
    if (!path.ends_with(".lisp")) path += ".lisp";
    if (path.starts_with('/') || path.find("..") != std::string::npos)
        fail(items[1].span, "import paths are relative to the project root and stay inside it");

    const auto found = std::ranges::find_if(files_, [&](const auto& f) { return f->path == path; });
    std::size_t index = 0;
    if (found != files_.end()) {
        index = static_cast<std::size_t>(found - files_.begin());
        if (!files_[index]->loaded) {
            // The files being loaded, from the one imported again to this one.
            std::vector<std::size_t> chain = loading_;
            chain.push_back(current_);
            std::string cycle;
            for (auto at = std::ranges::find(chain, index); at != chain.end(); ++at)
                cycle += files_[*at]->path + " imports ";
            fail(items[1].span, "import cycle: ", cycle, path);
        }
    } else {
        index = loadFile(path, sourceOf(path, items[1].span));
        // The importing file is on the loading list while the import's forms run.
        class loading {
        public:
            loading(std::vector<std::size_t>& files, std::size_t file) : files_(files) {
                files_.push_back(file);
            }
            ~loading() { files_.pop_back(); }
            loading(const loading&) = delete;
            loading& operator=(const loading&) = delete;

        private:
            std::vector<std::size_t>& files_;
        };
        const loading guard(loading_, current_);
        evaluateFile(index);
    }

    const sourceFile& from = *files_[index];
    sourceFile& here = *files_[current_];
    const auto bind = [&](const interned& name, const value& v) {
        const interned as = prefix.empty() ? name : intern(prefix + "/" + *name);
        env->vars[as] = v;
        here.imported.insert(as);
    };
    // Binds exported @p name, which a file defines unless it is a class or
    // a generic function: those are global, so they need no binding.
    const auto bindExported = [&](const interned& name, const sourceSpan& at) {
        if (const auto v = from.env->vars.find(name); v != from.env->vars.end()) return bind(name, v->second);
        const auto global = global_->vars.find(name);
        if (!classes_.contains(name) && (global == global_->vars.end() || global->second.k != kind::generic))
            fail(at, path, " exports ", *name, " but does not define it");
    };
    if (only) {
        for (const auto& n : *only) {
            if (n.kind != datumKind::symbol) fail(n.span, ":only lists names");
            const interned name = intern(n.text);
            const bool exported = from.exportsAll
                                      ? from.env->vars.contains(name) && !from.imported.contains(name)
                                      : std::ranges::find(from.exports, name) != from.exports.end();
            if (!exported) fail(n.span, path, " does not export ", n.text);
            bindExported(name, n.span);
        }
    } else if (from.exportsAll) {
        for (const auto& [name, v] : from.env->vars) {
            if (!from.imported.contains(name)) bind(name, v);
        }
    } else {
        for (const auto& name : from.exports) bindExported(name, items[1].span);
    }
    return {};
}

}  // namespace detail

}  // namespace interpreter
