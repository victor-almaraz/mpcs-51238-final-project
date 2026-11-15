// The server's visual output: a separate OpenGL window that shows the
// visual layer (AGENTS.md: "a separate OpenGL window for visual output").
// JUCE: the window lives on the message thread; frames render on JUCE's
// OpenGL thread, which is the engine's render thread.

#pragma once

#include <functional>
#include <memory>

#include <proj/primitives/engine.h>

namespace server {

/// A window of the engine's frame size that renders a frame on every
/// vertical refresh and shows the visual layer. Message thread only.
class videoWindow {
public:
    /// Opens the window. @p onClose runs on the message thread when the
    /// user closes it.
    videoWindow(primitives::engine& engine, std::function<void()> onClose);
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
