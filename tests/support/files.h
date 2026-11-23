// Reading a whole file in tests: fixtures, IR files, and files a test wrote.

#pragma once

#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <utility>

namespace support {

/// The bytes of the file at @p path; empty when it cannot be read.
inline std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return std::move(text).str();
}

}  // namespace support
