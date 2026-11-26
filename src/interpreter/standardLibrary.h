// The standard library: the .lisp files under lib/, compiled into the
// interpreter by cmake/EmbedLibrary.cmake. Import paths that begin with
// std/ name them (interpreter.md §11.2).

#pragma once

#include <span>
#include <string_view>

#include <espresso/interpreter/compiler.h>

namespace interpreter::detail {

using interpreter::libraryFile;

// Every file of the library.
std::span<const libraryFile> standardLibrary();

} // namespace interpreter::detail
