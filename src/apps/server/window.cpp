#include "window.h"

#include <array>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <utility>

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>

#include <proj/primitives/engine.h>
#include <proj/primitives/frame.h>

namespace server {

namespace {

using namespace juce::gl;

// Draws a texture over the whole viewport. Premultiplied pixels shown
// over black need no blending.
constexpr const char* vertexShader = R"(
attribute vec2 corner;
varying vec2 uv;
void main() {
    uv = vec2((corner.x + 1.0) / 2.0, (1.0 - corner.y) / 2.0);
    gl_Position = vec4(corner, 0.0, 1.0);
})";
constexpr const char* fragmentShader = R"(
varying vec2 uv;
uniform sampler2D frame;
void main() { gl_FragColor = texture2D(frame, uv); })";

// The component the OpenGL context draws into. renderOpenGL runs on JUCE's
// OpenGL thread: it renders the engine's frame and uploads it into a
// texture allocated when the context was created, so a frame allocates
// nothing in this code.
class frameView final : public juce::Component, private juce::OpenGLRenderer {
public:
    explicit frameView(primitives::engine& engine)
        : engine_(engine), width_(engine.info().frameWidth), height_(engine.info().frameHeight) {
        setSize(width_, height_);
        context_.setRenderer(this);
        context_.setContinuousRepainting(true);
        context_.setSwapInterval(1);  // one frame per vertical refresh
        context_.attachTo(*this);
    }

    ~frameView() override { context_.detach(); }

    frameView(const frameView&) = delete;
    frameView& operator=(const frameView&) = delete;
    frameView(frameView&&) = delete;
    frameView& operator=(frameView&&) = delete;

private:
    void newOpenGLContextCreated() override {
        program_ = std::make_unique<juce::OpenGLShaderProgram>(context_);
        const bool built =
            program_->addVertexShader(juce::OpenGLHelpers::translateVertexShaderToV3(vertexShader)) &&
            program_->addFragmentShader(juce::OpenGLHelpers::translateFragmentShaderToV3(fragmentShader)) &&
            program_->link();
        if (!built) {
            // Once, at context creation: the window stays black without it.
            std::cerr << "proj-server: cannot build the window's shader: " << program_->getLastError()
                      << "\n";
            program_.reset();
        }
        glGenTextures(1, &texture_);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // OpenGL takes texture data by pointer: none yet, storage only.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width_, height_, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        glGenBuffers(1, &corners_);
        glBindBuffer(GL_ARRAY_BUFFER, corners_);
        constexpr std::array<GLfloat, 8> strip{-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
        glBufferData(GL_ARRAY_BUFFER, sizeof strip, strip.data(), GL_STATIC_DRAW);
        start_ = clock::now();
    }

    void renderOpenGL() override {
        const double t = std::chrono::duration<double>(clock::now() - start_).count();
        const auto f = engine_.renderFrame(t);
        juce::OpenGLHelpers::clear(juce::Colours::black);
        if (!f || !program_) return;
        const primitives::frame& image = f->get();
        glBindTexture(GL_TEXTURE_2D, texture_);
        // Frame pixels are 0xAARRGGBB words, BGRA bytes on little-endian
        // hosts; OpenGL takes them by pointer.
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image.width(), image.height(), GL_BGRA, GL_UNSIGNED_BYTE,
                        image.pixels().data());
        const auto scale = static_cast<float>(context_.getRenderingScale());
        glViewport(0, 0, juce::roundToInt(scale * static_cast<float>(getWidth())),
                   juce::roundToInt(scale * static_cast<float>(getHeight())));
        program_->use();
        glBindBuffer(GL_ARRAY_BUFFER, corners_);
        const GLint corner = glGetAttribLocation(program_->getProgramID(), "corner");
        glVertexAttribPointer(static_cast<GLuint>(corner), 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(static_cast<GLuint>(corner));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(static_cast<GLuint>(corner));
    }

    void openGLContextClosing() override {
        program_.reset();
        glDeleteTextures(1, &texture_);
        glDeleteBuffers(1, &corners_);
    }

    using clock = std::chrono::steady_clock;

    primitives::engine& engine_;
    int width_ = 0;
    int height_ = 0;
    juce::OpenGLContext context_;
    std::unique_ptr<juce::OpenGLShaderProgram> program_;
    GLuint texture_ = 0;
    GLuint corners_ = 0;
    clock::time_point start_ = clock::now();
};

// A plain window around the frame view; closing it runs @p onClose.
class frameWindow final : public juce::DocumentWindow {
public:
    frameWindow(primitives::engine& engine, std::function<void()> onClose)
        : juce::DocumentWindow("proj", juce::Colours::black, juce::DocumentWindow::closeButton),
          view_(std::make_unique<frameView>(engine)),
          onClose_(std::move(onClose)) {
        setUsingNativeTitleBar(true);
        // JUCE takes the content by pointer; this window owns it.
        setContentNonOwned(view_.get(), true);
        setResizable(true, false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    // The content goes before the view it points to.
    ~frameWindow() override { clearContentComponent(); }

    frameWindow(const frameWindow&) = delete;
    frameWindow& operator=(const frameWindow&) = delete;
    frameWindow(frameWindow&&) = delete;
    frameWindow& operator=(frameWindow&&) = delete;

    void closeButtonPressed() override {
        if (onClose_) onClose_();
    }

private:
    std::unique_ptr<frameView> view_;
    std::function<void()> onClose_;
};

}  // namespace

class videoWindow::impl {
public:
    impl(primitives::engine& engine, std::function<void()> onClose) : window_(engine, std::move(onClose)) {}

private:
    frameWindow window_;
};

videoWindow::videoWindow(primitives::engine& engine, std::function<void()> onClose)
    : impl_(std::make_unique<impl>(engine, std::move(onClose))) {}

videoWindow::~videoWindow() = default;

}  // namespace server
