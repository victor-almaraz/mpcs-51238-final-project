// Frame pipes and spectral parameters in IR validation (samples.md §10.4,
// §12.2).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <utility>

#include <espresso/metadata/graph.h>

#include "support/pipes.h"
#include "support/validation.h"

using Catch::Matchers::ContainsSubstring;
using metadata::graphSpec;
using metadata::nodeSpec;
using support::audio;
using support::errors;
using support::events;

namespace {

nodeSpec node(std::string id, std::string type) {
    nodeSpec n;
    n.id = std::move(id);
    n.type = std::move(type);
    return n;
}

// audio 0 -> fft -> event 1 -> ifft -> audio 2.
graphSpec chain() {
    graphSpec g;
    g.pipes = {{.pipe = audio(0), .channels = 1, .feedback = false},
               {.pipe = events(1), .channels = 1, .feedback = false},
               {.pipe = audio(2), .channels = 1, .feedback = false}};
    auto f = node("fft", "fft");
    f.inputs["in"] = {audio(0)};
    f.outputs["out"] = {events(1)};
    auto i = node("ifft", "ifft");
    i.inputs["in"] = {events(1)};
    i.outputs["out"] = {audio(2)};
    g.nodes = {f, i};
    return g;
}

}  // namespace

TEST_CASE("an fft and an ifft connect through a frame pipe") { CHECK(errors(chain()).empty()); }

TEST_CASE("only frame ports connect to a pipe that carries frames") {
    graphSpec g = chain();
    auto gate = node("env", "adsr");
    gate.inputs["gate"] = {events(1)};
    gate.outputs["out"] = {audio(2)};
    g.nodes.push_back(gate);
    CHECK_THAT(errors(g),
               ContainsSubstring(
                   "node env: event pipe 1 carries spectral frames; only frame ports may connect to it"));
}

TEST_CASE("a frame pipe has one writer and every frame input has one") {
    graphSpec g = chain();
    auto second = g.nodes[0];
    second.id = "fft2";
    g.nodes.insert(g.nodes.begin() + 1, second);
    CHECK_THAT(errors(g), ContainsSubstring("node fft2: event pipe 1 already has a frame writer, node fft"));

    graphSpec orphan = chain();
    orphan.nodes.erase(orphan.nodes.begin());
    CHECK_THAT(errors(orphan),
               ContainsSubstring("node ifft: reads frames from event pipe 1, which no frame output writes"));
}

TEST_CASE("a spectral size is a power of two from 256 to 16384 and an overlap 2 or 4 (samples.md 10.1)") {
    for (const double size : {256.0, 2048.0, 16384.0}) {
        graphSpec g = chain();
        g.nodes[0].params["size"] = size;
        CHECK(errors(g).empty());
    }
    graphSpec g = chain();
    g.nodes[0].params["size"] = 3000.0;
    CHECK_THAT(errors(g), ContainsSubstring("parameter size must be a power of two"));
    g.nodes[0].params["size"] = 32768.0;
    CHECK_THAT(errors(g), ContainsSubstring("parameter size is 32768; expected 256 to 16384"));
    g.nodes[0].params["size"] = 2048.0;
    g.nodes[0].params["overlap"] = 3.0;
    CHECK_THAT(errors(g), ContainsSubstring("parameter overlap must be a power of two"));
}
