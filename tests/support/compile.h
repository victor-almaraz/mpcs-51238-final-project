// Compiles pieces in interpreter tests and reads the results.

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>

#include <proj/interpreter/compiler.h>
#include <proj/metadata/graph.h>

#include "support/unwrap.h"

namespace support {

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

}  // namespace support
