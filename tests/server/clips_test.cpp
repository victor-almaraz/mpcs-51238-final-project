// A project's clips (canvas.md §5.3): the files of each bank's folder, read
// into the engine's banks; saved and listed by pages on the page port.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <espresso/primitives/clip.h>

#include "clips.h"
#include "pageBridge.h"
#include "pageMessages.h"
#include "pagePlan.h"
#include "support/sockets.h"
#include "support/tempDir.h"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using namespace server;

namespace {

// A MIDI file of one note, @p note, a quarter note long.
std::vector<std::uint8_t> oneNote(std::uint8_t note = 60) {
    return {'M', 'T',  'h',  'd', 0, 0, 0, 6,  0, 0,    0,    1,   0,  96,             // format 0, 96 ticks
            'M', 'T',  'r',  'k', 0, 0, 0, 12, 0, 0x90, note, 100, 96, 0x80, note, 0,  // the note
            0,   0xFF, 0x2F, 0};                                                       // the end
}

std::string asText(const std::vector<std::uint8_t>& b) { return {b.begin(), b.end()}; }

void write(const std::filesystem::path& file, const std::vector<std::uint8_t>& b) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << asText(b);
}

// The clip a bank's slot holds once its node takes what it was given.
std::optional<int> firstNote(primitives::clipBank& bank, int slot) {
    bank.update();
    const auto c = bank.at(slot);
    if (!c || c->get().notes.empty()) return std::nullopt;
    return c->get().notes[0].note;
}

}  // namespace

TEST_CASE("a clip file's name gives its slot, from 1, and its title", "[clips]") {
    CHECK(clipSlotOf("1.mid") == 0);
    CHECK(clipSlotOf("3 walking bass.mid") == 2);
    CHECK(clipSlotOf("12-fill.mid") == 11);
    CHECK_FALSE(clipSlotOf("0.mid"));
    CHECK_FALSE(clipSlotOf("65.mid"));
    CHECK_FALSE(clipSlotOf("3x.mid"));
    CHECK_FALSE(clipSlotOf("groove.mid"));
    CHECK_FALSE(clipSlotOf("3.wav"));
    CHECK(clipTitle("clips/bass/3 walking bass.mid") == "walking bass");
    CHECK(clipTitle("clips/bass/2.mid").empty());
    CHECK(isClipBankName("lead-2"));
    CHECK_FALSE(isClipBankName("Bass"));
    CHECK_FALSE(isClipBankName("../ui"));
}

TEST_CASE("a saved clip replaces its slot's files and is listed by slot", "[clips]") {
    const support::tempDir root;
    write(root / "clips/bass/2 old.mid", oneNote());
    write(root / "clips/bass/2-older.mid", oneNote());
    REQUIRE_FALSE(saveClip(root.path(), "bass", 1, "Walk/ing.mid", oneNote(48)));
    const auto files = clipFilesOf(root.path(), "bass");
    REQUIRE(files.size() == 1);
    CHECK(files.at(1).filename() == "2 Walking.mid");
    CHECK_FALSE(std::filesystem::exists(root / "clips/bass/2 old.mid"));
    // What is not a MIDI file, or not a bank or slot, is refused and saves nothing.
    CHECK_THAT(saveClip(root.path(), "bass", 0, "x", std::vector<std::uint8_t>{'n', 'o'}).value_or(""),
               ContainsSubstring("not a MIDI file"));
    CHECK(saveClip(root.path(), "../ui", 0, "x", oneNote()));
    CHECK(saveClip(root.path(), "bass", primitives::maxClipSlots, "x", oneNote()));
    CHECK(clipFilesOf(root.path(), "bass").size() == 1);
}

TEST_CASE("the engine's banks take the files of their folders that changed", "[clips]") {
    const support::tempDir root;
    write(root / "clips/bass/1 a.mid", oneNote(40));
    write(root / "clips/bass/3 bad.mid", {'n', 'o', 't'});
    primitives::clipBanks banks;
    const auto bass = banks.bank("bass");
    clipFiles files(root.path());
    const auto errors = files.sync(banks);
    REQUIRE(errors.size() == 1);
    CHECK_THAT(errors[0], ContainsSubstring("3 bad.mid"));
    CHECK(firstNote(*bass, 0) == 40);
    CHECK_FALSE(firstNote(*bass, 2));
    // Nothing changed: nothing is given again, and the bad file is not
    // reported again.
    CHECK(files.sync(banks).empty());
    // A file that did not read for want of permission reads once it may:
    // its stamp is as it was.
    write(root / "clips/bass/4 locked.mid", oneNote(44));
    std::filesystem::permissions(root / "clips/bass/4 locked.mid", std::filesystem::perms::none);
    const auto locked = files.sync(banks);
    REQUIRE(locked.size() == 1);
    CHECK_THAT(locked[0], StartsWith("clips/bass/4 locked.mid: "));  // from the project's root
    CHECK(files.sync(banks).empty());                                // reported once
    std::filesystem::permissions(root / "clips/bass/4 locked.mid", std::filesystem::perms::owner_read);
    CHECK(files.sync(banks).empty());
    CHECK(firstNote(*bass, 3) == 44);
    // A file replaced, and one removed.
    std::filesystem::remove(root / "clips/bass/1 a.mid");
    REQUIRE_FALSE(saveClip(root.path(), "bass", 1, "b", oneNote(41)));
    CHECK(files.sync(banks).empty());
    CHECK_FALSE(firstNote(*bass, 0));
    CHECK(firstNote(*bass, 1) == 41);
    banks.collect();
}

TEST_CASE("pages list a bank's clips and save one with POST /clips/<bank>/<slot> (canvas.md 5.3)",
          "[clips]") {
    const support::tempDir root;
    std::filesystem::create_directories(root / "ui");
    pageBridge bridge({.root = root.path(), .helper = {}, .controls = {}, .report = {}});
    bridge.listen(0);
    const auto port = bridge.port();
    REQUIRE(port != 0);
    bridge.follow(std::make_shared<const pagePlan>(pagePlan{.addresses = {}, .banks = {"bass"}}));
    const std::string host = "localhost:" + std::to_string(port);
    const auto request = [&](const std::string& method, const std::string& path, const std::string& body,
                             bool own = true) {
        support::httpClient c(port);
        c.write(method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\n" +
                (own ? "Origin: http://" + host + "\r\n" : "") +
                "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
        return c.response();
    };
    CHECK_THAT(request("GET", "/clips/bass", ""), ContainsSubstring("\r\n\r\n{}"));
    CHECK_FALSE(bridge.takeClipsChanged());
    // An open page hears of the save.
    support::httpClient stream(port);
    stream.write("GET /osc HTTP/1.1\r\nHost: " + host + "\r\n\r\n");
    stream.readUntil([](const std::string& b) { return b.find("\r\n\r\n") != std::string::npos; });
    CHECK_THAT(request("POST", "/clips/bass/2?name=walking%20bass", asText(oneNote())),
               StartsWith("HTTP/1.1 204 "));
    CHECK(bridge.takeClipsChanged());
    CHECK_FALSE(bridge.takeClipsChanged());
    CHECK_THAT(
        stream.readUntil([](const std::string& b) { return b.find("/espresso/clips") != std::string::npos; }),
        ContainsSubstring(R"(data: ["/espresso/clips","bass"])"));
    CHECK(std::filesystem::exists(root / "clips/bass/2 walking bass.mid"));
    CHECK_THAT(request("GET", "/clips/bass", ""), ContainsSubstring(R"({"2":"walking bass"})"));
    // Refusals: another origin, a file that is not MIDI, no such slot or bank, the wrong method.
    CHECK_THAT(request("POST", "/clips/bass/1", asText(oneNote()), false), StartsWith("HTTP/1.1 403 "));
    const auto bad = request("POST", "/clips/bass/1", "not midi");
    CHECK_THAT(bad, StartsWith("HTTP/1.1 400 "));
    CHECK_THAT(bad, ContainsSubstring("not a MIDI file"));
    CHECK_THAT(request("POST", "/clips/bass/99", asText(oneNote())), StartsWith("HTTP/1.1 400 "));
    CHECK_THAT(request("GET", "/clips/Bass", ""), StartsWith("HTTP/1.1 404 "));
    // A bank the piece playing does not play: no folder is made for it.
    CHECK_THAT(request("POST", "/clips/pad/1", asText(oneNote())), StartsWith("HTTP/1.1 404 "));
    CHECK_FALSE(std::filesystem::exists(root / "clips/pad"));
    CHECK_THAT(request("GET", "/clips/bass/2", ""), StartsWith("HTTP/1.1 405 "));
    CHECK_FALSE(bridge.takeClipsChanged());
}

TEST_CASE("a clip too large is refused with 413 after its body is read; one of 1 MiB is not", "[clips]") {
    const support::tempDir root;
    pageBridge bridge({.root = root.path(), .helper = {}, .controls = {}, .report = {}});
    bridge.listen(0);
    const auto port = bridge.port();
    REQUIRE(port != 0);
    bridge.follow(std::make_shared<const pagePlan>(pagePlan{.addresses = {}, .banks = {"bass"}}));
    const std::string host = "localhost:" + std::to_string(port);
    const auto post = [&](std::size_t size) {
        support::httpClient c(port);
        // The body in pieces, as a browser sends it, after the head.
        c.write("POST /clips/bass/1 HTTP/1.1\r\nHost: " + host + "\r\nOrigin: http://" + host +
                "\r\nContent-Length: " + std::to_string(size) + "\r\n\r\n");
        const std::string piece(std::size_t{64} << 10U, 'x');
        for (std::size_t sent = 0; sent < size; sent += piece.size())
            c.write(piece.substr(0, std::min(piece.size(), size - sent)));
        return c.response();
    };
    // 1 MiB passes the limit, and is refused for what it holds.
    CHECK_THAT(post(maxClipBody), StartsWith("HTTP/1.1 400 "));
    const auto large = post(maxClipBody + 1);
    CHECK_THAT(large, StartsWith("HTTP/1.1 413 "));
    CHECK_THAT(large, ContainsSubstring("1 MiB for a clip"));
}

TEST_CASE("a saved clip takes its slot's place at once, the slot never empty", "[clips]") {
    const support::tempDir root;
    write(root / "clips/bass/1 old.mid", oneNote(40));
    REQUIRE_FALSE(saveClip(root.path(), "bass", 0, "new", oneNote(41)));
    const auto files = clipFilesOf(root.path(), "bass");
    REQUIRE(files.size() == 1);
    CHECK(files.at(0).filename() == "1 new.mid");
    // A title that differs only in case, on a file system that ignores
    // case as well as on one that does not: the slot keeps one file, in the
    // new spelling.
    REQUIRE_FALSE(saveClip(root.path(), "bass", 0, "New", oneNote(42)));
    const auto again = clipFilesOf(root.path(), "bass");
    REQUIRE(again.size() == 1);
    CHECK(again.at(0).filename() == "1 New.mid");
    // An error names the file from the project's root.
    std::filesystem::permissions(root / "clips/bass",
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    const auto error = saveClip(root.path(), "bass", 1, "x", oneNote());
    std::filesystem::permissions(root / "clips/bass", std::filesystem::perms::owner_all);
    REQUIRE(error);
    CHECK_THAT(error.value_or(""), StartsWith("cannot write clips/bass/2 x.mid"));
}

TEST_CASE("a bank that takes no changes back waits, unread, until it does", "[clips]") {
    const support::tempDir root;
    primitives::clipBanks banks;
    const auto bass = banks.bank("bass");
    // A bank whose node does not play: changes fill its queue.
    for (std::size_t i = 0; i < primitives::clipBank::queueSize; ++i)
        REQUIRE(bass->offer(0, std::make_unique<const primitives::clip>()));
    write(root / "clips/bass/1 a.mid", oneNote(40));
    clipFiles files(root.path());
    CHECK(files.sync(banks).empty());
    CHECK_FALSE(files.retryDue(banks));  // no retry while it cannot take one
    // Its node takes them, and gives them back.
    bass->update();
    banks.collect();
    CHECK(files.retryDue(banks));
    CHECK(files.sync(banks).empty());
    CHECK_FALSE(files.retryDue(banks));
    CHECK(firstNote(*bass, 0) == 40);
    banks.collect();
}
