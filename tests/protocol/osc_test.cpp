#include <catch2/catch_test_macros.hpp>

#include <proj/protocol/crc32.h>
#include <proj/protocol/irTransfer.h>
#include <proj/protocol/osc.h>

#include <string>
#include <vector>

using namespace protocol;

TEST_CASE("OSC messages round-trip every argument type (protocol.md 2.2)", "[protocol][osc]") {
    const message m{"/ir.begin",
                    {std::int32_t{-7}, std::int64_t{1} << 40, 0.25f, 3.5, std::string("abcd"),
                     blob{{1, 2, 3, 4, 5}}}};
    const auto bytes = encode(m);
    CHECK(bytes.size() % 4 == 0);
    const auto back = decode(bytes);
    REQUIRE(back);
    CHECK(*back == m);
    CHECK(back->tags() == "ihfdsb");
    CHECK(back->int32(0) == -7);
    CHECK(back->int64(1) == (std::int64_t{1} << 40));
    CHECK(back->float32(2) == 0.25f);
    CHECK(back->string(4) == "abcd");
    CHECK(back->bytes(5)->bytes.size() == 5);
    CHECK_FALSE(back->int32(4));
    CHECK_FALSE(back->int32(9));
}

TEST_CASE("OSC strings and blobs pad to 4 bytes", "[protocol][osc]") {
    CHECK(encode({"/a", {}}).size() == 4 + 4);                         // "/a\0\0" ",\0\0\0"
    CHECK(encode({"/abc", {}}).size() == 8 + 4);                       // "/abc\0" pads to 8
    CHECK(encode({"/a", {std::string("xyz")}}).size() == 4 + 4 + 4);  // "xyz\0"
    CHECK(encode({"/a", {blob{{1}}}}).size() == 4 + 4 + 4 + 4);       // size + 1 byte padded
    const auto bytes = encode({"/a", {std::int32_t{0x01020304}}});
    CHECK(std::vector<std::uint8_t>(bytes.end() - 4, bytes.end()) ==
          std::vector<std::uint8_t>{1, 2, 3, 4});
}

TEST_CASE("malformed packets do not decode (protocol.md 2.4)", "[protocol][osc]") {
    auto bytes = encode({"/play", {std::int32_t{1}, std::string("none")}});
    CHECK(decode(bytes));
    CHECK_FALSE(decode(std::vector<std::uint8_t>(bytes.begin(), bytes.end() - 4)));  // truncated
    CHECK_FALSE(decode(std::vector<std::uint8_t>(bytes.begin(), bytes.end() - 1)));  // misaligned
    CHECK_FALSE(decode(encode({"#bundle", {}})));
    CHECK_FALSE(decode(std::vector<std::uint8_t>{}));

    auto badTag = encode({"/a", {std::int32_t{1}}});
    badTag[5] = 'x';  // type tag string is ",i\0\0" at offset 4
    CHECK_FALSE(decode(badTag));

    auto badPadding = encode({"/abc", {}});
    badPadding[5] = 'z';  // padding after "/abc\0" must be zero
    CHECK_FALSE(decode(badPadding));

    auto extra = encode({"/a", {}});
    extra.insert(extra.end(), {0, 0, 0, 0});
    CHECK_FALSE(decode(extra));
}

TEST_CASE("describe renders messages", "[protocol][osc]") {
    CHECK(describe({"/play", {std::int32_t{3}, 0.5f, std::string("bar"), blob{{1, 2}}}}) ==
          "/play 3 0.5 \"bar\" <blob 2 bytes>");
}

TEST_CASE("crc32 matches the IEEE check value (protocol.md 6.5)", "[protocol][crc32]") {
    CHECK(crc32("123456789") == 0xCBF43926u);
    CHECK(crc32("") == 0u);
    const std::string text = "the quick brown fox";
    const auto whole = crc32(text);
    const auto part = crc32(std::string_view(text).substr(0, 7));
    CHECK(crc32(std::span(reinterpret_cast<const std::uint8_t*>(text.data()) + 7, text.size() - 7),
                part) == whole);
}

TEST_CASE("IR splits into chunks of at most 32 KiB (protocol.md 6.1)", "[protocol][ir]") {
    const std::string payload(100'000, 'x');
    const auto chunks = splitChunks(payload);
    REQUIRE(chunks.size() == 4);
    CHECK(chunks[0].size() == maxChunkBytes);
    CHECK(chunks[3].size() == 100'000 - 3 * maxChunkBytes);
    CHECK(splitChunks("").empty());
}

TEST_CASE("IR assembler reports missing chunks and checks the CRC (protocol.md 6.3)",
          "[protocol][ir]") {
    std::string payload;
    for (int i = 0; i < 70'000; ++i) payload.push_back(static_cast<char>('a' + i % 26));
    const auto chunks = splitChunks(payload);
    auto bytesOf = [](std::string_view s) {
        return std::span(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    };

    irAssembler a;
    REQUIRE(a.begin(9, static_cast<std::int64_t>(payload.size()), 3, crc32(payload)));
    CHECK(a.add(9, 0, bytesOf(chunks[0])));
    CHECK(a.add(9, 2, bytesOf(chunks[2])));
    CHECK_FALSE(a.add(9, 3, bytesOf(chunks[2])));  // out of range
    CHECK_FALSE(a.add(8, 0, bytesOf(chunks[0])));  // unknown plan

    auto r = a.finish(9);
    CHECK(r.what == irAssembler::result::kind::missing);
    CHECK(r.missing == std::vector<std::int32_t>{1});
    CHECK(a.pending() == 1);

    a.add(9, 1, bytesOf(chunks[1]));
    r = a.finish(9);
    REQUIRE(r.what == irAssembler::result::kind::complete);
    CHECK(r.payload == payload);
    CHECK(a.pending() == 0);
    CHECK(a.finish(9).what == irAssembler::result::kind::unknownPlan);

    REQUIRE(a.begin(10, 5, 1, crc32("hello") ^ 1u));
    a.add(10, 0, bytesOf("hello"));
    CHECK(a.finish(10).what == irAssembler::result::kind::checksumMismatch);

    CHECK_FALSE(a.begin(11, 100'000, 1, 0));  // too few chunks for the size
    CHECK_FALSE(a.begin(11, -1, 0, 0));
}
