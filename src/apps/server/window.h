// The server's visual output: a separate OpenGL window that shows the
// visual layer (AGENTS.md: "a separate OpenGL window for visual output").
// JUCE: the window lives on the message thread; frames render on JUCE's
// OpenGL thread, which is the engine's render thread.

#pragma once

#include <functional>
#include <memory>

#include <espresso/primitives/engine.h>

#include "splash.h"

namespace server {

/// A window of the engine's frame size that renders a frame on every
/// vertical refresh and shows the visual layer, or a splash screen of
/// centered text while the engine renders no frame. Message thread only.
class videoWindow {
public:
    /// Opens the window with the splash screen @p splash. @p onClose runs
    /// on the message thread when the user closes it.
    videoWindow(primitives::engine& engine, const splashText& splash, std::function<void()> onClose);
    ~videoWindow();

    videoWindow(const videoWindow&) = delete;
    videoWindow& operator=(const videoWindow&) = delete;
    videoWindow(videoWindow&&) = delete;
    videoWindow& operator=(videoWindow&&) = delete;

private:
    class impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace server
