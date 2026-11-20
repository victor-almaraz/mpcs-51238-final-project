// JSON fields, server-sent events, and reconnection delays (io.md §6.2,
// §6.3, §8, §14.1).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "webData.h"

using namespace server;
using std::chrono::milliseconds;
using strings = std::vector<std::string>;

namespace {

// The values of @p e, with -999 for each missing one, so a comparison of
// lists shows which are missing.
std::vector<float> read(const extraction& e) {
    std::vector<float> out;
    out.reserve(e.frame.count);
    for (std::size_t i = 0; i < e.frame.count; ++i)
        out.push_back(e.frame.has(i) ? e.frame.values[i] : -999.0f);
    return out;
}

}  // namespace

TEST_CASE("fields are JSON Pointers into the document (io.md 8.1)") {
    const std::string doc = R"({"main": {"temp": 21.5, "humidity": 60}, "wind": {"speed": 3},
                               "data": [{"value": 7}, {"value": 8}], "a/b": 1, "m~n": 2})";
    const auto e =
        extractFields(doc, strings{"/main/temp", "/wind/speed", "/data/1/value", "/a~1b", "/m~0n"});
    REQUIRE(e.kind == documentKind::read);
    CHECK(read(e) == std::vector<float>{21.5f, 3.0f, 8.0f, 1.0f, 2.0f});
    CHECK(e.missing == 0);
    // No fields reads the whole document: an endpoint that returns a number.
    CHECK(read(extractFields("42.5", {})) == std::vector<float>{42.5f});
    CHECK(read(extractFields(doc, strings{""})) == std::vector<float>{-999.0f});  // an object is no number
}

TEST_CASE("booleans and numeric strings convert and anything else is missing (io.md 8.2)") {
    const std::string doc = R"({"on": true, "off": false, "s": "0.25", "word": "high", "n": null,
                               "list": [1], "huge": 1e300, "padded": " 3"})";
    const auto e =
        extractFields(doc, strings{"/on", "/off", "/s", "/word", "/n", "/list", "/huge", "/padded"});
    CHECK(read(e) == std::vector<float>{1.0f, 0.0f, 0.25f, -999.0f, -999.0f, -999.0f, -999.0f, -999.0f});
    CHECK(e.missing == 5);
    // Absent paths and paths that are not pointers are missing too.
    const auto absent = extractFields(doc, strings{"/nothere", "/list/5", "/list/x", "on", "/on"});
    CHECK(read(absent) == std::vector<float>{-999.0f, -999.0f, -999.0f, -999.0f, 1.0f});
    CHECK(absent.missing == 4);
}

TEST_CASE("documents that are not JSON or larger than 1 MiB are refused (io.md 8.3)") {
    CHECK(extractFields("{\"a\": ", strings{"/a"}).kind == documentKind::notJson);
    CHECK(extractFields("", {}).kind == documentKind::notJson);
    std::string big = "[" + std::string(maxDocumentBytes, ' ') + "1]";
    CHECK(extractFields(big, {}).kind == documentKind::tooLarge);
    big.resize(maxDocumentBytes);  // at the limit: read
    big.back() = ']';
    CHECK(extractFields(big, strings{"/0"}).kind == documentKind::read);
}

namespace {

std::vector<sseMessage> parse(sseParser& p, std::string_view bytes) {
    std::vector<sseMessage> out;
    p.feed(bytes, [&](const sseMessage& m) { out.push_back(m); });
    return out;
}

}  // namespace

TEST_CASE(
    "a server-sent event's data lines join with line feeds and a blank line dispatches it (io.md 6.2)") {
    sseParser p;
    auto m = parse(p, ": a comment\ndata: {\"a\":\ndata:1}\n\n");
    REQUIRE(m.size() == 1);
    CHECK(m[0].event == "message");
    CHECK(m[0].data == "{\"a\":\n1}");
    // Lines end with CR, LF, or CRLF, in any pieces.
    m = parse(p, "event: edit\r\ndata: 1\r");
    CHECK(m.empty());
    m = parse(p, "\r\ndata:2\n\n");
    REQUIRE(m.size() == 2);
    CHECK(m[0].event == "edit");
    CHECK(m[0].data == "1");
    CHECK(m[1].event == "message");  // the name ends with its message
    CHECK(m[1].data == "2");
    // A message without data is not dispatched; one leading space is stripped.
    CHECK(parse(p, "event: ping\n\ndata:  x\n\n").at(0).data == " x");
}

TEST_CASE("a server-sent event stream's id and retry carry across messages and reconnections (io.md 6.3)") {
    sseParser p;
    auto m = parse(p, "\xEF\xBB\xBFid: 41\nretry: 5000\ndata: a\n\n");
    REQUIRE(m.size() == 1);
    CHECK(m[0].lastEventId == "41");
    CHECK(p.retry() == milliseconds(5000));
    CHECK(parse(p, "retry: 5s\nretry: \ndata: b\n\n").at(0).lastEventId == "41");
    CHECK(p.retry() == milliseconds(5000));  // a retry that is not digits is ignored
    parse(p, "id: 42\ndata: half");
    p.reset();  // the connection dropped mid-message
    m = parse(p, "\n\ndata: c\n\n");
    REQUIRE(m.size() == 1);
    CHECK(m[0].data == "c");
    CHECK(p.lastEventId() == "42");
}

TEST_CASE("a server-sent event larger than 1 MiB is dropped whole and counted") {
    sseParser p;
    const std::string chunk(std::size_t{1} << 16U, 'x');
    std::vector<sseMessage> m;
    for (std::size_t sent = 0; sent <= maxDocumentBytes; sent += chunk.size()) {
        const auto got = parse(p, "data: " + chunk + "\n");
        m.insert(m.end(), got.begin(), got.end());
    }
    const auto after = parse(p, "\ndata: next\n\n");
    m.insert(m.end(), after.begin(), after.end());
    REQUIRE(m.size() == 1);
    CHECK(m[0].data == "next");
    CHECK(p.tooLarge() == 1);
    // One line past the limit too.
    sseParser q;
    CHECK(parse(q, "data: " + std::string(maxDocumentBytes, 'y') + "\n\ndata: ok\n\n").size() == 1);
    CHECK(q.tooLarge() == 1);
}

TEST_CASE("reconnection waits 3 s or the retry time and doubles to 60 s (io.md 6.3 and 6.6)") {
    CHECK(reconnectDelay(1, milliseconds(0)) == milliseconds(3000));
    CHECK(reconnectDelay(2, milliseconds(0)) == milliseconds(6000));
    CHECK(reconnectDelay(5, milliseconds(0)) == milliseconds(48000));
    CHECK(reconnectDelay(6, milliseconds(0)) == milliseconds(60000));
    CHECK(reconnectDelay(40, milliseconds(0)) == milliseconds(60000));
    CHECK(reconnectDelay(1, milliseconds(500)) == milliseconds(500));
    CHECK(reconnectDelay(3, milliseconds(500)) == milliseconds(2000));
    CHECK(reconnectDelay(9, milliseconds(500)) == milliseconds(60000));
    CHECK(reconnectDelay(4, milliseconds(90000)) == milliseconds(90000));  // the server asked for longer
}
