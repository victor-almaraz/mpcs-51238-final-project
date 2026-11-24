#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

TEST_CASE("fileStore lists links by their own names and descends into linked directories",
          "[tui][fileStore]") {
    support::tempDir dir;
    dir.write("examples/phasing/loops.lisp", "");
    dir.write("samples/kick.wav", "");
    dir.write("project/main.lisp", "");
    dir.write("project/zz/last.lisp", "");  // listed after the dangling link
    const fs::path root = dir / "project";
    fs::create_symlink("../examples/phasing/loops.lisp", root / "loops.lisp");
    fs::create_directory_symlink(dir / "samples", root / "samples");
    fs::create_directory_symlink(root, root / "zz/self");               // a cycle through the root
    fs::create_directory_symlink("..", root / "up");                    // a cycle through the root's parent
    fs::create_symlink(root / "missing.lisp", root / "dangling.lisp");  // skipped, not fatal

    const auto files = fileStore::listProject(root);
    for (const fs::path f :
         {"loops.lisp", "main.lisp", "samples/kick.wav", "zz/last.lisp", "up/samples/kick.wav"})
        CHECK(std::ranges::find(files, f) != files.end());
    CHECK(std::ranges::find(files, fs::path("dangling.lisp")) == files.end());
    CHECK(std::ranges::find(files, fs::path("zz/self/main.lisp")) == files.end());
    CHECK(std::ranges::find(files, fs::path("up/project/main.lisp")) == files.end());
    CHECK(std::ranges::is_sorted(files));
    for (const auto& f : files) CHECK(*f.begin() != "..");
}

TEST_CASE("saving through a link replaces the file it names and keeps the link", "[tui][fileStore]") {
    support::tempDir dir;
    dir.write("examples/loops.lisp", "old");
    fs::create_directories(dir / "project");
    fs::create_symlink("../examples/loops.lisp", dir / "project/loops.lisp");
    fs::create_symlink("loops.lisp", dir / "project/again.lisp");  // a chain of two links

    REQUIRE_FALSE(fileStore::writeAtomic(dir / "project/again.lisp", "new"));
    CHECK(fs::is_symlink(dir / "project/loops.lisp"));
    CHECK(fs::is_symlink(dir / "project/again.lisp"));
    CHECK(support::unwrap(fileStore::read(dir / "examples/loops.lisp").text) == "new");

    // A dangling link's target is created.
    fs::create_symlink("../examples/new.lisp", dir / "project/new.lisp");
    REQUIRE_FALSE(fileStore::writeAtomic(dir / "project/new.lisp", "made"));
    CHECK(fs::is_symlink(dir / "project/new.lisp"));
    CHECK(support::unwrap(fileStore::read(dir / "examples/new.lisp").text) == "made");
}
