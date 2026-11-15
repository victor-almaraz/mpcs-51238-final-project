// Checked access to optional values in tests. REQUIRE stops a test when
// an optional is empty, but static analysis cannot see that it throws, so
// tests read optionals through unwrap() instead of operator* or ->.

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stdexcept>
#include <utility>

namespace support {

/// The value of @p o; fails the test when @p o is empty.
template <typename T>
const T& unwrap(const std::optional<T>& o) {
    if (!o) {
        FAIL("an optional value is empty");
        throw std::logic_error("unreachable: FAIL throws");
    }
    return *o;
}

/// The value of @p o; fails the test when @p o is empty.
template <typename T>
T& unwrap(std::optional<T>& o) {
    if (!o) {
        FAIL("an optional value is empty");
        throw std::logic_error("unreachable: FAIL throws");
    }
    return *o;
}

/// The value of the temporary @p o, by value; fails the test when @p o is
/// empty.
template <typename T>
T unwrap(std::optional<T>&& o) {
    if (!o) {
        FAIL("an optional value is empty");
        throw std::logic_error("unreachable: FAIL throws");
    }
    return std::move(*o);
}

}  // namespace support
