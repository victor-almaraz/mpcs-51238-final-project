#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include <espresso/tui/fileStore.h>
#include <espresso/tui/utf8.h>

#include "support/tempDir.h"
#include "support/unwrap.h"

namespace fs = std::filesystem;
using tui::fileStore;


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
    support::tempDir dir;
    dir.write("ok.lisp", "(sine-osc :freq 440hz)\n");
    dir.write("latin1.lisp", "caf\xe9");

    const auto ok = fileStore::read(dir / "ok.lisp");
    CHECK(support::unwrap(ok.text) == "(sine-osc :freq 440hz)\n");

    const auto bad = fileStore::read(dir / "latin1.lisp");
    CHECK_FALSE(bad.text);
    CHECK(bad.error.find("latin1.lisp") != std::string::npos);

    CHECK_FALSE(fileStore::read(dir / "missing.lisp").text);
}

TEST_CASE("fileStore saves atomically (tui.md 7.2)", "[tui][fileStore]") {
    support::tempDir dir;
    const fs::path target = dir / "main.lisp";
    dir.write("main.lisp", "old");
    ::chmod(target.c_str(), 0640);

    REQUIRE_FALSE(fileStore::writeAtomic(target, "new"));
    CHECK(support::unwrap(fileStore::read(target).text) == "new");
    CHECK((fs::status(target).permissions() & fs::perms::all) ==
          (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read));

    // No temporary files are left behind.
    int entries = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator(dir.path())) ++entries;
    CHECK(entries == 1);
}

TEST_CASE("a failed save leaves the original unchanged", "[tui][fileStore]") {
    if (::geteuid() == 0) SKIP("permission checks do not apply to root");
    support::tempDir dir;
    const fs::path target = dir / "main.lisp";
    dir.write("main.lisp", "original");
    fs::permissions(dir.path(), fs::perms::owner_read | fs::perms::owner_exec);

    const auto error = fileStore::writeAtomic(target, "replacement");
    CHECK(error);
    CHECK(support::unwrap(fileStore::read(target).text) == "original");
}

TEST_CASE("a new file gets the default permissions and a failed rename leaves no temporary file",
          "[tui][fileStore]") {
    support::tempDir dir;
    const auto mask = ::umask(0);
    ::umask(mask);
    REQUIRE_FALSE(fileStore::writeAtomic(dir / "new.lisp", "x"));
    const auto readWrite = fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                           fs::perms::group_write | fs::perms::others_read | fs::perms::others_write;
    CHECK((fs::status(dir / "new.lisp").permissions() & fs::perms::all) ==
          (readWrite & ~static_cast<fs::perms>(mask)));

    fs::create_directory(dir / "occupied");  // a file cannot replace a directory
    CHECK(fileStore::writeAtomic(dir / "occupied", "x"));
    int entries = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator(dir.path())) ++entries;
    CHECK(entries == 2);  // new.lisp and occupied
}

TEST_CASE("fileStore lists project files", "[tui][fileStore]") {
    support::tempDir dir;
    dir.write("main.lisp", "");
    dir.write("util/scales.lisp", "");
    dir.write(".git/HEAD", "");
    dir.write("build/out", "");
    dir.write(".hidden", "");

    const auto files = fileStore::listProject(dir.path());
    REQUIRE(files.size() == 2);
    CHECK(files[0] == fs::path("main.lisp"));
    CHECK(files[1] == fs::path("util/scales.lisp"));
}
