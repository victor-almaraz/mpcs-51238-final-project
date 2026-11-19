// Buffer declarations and buffer parameters in the IR (samples.md §5.1, §12.2).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>

#include "support/pipes.h"
#include "support/unwrap.h"
#include "support/validation.h"

using Catch::Matchers::ContainsSubstring;
using metadata::bufferDecl;
using metadata::bufferRef;
using metadata::graphSpec;
using metadata::nodeSpec;
using support::audio;
using support::errors;

namespace {

bufferDecl ring() {
    return {
        .buffer = {.index = 0},
        .id = "main.lisp/main/buffer@4:9#0",
        .channels = 2,
        .seconds = 20.0,
        .file = "samples/rain.wav",
        .src = metadata::sourceSpan{.file = "main.lisp", .line = 4, .column = 9, .offset = 51, .length = 40}};
}

// A sine to audio pipe 0, beside the buffers @p buffers.
graphSpec withBuffers(std::vector<bufferDecl> buffers) {
    graphSpec g;
    g.pipes = {{.pipe = audio(0), .channels = 1, .feedback = false}};
    g.buffers = std::move(buffers);
    nodeSpec n;
    n.id = "osc";
    n.type = "sineOsc";
    n.outputs["out"] = {audio(0)};
    g.nodes.push_back(std::move(n));
    return g;
}

}  // namespace

TEST_CASE("buffer declarations round trip through JSON") {
    bufferDecl silent{.buffer = {.index = 1},
                      .id = "main.lisp/main/buffer@5:9#0",
                      .channels = 1,
                      .seconds = 4.0,
                      .file = std::nullopt,
                      .src = std::nullopt};
    bufferDecl loaded{.buffer = {.index = 2},
                      .id = "main.lisp/main/buffer@6:9#0",
                      .channels = 1,
                      .seconds = std::nullopt,
                      .file = "kick.wav",
                      .src = std::nullopt};
    bufferDecl tables = loaded;
    tables.buffer.index = 3;
    tables.id = "main.lisp/main/buffer@7:9#0";
    tables.resample = false;
    const graphSpec g = withBuffers({ring(), silent, loaded, tables});
    const auto r = ir::read(ir::write(g, 2));
    INFO(r.text());
    REQUIRE(r.ok());
    CHECK(support::unwrap(r.graph) == g);
}

TEST_CASE("a buffer is written with resample only when it keeps its file's rate") {
    bufferDecl b = ring();
    CHECK(ir::write(withBuffers({b})).find("resample") == std::string::npos);
    b.resample = false;
    CHECK(ir::write(withBuffers({b}), 2).find(R"("resample": false)") != std::string::npos);
}

TEST_CASE("a graph without buffers writes no buffers member") {
    CHECK(ir::write(withBuffers({})).find("buffers") == std::string::npos);
}

TEST_CASE("a buffer parameter is written as an object naming the buffer") {
    graphSpec g = withBuffers({ring()});
    g.nodes[0].params["freq"] = bufferRef{.index = 0};
    const std::string text = ir::write(g);
    CHECK_THAT(text, ContainsSubstring(R"("freq":{"buffer":0})"));
    // It reads back as a buffer, which sineOsc's frequency does not take.
    CHECK_THAT(ir::read(text).text(), ContainsSubstring("node osc: parameter freq does not take a buffer"));
}

TEST_CASE("the schema requires a buffer's length or file") {
    const auto r = ir::read(R"({"format":"espresso-ir","version":1,"pipes":[],"nodes":[],
        "buffers":[{"buffer":0,"id":"b","channels":1,
                    "src":{"file":"main.lisp","line":4,"column":9}}]})");
    REQUIRE_FALSE(r.ok());
    CHECK_THAT(r.text(), ContainsSubstring("/buffers/0"));
    // The error points at the buffer form.
    CHECK(support::unwrap(r.errors.front().src).line == 4);
}

TEST_CASE("the schema refuses a buffer of no length and an unknown member") {
    CHECK_FALSE(ir::read(R"({"format":"espresso-ir","version":1,"pipes":[],"nodes":[],
        "buffers":[{"buffer":0,"id":"b","seconds":0}]})")
                    .ok());
    CHECK_FALSE(ir::read(R"({"format":"espresso-ir","version":1,"pipes":[],"nodes":[],
        "buffers":[{"buffer":0,"id":"b","seconds":1,"frames":48000}]})")
                    .ok());
}

TEST_CASE("validation checks each buffer declaration at its form") {
    bufferDecl b = ring();

    SECTION("a valid declaration") { CHECK(errors(withBuffers({b})).empty()); }
    SECTION("channels") {
        b.channels = 9;
        CHECK_THAT(errors(withBuffers({b})),
                   ContainsSubstring("main.lisp:4:9: buffer 0: channels must be 1 to 8"));
    }
    SECTION("length or file") {
        b.seconds.reset();
        b.file.reset();
        CHECK_THAT(errors(withBuffers({b})),
                   ContainsSubstring("buffer 0: needs a length in seconds, a file, or both"));
    }
    SECTION("a length that is not positive") {
        for (const double bad :
             {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
            b.seconds = bad;
            CHECK_THAT(errors(withBuffers({b})),
                       ContainsSubstring("buffer 0: seconds must be a positive number"));
        }
    }
    SECTION("resample without a file") {
        b.file.reset();
        b.resample = false;
        CHECK_THAT(errors(withBuffers({b})), ContainsSubstring("buffer 0: resample applies only to a file"));
    }
    SECTION("an empty file name") {
        b.file = "";
        CHECK_THAT(errors(withBuffers({b})), ContainsSubstring("buffer 0: the file name is empty"));
    }
    SECTION("a repeated index or ID") {
        bufferDecl other = b;
        other.id = "other";
        CHECK_THAT(errors(withBuffers({b, other})), ContainsSubstring("buffer 0: declared twice"));
        other = b;
        other.buffer.index = 1;
        CHECK_THAT(errors(withBuffers({b, other})), ContainsSubstring("buffer 1: duplicate buffer ID"));
    }
    SECTION("a negative index") {
        b.buffer.index = -1;
        CHECK_THAT(errors(withBuffers({b})), ContainsSubstring("buffer -1: negative index"));
    }
}

namespace {

// bufferWrite from audio pipe 0 into buffer 0, and bufferRead of it into
// audio pipe 1, in the order given.
graphSpec writeAndRead(bool readerFirst) {
    graphSpec g = withBuffers({ring()});
    g.pipes.push_back({.pipe = audio(1), .channels = 1, .feedback = false});
    nodeSpec w;
    w.id = "write";
    w.type = "bufferWrite";
    w.params["buffer"] = bufferRef{.index = 0};
    w.inputs["in"] = {audio(0)};
    nodeSpec r;
    r.id = "read";
    r.type = "bufferRead";
    r.params["buffer"] = bufferRef{.index = 0};
    r.outputs["out"] = {audio(1)};
    if (readerFirst) {
        g.nodes.push_back(std::move(r));
        g.nodes.push_back(std::move(w));
    } else {
        g.nodes.push_back(std::move(w));
        g.nodes.push_back(std::move(r));
    }
    return g;
}

std::vector<std::string> order(const graphSpec& g) {
    std::vector<std::string> ids;
    ids.reserve(g.nodes.size());
    for (const auto& n : g.nodes) ids.push_back(n.id);
    return ids;
}

}  // namespace

TEST_CASE("a buffer parameter must name a declared buffer") {
    graphSpec g = writeAndRead(false);
    CHECK(errors(g).empty());
    g.nodes[2].params["buffer"] = bufferRef{.index = 3};
    CHECK_THAT(errors(g), ContainsSubstring("node read: parameter buffer: buffer 3 is not declared"));
    g.nodes[2].params["buffer"] = 0.0;
    CHECK_THAT(errors(g), ContainsSubstring("node read: parameter buffer must name a buffer"));
    g.nodes[2].params.erase("buffer");
    CHECK_THAT(errors(g), ContainsSubstring("node read: parameter buffer is required"));
}

TEST_CASE("a buffer's writers run before its readers (samples.md 5.7)") {
    graphSpec g = writeAndRead(true);
    REQUIRE(ir::sortByDataFlow(g).empty());
    CHECK(order(g) == std::vector<std::string>{"osc", "write", "read"});
    CHECK(errors(g).empty());
}

TEST_CASE("an ordering edge that would close a cycle leaves the order to the pipes") {
    // The reader's output is recorded back into the buffer: the reader must
    // run first, a block behind the writer, as with a feedback pipe.
    graphSpec g = writeAndRead(false);
    g.nodes[1].inputs["in"] = {audio(1)};
    REQUIRE(ir::sortByDataFlow(g).empty());
    CHECK(order(g) == std::vector<std::string>{"osc", "read", "write"});
    CHECK(errors(g).empty());
}

TEST_CASE("two writers to one buffer are a hazard") {
    graphSpec g = writeAndRead(false);
    nodeSpec second = g.nodes[1];
    second.id = "write2";
    g.nodes.insert(g.nodes.begin() + 2, second);
    REQUIRE(errors(g).empty());
    std::string warnings;
    for (const auto& w : ir::hazards(g, 48000.0)) warnings += w.text() + "\n";
    CHECK_THAT(warnings, ContainsSubstring("node write2: node write also writes buffer 0"));
}
