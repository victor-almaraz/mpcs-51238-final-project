#include <catch2/catch_test_macros.hpp>

#include <proj/tui/fileStore.h>
#include <proj/tui/utf8.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using tui::fileStore;

namespace {

struct tempDir {
    fs::path path;
    tempDir() {
        std::string tmpl = (fs::temp_directory_path() / "proj-tui-XXXXXX").string();
        path = ::mkdtemp(tmpl.data());
    }
    ~tempDir() {
        std::error_code ec;
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::add, ec);
        fs::remove_all(path, ec);
    }
};

void writeRaw(const fs::path& p, const std::string& bytes) {
    std::ofstream(p, std::ios::binary) << bytes;
}

} // namespace

TEST_CASE("utf8 validation", "[tui][utf8]") {
    CHECK(tui::utf8::isValid("plain"));
    CHECK(tui::utf8::isValid("é€😀"));
    CHECK_FALSE(tui::utf8::isValid("\xff"));
    CHECK_FALSE(tui::utf8::isValid("\xc3"));          // truncated
    CHECK_FALSE(tui::utf8::isValid("\xc0\xaf"));      // overlong
    CHECK_FALSE(tui::utf8::isValid("\xed\xa0\x80"));  // surrogate
    CHECK(tui::utf8::length("é€x") == 3);
}

TEST_CASE("fileStore reads UTF-8 and rejects other encodings (tui.md 7.1)", "[tui][fileStore]") {
    tempDir dir;
    writeRaw(dir.path / "ok.lisp", "(sine-osc :freq 440hz)\n");
    writeRaw(dir.path / "latin1.lisp", "caf\xe9");

    const auto ok = fileStore::read(dir.path / "ok.lisp");
    REQUIRE(ok.text);
    CHECK(*ok.text == "(sine-osc :freq 440hz)\n");

    const auto bad = fileStore::read(dir.path / "latin1.lisp");
    CHECK_FALSE(bad.text);
    CHECK(bad.error.find("latin1.lisp") != std::string::npos);

    CHECK_FALSE(fileStore::read(dir.path / "missing.lisp").text);
}

TEST_CASE("fileStore saves atomically (tui.md 7.2)", "[tui][fileStore]") {
    tempDir dir;
    const fs::path target = dir.path / "main.lisp";
    writeRaw(target, "old");
    ::chmod(target.c_str(), 0640);

    REQUIRE_FALSE(fileStore::writeAtomic(target, "new"));
    CHECK(*fileStore::read(target).text == "new");
    CHECK((fs::status(target).permissions() & fs::perms::all) ==
          (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read));

    // No temporary files are left behind.
    int entries = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator(dir.path)) ++entries;
    CHECK(entries == 1);
}

TEST_CASE("a failed save leaves the original unchanged", "[tui][fileStore]") {
    if (::geteuid() == 0) SKIP("permission checks do not apply to root");
    tempDir dir;
    const fs::path target = dir.path / "main.lisp";
    writeRaw(target, "original");
    fs::permissions(dir.path, fs::perms::owner_read | fs::perms::owner_exec);

    const auto error = fileStore::writeAtomic(target, "replacement");
    CHECK(error);
    CHECK(*fileStore::read(target).text == "original");
}

TEST_CASE("fileStore lists project files", "[tui][fileStore]") {
    tempDir dir;
    fs::create_directories(dir.path / "util");
    fs::create_directories(dir.path / ".git");
    fs::create_directories(dir.path / "build");
    writeRaw(dir.path / "main.lisp", "");
    writeRaw(dir.path / "util" / "scales.lisp", "");
    writeRaw(dir.path / ".git" / "HEAD", "");
    writeRaw(dir.path / "build" / "out", "");
    writeRaw(dir.path / ".hidden", "");

    const auto files = fileStore::listProject(dir.path);
    REQUIRE(files.size() == 2);
    CHECK(files[0] == fs::path("main.lisp"));
    CHECK(files[1] == fs::path("util/scales.lisp"));
}
