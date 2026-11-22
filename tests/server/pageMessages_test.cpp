// Pages' messages as JSON and OSC, and HTTP requests (io.md §12.5, §12.11).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <espresso/protocol/osc.h>

#include "pageMessages.h"
#include "support/unwrap.h"

using Catch::Matchers::ContainsSubstring;
using namespace server;

namespace {

protocol::message decoded(const oscBytes& b) { return support::unwrap(protocol::decode(b)); }

std::string refusal(const std::string& body) {
    std::string error;
    CHECK_FALSE(messagesFromPage(body, error));
    return error;
}

oscBytes encoded(const protocol::message& m) { return protocol::encode(m); }

}  // namespace

TEST_CASE("a page's messages become OSC: numbers floats strings strings and booleans 1 and 0 (io.md 12.5)") {
    std::string error;
    const auto batch = support::unwrap(messagesFromPage(
        R"([["/cutoff", 0.4], ["/note", 60, 100], ["/name", "pad", true, false], ["/bang"]])", error));
    REQUIRE(batch.size() == 4);
    CHECK(decoded(batch[0]) == protocol::message{.address = "/cutoff", .args = {0.4f}});
    CHECK(decoded(batch[1]) == protocol::message{.address = "/note", .args = {60.0f, 100.0f}});
    CHECK(decoded(batch[2]) ==
          protocol::message{.address = "/name",
                            .args = {std::string("pad"), std::int32_t{1}, std::int32_t{0}}});
    CHECK(decoded(batch[3]).args.empty());
    CHECK(support::unwrap(messagesFromPage("[]", error)).empty());
}

TEST_CASE("a page's batch with any message that breaks a rule is refused whole") {
    CHECK_THAT(refusal("not json"), ContainsSubstring("not a JSON array of messages"));
    CHECK_THAT(refusal(R"({"a": 1})"), ContainsSubstring("not a JSON array of messages"));
    CHECK_THAT(refusal(R"([["/ok", 1], "/loose"])"), ContainsSubstring("an array of an address"));
    CHECK_THAT(refusal(R"([[]])"), ContainsSubstring("an array of an address"));
    CHECK_THAT(refusal(R"([[1, 2]])"), ContainsSubstring("an array of an address"));
    CHECK_THAT(refusal(R"([["cutoff", 1]])"), ContainsSubstring("does not start with /"));
    CHECK_THAT(refusal(R"([["/a", null]])"),
               ContainsSubstring("argument 1 of /a is not a number, string, or boolean"));
    CHECK_THAT(refusal(R"([["/a", [1]]])"), ContainsSubstring("not a number, string, or boolean"));
    CHECK_THAT(refusal(R"([["/a", 1e300]])"), ContainsSubstring("does not fit a float"));
    CHECK_THAT(refusal(R"([["/a", ")" + std::string(1100, 'x') + R"("]])"),
               ContainsSubstring("larger than 1024 bytes"));
}

TEST_CASE("messages to a page become JSON arrays with numbers strings booleans and nulls (io.md 12.5)") {
    CHECK(messageToPage(encoded({.address = "/level", .args = {0.25f}})) == R"(["/level",0.25])");
    CHECK(messageToPage(encoded({.address = "/mix",
                                 .args = {std::int32_t{3}, std::int64_t{1099511627776}, 0.5,
                                          std::string("x")}})) == R"(["/mix",3,1099511627776,0.5,"x"])");
    CHECK(messageToPage(
              encoded({.address = "/blob", .args = {protocol::blob{.bytes = {1, 2, 3}}, std::int32_t{7}}})) ==
          R"(["/blob",null,7])");
    CHECK(messageToPage(encoded({.address = "/nan", .args = {std::numeric_limits<float>::quiet_NaN()}})) ==
          R"(["/nan",null])");
    // T, F, N, and I carry no bytes.
    oscBytes tf = {'/', 't', 0, 0, ',', 'T', 'F', 'N', 'I', 'i', 0, 0, 0, 0, 0, 9};
    CHECK(messageToPage(tf) == R"(["/t",true,false,null,null,9])");
    CHECK(
        messageToPage(encoded({.address = "/other",
                               .args = {protocol::character{'A'}, protocol::rgba{1}, protocol::midiMessage{},
                                        protocol::timeTag{1}, protocol::symbol{"s"}, std::int32_t{2}}})) ==
        R"(["/other",null,null,null,null,null,2])");
    // A sender that leaves out the type tags (protocol.md 2.5).
    CHECK(messageToPage(oscBytes{'/', 't', 0, 0}) == R"(["/t"])");
    oscBytes unknown = {'/', 'u', 0, 0, ',', 'Q', 0, 0};
    CHECK_FALSE(messageToPage(unknown));
    oscBytes truncated = {'/', 'u', 0, 0, ',', 'i', 0, 0, 0, 1};
    CHECK_FALSE(messageToPage(truncated));
    CHECK_FALSE(messageToPage(oscBytes{'x', 0, 0, 0}));
}

TEST_CASE("a binary frame from a page is an OSC message of at most 1024 bytes (io.md 12.6)") {
    const auto m = encoded({.address = "/x", .args = {1.0f}});
    CHECK(binaryFromPage(m) == m);
    CHECK_FALSE(binaryFromPage(oscBytes{}));
    CHECK_FALSE(binaryFromPage(oscBytes{'x', 0, 0, 0}));
    CHECK_FALSE(binaryFromPage(oscBytes{'/', 'n', 'o'}));  // no end to the address
    CHECK_FALSE(binaryFromPage(oscBytes(1025, '/')));
}

TEST_CASE("requests are read one after another with their headers and bodies (io.md 12.11)") {
    requestReader r;
    r.feed("GET /osc?format=binary&x HTTP/1.1\r\nHost: localhost:8000\r\nUpgrade:  websocket \r\n\r\nPOST "
           "/osc HTTP/1.1\r\n");
    auto first = support::unwrap(r.next());
    CHECK(first.method == "GET");
    CHECK(first.path() == "/osc");
    CHECK(first.query("format") == "binary");
    CHECK(first.query("x") == "");
    CHECK_FALSE(first.query("y"));
    CHECK(first.header("host") == "localhost:8000");
    CHECK(first.header("upgrade") == "websocket");
    CHECK_FALSE(r.next());  // the second has not arrived whole
    r.feed("Content-Length: 11\r\n\r\n[[\"/a\",1]]");
    CHECK_FALSE(r.next());
    r.feed("]extra");
    const auto second = support::unwrap(r.next());
    CHECK(second.body == "[[\"/a\",1]]]");
    CHECK(r.takeRest() == "extra");
}

TEST_CASE("requests that cannot be read get their status") {
    const auto error = [](const std::string& bytes) {
        requestReader r;
        r.feed(bytes);
        CHECK_FALSE(r.next());
        return r.error();
    };
    CHECK(error("GET / HTTP/1.0\r\n\r\n") == requestError::badRequest);
    CHECK(error("nonsense\r\n\r\n") == requestError::badRequest);
    CHECK(error("GET http://x/ HTTP/1.1\r\n\r\n") == requestError::badRequest);
    CHECK(error("GET / HTTP/1.1\r\nno colon\r\n\r\n") == requestError::badRequest);
    CHECK(error("POST /osc HTTP/1.1\r\nContent-Length: x1\r\n\r\n") == requestError::badRequest);
    CHECK(error("POST /osc HTTP/1.1\r\n\r\n") == requestError::lengthRequired);
    CHECK(error("POST /osc HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") == requestError::lengthRequired);
    CHECK(error("POST /osc HTTP/1.1\r\nContent-Length: 65537\r\n\r\n") == requestError::tooLarge);
    CHECK(error("GET / HTTP/1.1\r\nX: " + std::string(17000, 'a')) == requestError::headersTooLarge);
    requestReader ok;
    ok.feed("POST /osc HTTP/1.1\r\nContent-Length: 65536\r\n\r\n" + std::string(65536, ' '));
    CHECK(ok.next());
}
