// The standard library: the .lisp files under lib/, compiled into the
// interpreter by cmake/EmbedLibrary.cmake. Import paths that begin with
// std/ name them (interpreter.md §11.2).

#pragma once

#include <span>
#include <string_view>

namespace interpreter::detail {

// One file of the library, by its path under lib/.
struct libraryFile {
    std::string_view path;
    std::string_view text;
};

// Every file of the library.
std::span<const libraryFile> standardLibrary();

} // namespace interpreter::detail
