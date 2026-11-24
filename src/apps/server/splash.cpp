#include "splash.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace server {

splashText splashTextFor(std::string_view version, std::uint16_t port) {
    return {.name = "espresso",
            .version = std::string(version),
            .status = "listening on 127.0.0.1:" + std::to_string(port) + " \xc2\xb7 no visual output"};
}

pixelSize splashSize(int frameWidth, int frameHeight) noexcept {
    const int w = std::max(frameWidth, 1);
    const int h = std::max(frameHeight, 1);
    const double scale = std::min(2.0, static_cast<double>(maxSplashSide) / std::max(w, h));
    const auto scaled = [scale](int side) {
        return std::max(1, static_cast<int>(std::lround(side * scale)));
    };
    return {.width = scaled(w), .height = scaled(h)};
}

viewport fitCentered(viewport area, pixelSize content) noexcept {
    if (area.width <= 0 || area.height <= 0 || content.width <= 0 || content.height <= 0) return area;
    // Compare area.width / area.height with content.width / content.height
    // without division: the wider of the two decides which side fills.
    const auto areaW = static_cast<std::int64_t>(area.width);
    const auto areaH = static_cast<std::int64_t>(area.height);
    viewport fit = area;
    if (areaW * content.height > areaH * content.width) {
        // The area is wider: the content fills its height.
        fit.width = static_cast<int>(areaH * content.width / content.height);
        fit.x = area.x + (area.width - fit.width) / 2;
    } else {
        // The area is taller: the content fills its width.
        fit.height = static_cast<int>(areaW * content.height / content.width);
        fit.y = area.y + (area.height - fit.height) / 2;
    }
    return fit;
}

}  // namespace server
