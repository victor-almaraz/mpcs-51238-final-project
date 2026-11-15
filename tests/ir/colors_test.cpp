// Color parameters in IR validation (primitives.md §7.7.3, §7.7.4).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <proj/ir/ir.h>
#include <proj/metadata/graph.h>
#include <proj/metadata/primitiveSpec.h>

using Catch::Matchers::ContainsSubstring;
using metadata::graphSpec;
using metadata::nodeSpec;
using metadata::pipeKind;
using metadata::pipeRef;

namespace {

constexpr pipeRef control(std::int32_t i) { return {.kind = pipeKind::control, .index = i}; }
constexpr pipeRef video(std::int32_t i) { return {.kind = pipeKind::video, .index = i}; }

// A rect drawing to video pipe 0 with @p fill; control 1 has one channel,
// control 2 four.
graphSpec rectWith(metadata::paramValue fill) {
    graphSpec g;
    g.pipes = {{.pipe = control(1), .channels = 1, .feedback = false},
               {.pipe = control(2), .channels = metadata::colorChannels, .feedback = false},
               {.pipe = video(0), .channels = 1, .feedback = false}};
    nodeSpec n;
    n.id = "box";
    n.type = "rect";
    n.params["fill"] = std::move(fill);
    n.outputs["out"] = {video(0)};
    g.nodes.push_back(std::move(n));
    return g;
}

std::string errors(const graphSpec& g) {
    std::string out;
    for (const auto& e : ir::validate(g)) out += e.text() + "\n";
    return out;
}

} // namespace

TEST_CASE("a color is four components from 0 to 1, none where allowed, or a 4-channel control pipe") {
    CHECK(errors(rectWith(std::vector<double>{1.0, 0.5, 0.0, 1.0})).empty());
    CHECK(errors(rectWith(std::string("none"))).empty());
    CHECK(errors(rectWith(control(2))).empty());
    CHECK_THAT(errors(rectWith(std::vector<double>{1.0, 0.5, 0.0})),
               ContainsSubstring("parameter fill must be four numbers from 0 to 1"));
    CHECK_THAT(errors(rectWith(std::vector<double>{1.0, 0.5, 0.0, 1.5})),
               ContainsSubstring("parameter fill must be four numbers from 0 to 1"));
    CHECK_THAT(errors(rectWith(std::string("blue"))), ContainsSubstring("parameter fill must be a color or none"));
    CHECK_THAT(errors(rectWith(control(1))), ContainsSubstring("parameter fill must follow a pipe of 4 channels"));
}

TEST_CASE("a background color has no none") {
    graphSpec g;
    g.pipes = {{.pipe = video(0), .channels = 1, .feedback = false}};
    nodeSpec n;
    n.id = "bg";
    n.type = "background";
    n.params["color"] = std::string("none");
    n.outputs["out"] = {video(0)};
    g.nodes.push_back(std::move(n));
    CHECK_THAT(errors(g), ContainsSubstring("parameter color must be a color"));
}

TEST_CASE("a video pipe has one channel") {
    graphSpec g = rectWith(std::string("none"));
    g.pipes.back().channels = 2;
    CHECK_THAT(errors(g), ContainsSubstring("video pipe 0: video pipes have one channel"));
}

