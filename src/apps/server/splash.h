// The splash screen of the server window: what the window shows while no
// plan draws to a videoOut, and where. The window draws the splash with
// JUCE (window.cpp); these decisions have no JUCE dependency.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace server {

/// The three centered lines of the splash screen.
struct splashText {
    /// The program name.
    std::string name;
    /// The server version.
    std::string version;
    /// What the server does while the window shows the splash.
    std::string status;
};

/// The splash text for server version @p version listening on port @p port.
splashText splashTextFor(std::string_view version, std::uint16_t port);

/// A size, in pixels.
struct pixelSize {
    /// Width, in pixels.
    int width = 0;
    /// Height, in pixels.
    int height = 0;
};

/// Largest side of the splash image, in pixels.
inline constexpr int maxSplashSide = 4096;

/// The size of the splash image for frames of @p frameWidth ×
/// @p frameHeight: the frame's aspect at twice its size, so text stays
/// sharp on a high-density display, scaled down to fit @ref maxSplashSide
/// and rounded. Each side is at least 1.
pixelSize splashSize(int frameWidth, int frameHeight) noexcept;

/// A rectangle of the window, in physical pixels from its lower left corner
/// (OpenGL's viewport).
struct viewport {
    /// Left edge.
    int x = 0;
    /// Bottom edge.
    int y = 0;
    /// Width.
    int width = 0;
    /// Height.
    int height = 0;

    /// True when every edge is equal.
    friend bool operator==(const viewport&, const viewport&) = default;
};

/// The largest rectangle with the aspect of @p content centered in
/// @p area. An empty area or content gives @p area.
viewport fitCentered(viewport area, pixelSize content) noexcept;

/// What the window shows on a refresh.
enum class windowContent : std::uint8_t {
    frame,   ///< The plan's frame.
    splash,  ///< The splash screen.
    nothing  ///< Only the background: the window cannot draw textures.
};

/// What the window shows when the engine rendered a frame (@p hasFrame) or
/// none, and its shader is (@p canDraw) or is not built.
constexpr windowContent contentFor(bool hasFrame, bool canDraw) noexcept {
    if (!canDraw) return windowContent::nothing;
    return hasFrame ? windowContent::frame : windowContent::splash;
}

}  // namespace server
