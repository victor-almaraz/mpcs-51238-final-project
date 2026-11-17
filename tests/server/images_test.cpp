// Image files in the server: PNG frames and imagePlayer files
// (primitives.md §7.7.6).

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <espresso/primitives/frame.h>

#include "images.h"
#include "support/tempDir.h"
#include "support/unwrap.h"

TEST_CASE("a frame written as PNG loads back with the same pixels") {
    const support::tempDir dir;
    primitives::frame f(3, 2);
    f.pixels()[0] = 0xffff0000u;  // opaque red
    f.pixels()[1] = 0x00000000u;  // transparent
    f.pixels()[2] = 0xff0000ffu;  // opaque blue
    f.pixels()[3] = 0xff00ff00u;  // opaque green
    REQUIRE(server::writePng(dir.path() / "frame.png", f));

    std::string error;
    const auto loaded =
        support::unwrap(server::imageLoaderFor(dir.path())("frame.png", error));  // relative to the root
    CHECK(loaded.width == 3);
    CHECK(loaded.height == 2);
    CHECK(loaded.pixels[0] == 0xffff0000u);
    CHECK(loaded.pixels[1] == 0x00000000u);
    CHECK(loaded.pixels[2] == 0xff0000ffu);
    CHECK(loaded.pixels[3] == 0xff00ff00u);
}

TEST_CASE("a missing image file is an error, not an image") {
    const support::tempDir dir;
    std::string error;
    CHECK_FALSE(server::loadImageFile(dir.path() / "missing.png", error));
    CHECK_FALSE(error.empty());
}
