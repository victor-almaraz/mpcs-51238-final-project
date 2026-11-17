/// @file
/// Test support: a temporary directory that is removed, with everything
/// in it, when the test ends.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <unistd.h>

namespace support {

class tempDir {
public:
    tempDir() {
        std::string tmpl = (std::filesystem::temp_directory_path() / "espresso-test-XXXXXX").string();
        if (::mkdtemp(tmpl.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
        root_ = tmpl;
    }

    ~tempDir() {
        std::error_code ec;  // tests may leave the directory read-only
        std::filesystem::permissions(root_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, ec);
        std::filesystem::remove_all(root_, ec);
    }

    tempDir(const tempDir&) = delete;
    tempDir& operator=(const tempDir&) = delete;

    const std::filesystem::path& path() const noexcept { return root_; }
    std::filesystem::path operator/(const std::filesystem::path& rel) const { return root_ / rel; }

    /// Writes @p bytes to @p rel, creating parent directories.
    void write(const std::filesystem::path& rel, std::string_view bytes) const {
        const auto target = root_ / rel;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream(target, std::ios::binary) << bytes;
    }

private:
    std::filesystem::path root_;
};

} // namespace support
