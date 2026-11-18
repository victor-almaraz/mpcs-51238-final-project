// The server window's splash screen: its text, size, placement, and when
// the window shows it instead of a frame.

#include <catch2/catch_test_macros.hpp>

#include "splash.h"

using server::windowContent;

TEST_CASE("the splash names the program and version and where the server listens") {
    const auto text = server::splashTextFor("1.2.3", 57190);
    CHECK(text.name == "espresso");
    CHECK(text.version == "1.2.3");
    CHECK(text.status == "listening on 127.0.0.1:57190 \xc2\xb7 no visual output");
}

TEST_CASE("the window shows the splash while the engine renders no frame") {
    CHECK(server::contentFor(false, true) == windowContent::splash);
    // A plan draws: its frame replaces the splash.
    CHECK(server::contentFor(true, true) == windowContent::frame);
    // Without the shader, the window shows only its background.
    CHECK(server::contentFor(true, false) == windowContent::nothing);
    CHECK(server::contentFor(false, false) == windowContent::nothing);
}

TEST_CASE("the splash image is twice the frame size within the largest side") {
    const auto standard = server::splashSize(1280, 720);
    CHECK(standard.width == 2560);
    CHECK(standard.height == 1440);
    // 8192 x 2048 scales by 4096 / 8192 = 0.5 to 4096 x 1024.
    const auto large = server::splashSize(8192, 2048);
    CHECK(large.width == server::maxSplashSide);
    CHECK(large.height == 1024);
    // 3000 x 16 scales by 4096 / 3000; 16 x 4096 / 3000 = 21.8, rounded.
    const auto thin = server::splashSize(3000, 16);
    CHECK(thin.width == 4096);
    CHECK(thin.height == 22);
    const auto empty = server::splashSize(0, 0);
    CHECK(empty.width >= 1);
    CHECK(empty.height >= 1);
}

TEST_CASE("the splash keeps its aspect centered in the window") {
    const server::pixelSize splash{.width = 2560, .height = 1440};
    // Same aspect: the splash fills the window.
    CHECK(server::fitCentered({.x = 0, .y = 0, .width = 1280, .height = 720}, splash) ==
          server::viewport{.x = 0, .y = 0, .width = 1280, .height = 720});
    // A wider window: full height, 720 x 16 / 9 = 1280 wide, (2000 - 1280) / 2 = 360 on each side.
    CHECK(server::fitCentered({.x = 0, .y = 0, .width = 2000, .height = 720}, splash) ==
          server::viewport{.x = 360, .y = 0, .width = 1280, .height = 720});
    // A taller window: full width, 800 x 9 / 16 = 450 high, (1000 - 450) / 2 = 275 above and below.
    CHECK(server::fitCentered({.x = 0, .y = 0, .width = 800, .height = 1000}, splash) ==
          server::viewport{.x = 0, .y = 275, .width = 800, .height = 450});
    // The offset of the area carries over.
    CHECK(server::fitCentered({.x = 10, .y = 20, .width = 800, .height = 1000}, splash) ==
          server::viewport{.x = 10, .y = 295, .width = 800, .height = 450});
    // An empty window is left as it is.
    CHECK(server::fitCentered({.x = 0, .y = 0, .width = 0, .height = 0}, splash) == server::viewport{});
}
