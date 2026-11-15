#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>

#include <proj/metadata/graph.h>
#include <proj/primitives/frame.h>
#include <proj/primitives/pipe.h>
#include <proj/primitives/pipeTable.h>
#include <proj/primitives/primitive.h>

using primitives::affine;
using primitives::frame;

TEST_CASE("a frame starts transparent with the identity transform (primitives.md 5.5)") {
    const frame f(4, 3);
    CHECK(f.width() == 4);
    CHECK(f.height() == 3);
    CHECK(f.pixels().size() == 12);
    CHECK(std::ranges::all_of(f.pixels(), [](std::uint32_t p) { return p == 0u; }));
    CHECK(f.transform() == affine{});
    CHECK(f.at(-1, 0) == 0u);  // outside the frame
    CHECK(f.at(4, 0) == 0u);
    CHECK(frame(0, -2).width() == 1);  // at least one pixel each way
}

TEST_CASE("clear makes a frame transparent and resets its transform") {
    frame f(2, 2);
    f.pixels()[3] = 0xff00ff00u;
    f.setTransform({.x0 = 5.0});
    f.clear();
    CHECK(f.at(1, 1) == 0u);
    CHECK(f.transform() == affine{});
}

TEST_CASE("copyFrom copies pixels and transform and clips frames of another size") {
    frame from(3, 2);
    from.pixels()[0] = 0xffff0000u;  // (0, 0)
    from.pixels()[5] = 0xff0000ffu;  // (2, 1)
    from.setTransform({.xx = 2.0, .yy = 2.0});
    frame same(3, 2);
    same.copyFrom(from);
    CHECK(same.at(0, 0) == 0xffff0000u);
    CHECK(same.at(2, 1) == 0xff0000ffu);
    CHECK(same.transform() == from.transform());

    frame smaller(2, 2);
    smaller.pixels()[1] = 0xffffffffu;  // overwritten by the copy
    smaller.copyFrom(from);
    CHECK(smaller.at(0, 0) == 0xffff0000u);
    CHECK(smaller.at(1, 0) == 0u);
    CHECK(smaller.at(1, 1) == 0u);  // (2, 1) is outside the smaller frame

    frame larger(4, 3);
    larger.pixels()[11] = 0xffffffffu;  // (3, 2): outside the source, cleared
    larger.copyFrom(from);
    CHECK(larger.at(2, 1) == 0xff0000ffu);
    CHECK(larger.at(3, 2) == 0u);
}

TEST_CASE("the pipe table allocates video pipes at the frame size only when asked") {
    using metadata::pipeKind;
    const primitives::prepareInfo info{.sampleRate = 48000.0, .blockSize = 16, .frameWidth = 8, .frameHeight = 6};
    const primitives::pipeTable none({.audio = 1, .control = 1, .event = 1}, info);
    CHECK_FALSE(none.contains({pipeKind::video, 0}));

    primitives::pipeTable t({.audio = 1, .control = 1, .event = 1, .video = 2}, info);
    CHECK(t.contains({pipeKind::video, 1}));
    CHECK_FALSE(t.contains({pipeKind::video, 2}));
    REQUIRE(t.find({pipeKind::video, 1}) != nullptr);
    CHECK(t.find({pipeKind::video, 1})->kind() == pipeKind::video);
    CHECK(t.video(0).image().width() == 8);
    CHECK(t.video(0).image().height() == 6);
}
