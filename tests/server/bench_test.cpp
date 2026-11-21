// Hidden benchmarks of the M6 parsers and formatters and of the pipe table, run with "[.bench]":
// each prints its wall time for the step 9 A/B benchmark (lessons.md
// "Data types"). The workloads are fixed, so two builds compare.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/io.h>
#include <espresso/primitives/pipeTable.h>
#include <espresso/primitives/primitive.h>
#include <espresso/protocol/osc.h>

#include "../../src/apps/server/pageMessages.h"
#include "../../src/apps/server/serialLines.h"
#include "../../src/apps/server/webData.h"
#include "../../src/apps/server/webSocket.h"

namespace {

// Times @p work and prints it as `bench NAME SECONDS`.
template <typename Work>
void bench(std::string_view name, Work&& work) {
    const auto start = std::chrono::steady_clock::now();
    work();
    const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
    std::cout << "bench " << name << " " << took.count() << "\n";
}

}  // namespace

TEST_CASE("bench: serial lines formatted and parsed", "[.bench]") {
    primitives::dataFrame frame;
    for (std::size_t i = 0; i < frame.values.size(); ++i)
        frame.values[frame.count++] = 0.1234567f * static_cast<float>(i + 1);
    std::size_t bytes = 0;
    bench("serial-format", [&] {
        for (int i = 0; i < 20000; ++i) {
            frame.values[0] = static_cast<float>(i) * 0.001f;
            bytes += server::formatLine(frame).size();
        }
    });
    std::string line = server::formatLine(frame);
    line.pop_back();  // the newline, which the splitter takes off
    std::size_t values = 0;
    bench("serial-parse", [&] {
        for (int i = 0; i < 50000; ++i) values += server::parseLine(line).frame.count;
    });
    CHECK(bytes > 0);
    CHECK(values == 50000 * frame.count);
}

TEST_CASE("bench: page messages to and from JSON", "[.bench]") {
    const auto osc = protocol::encode({.address = "/level", .args = {0.25f}});
    std::size_t text = 0;
    bench("page-to-json", [&] {
        for (int i = 0; i < 100000; ++i) text += server::messageToPage(osc)->size();
    });
    std::string error;
    std::size_t messages = 0;
    bench("page-from-json", [&] {
        for (int i = 0; i < 100000; ++i)
            messages += server::messagesFromPage(R"([["/cutoff", 0.5]])", error)->size();
    });
    CHECK(text > 0);
    CHECK(messages == 100000);
}

TEST_CASE("bench: server-sent events with large data", "[.bench]") {
    constexpr std::size_t size = std::size_t{256} * 1024;
    const std::string event = "data: " + std::string(size, 'x') + "\n\n";
    std::size_t seen = 0;
    bench("sse-large", [&] {
        server::sseParser parser;
        for (int i = 0; i < 200; ++i)
            parser.feed(event, [&](const server::sseMessage& m) { seen += m.data.size(); });
    });
    CHECK(seen == 200 * size);
}

TEST_CASE("bench: many small WebSocket frames in one read", "[.bench]") {
    std::string bytes;
    for (int i = 0; i < 2000; ++i)
        bytes += server::encodeFrame(server::opcode::text, R"({"length": 140})", true,
                                     std::array<std::uint8_t, 4>{1, 2, 3, 4});
    std::size_t seen = 0;
    bench("websocket-frames", [&] {
        for (int round = 0; round < 50; ++round) {
            server::frameReader reader{true, 1U << 20U};
            CHECK(reader.feed(bytes, [&](const server::frameMessage&) { ++seen; }) ==
                  server::frameError::none);
        }
    });
    CHECK(seen == 50 * 2000);
}

TEST_CASE("bench: default pipe tables built", "[.bench]") {
    const primitives::prepareInfo info{.sampleRate = 48000.0, .blockSize = 256};
    std::size_t audio = 0;
    bench("pipe-table", [&] {
        for (int i = 0; i < 20; ++i) {
            const primitives::pipeTable table({}, info);
            audio += table.contains({.kind = metadata::pipeKind::audio, .index = 0}) ? 1 : 0;
        }
    });
    CHECK(audio == 20);
}
