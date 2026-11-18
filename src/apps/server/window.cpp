#include "window.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>

#include <espresso/primitives/engine.h>
#include <espresso/primitives/frame.h>

#include "splash.h"

namespace server {

using namespace juce::gl;

namespace {

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

// The splash screen's palette: quiet grays on a dark neutral background.
const juce::Colour splashBackground{0xff161616};
const juce::Colour splashName{0xffd6d6d6};
const juce::Colour splashVersion{0xff9a9a9a};
const juce::Colour splashStatus{0xff7c7c7c};

// One line of the splash: its text, font height and the space above it
// (fractions of the layout unit), color, and letter spacing.
struct splashLine {
    std::reference_wrapper<const std::string> text;
    float height = 0.0f;
    float gap = 0.0f;
    juce::Colour colour;
    float tracking = 0.0f;
};

// Draws @p text centered on a splash image of @p size with JUCE's software
// renderer, once, on the message thread. The layout unit is the height of
// the largest 16:9 box in the image, so the text keeps its proportions in
// any frame aspect.
primitives::image drawSplash(const splashText& text, pixelSize size) {
    juce::Image canvas(juce::Image::ARGB, size.width, size.height, false);
    {
        juce::Graphics g(canvas);
        g.fillAll(splashBackground);
        const auto width = static_cast<float>(size.width);
        const auto height = static_cast<float>(size.height);
        const float unit = std::min(height, width * 9.0f / 16.0f);
        const std::array lines{
            splashLine{
                .text = text.name, .height = 0.13f, .gap = 0.0f, .colour = splashName, .tracking = 0.04f},
            splashLine{.text = text.version, .height = 0.042f, .gap = 0.025f, .colour = splashVersion},
            splashLine{.text = text.status, .height = 0.034f, .gap = 0.06f, .colour = splashStatus}};
        float block = 0.0f;
        for (const auto& l : lines) block += (l.gap + l.height) * unit;
        float y = (height - block) / 2.0f;
        for (const auto& l : lines) {
            y += l.gap * unit;
            const float h = l.height * unit;
            const std::string& s = l.text.get();
            g.setColour(l.colour);
            g.setFont(juce::Font(juce::FontOptions(h).withKerningFactor(l.tracking)));
            g.drawFittedText(juce::String::fromUTF8(s.data(), static_cast<int>(s.size())),
                             juce::Rectangle<float>(0.0f, y, width, h * 1.25f).toNearestInt(),
                             juce::Justification::centredTop, 1);
            y += h;
        }
    }
    primitives::image splash{.width = size.width,
                             .height = size.height,
                             .pixels = std::vector<std::uint32_t>(static_cast<std::size_t>(size.width) *
                                                                  static_cast<std::size_t>(size.height))};
    const juce::Image::BitmapData bitmap(canvas, juce::Image::BitmapData::readOnly);
    std::size_t i = 0;
    for (int y = 0; y < size.height; ++y) {
        for (int x = 0; x < size.width; ++x) {
            // Premultiplied 0xAARRGGBB, as frame pixels are.
            splash.pixels[i++] = bitmap.getPixelColour(x, y).getPixelARGB().getNativeARGB();
        }
    }
    return splash;
}

// A new texture, bound, with minification filter @p minFilter and clamped
// edges.
GLuint makeTexture(GLint minFilter) {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

// The component the OpenGL context draws into. renderOpenGL runs on JUCE's
// OpenGL thread: it renders the engine's frame and uploads it into a
// texture allocated when the context was created, so a frame allocates
// nothing in this code. While the engine renders no frame, it shows the
// splash screen, drawn at construction and uploaded once per context.
class frameView final : public juce::Component, private juce::OpenGLRenderer {
public:
    frameView(primitives::engine& engine, const splashText& splash)
        : engine_(engine),
          width_(engine.info().frameWidth),
          height_(engine.info().frameHeight),
          splash_(drawSplash(splash, splashSize(width_, height_))) {
        setSize(width_, height_);
        context_.setRenderer(this);
        context_.setComponentPaintingEnabled(false);  // nothing to paint: no buffer, no extra draw
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
            // Once, at context creation: the window shows only its background without it.
            std::cerr << "espresso-server: cannot build the window's shader: " << program_->getLastError()
                      << "\n";
            program_.reset();
        } else {
            corner_ = glGetAttribLocation(program_->getProgramID(), "corner");
        }
        texture_ = makeTexture(GL_LINEAR);
        // OpenGL takes texture data by pointer: none yet, storage only.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width_, height_, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        // The splash, with mipmaps so that a small window shows it smoothly.
        splashTexture_ = makeTexture(GL_LINEAR_MIPMAP_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, splash_.width, splash_.height, 0, GL_BGRA, GL_UNSIGNED_BYTE,
                     splash_.pixels.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glGenBuffers(1, &corners_);
        glBindBuffer(GL_ARRAY_BUFFER, corners_);
        constexpr std::array<GLfloat, 8> strip{-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
        glBufferData(GL_ARRAY_BUFFER, sizeof strip, strip.data(), GL_STATIC_DRAW);
        start_ = clock::now();
    }

    void renderOpenGL() override {
        const double t = std::chrono::duration<double>(clock::now() - start_).count();
        const auto f = engine_.renderFrame(t);
        switch (contentFor(f.has_value(), program_ != nullptr && corner_ >= 0)) {
        case windowContent::frame: {
            if (!f) break;  // contentFor gives frame only with one
            juce::OpenGLHelpers::clear(juce::Colours::black);
            const primitives::frame& image = f->get();
            glBindTexture(GL_TEXTURE_2D, texture_);
            // Frame pixels are 0xAARRGGBB words, BGRA bytes on
            // little-endian hosts; OpenGL takes them by pointer.
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image.width(), image.height(), GL_BGRA, GL_UNSIGNED_BYTE,
                            image.pixels().data());
            // JUCE sets the viewport, in physical pixels, before
            // renderOpenGL; the frame fills it.
            drawBoundTexture();
            break;
        }
        case windowContent::splash: {
            juce::OpenGLHelpers::clear(splashBackground);
            // The splash keeps its aspect, centered on its background.
            std::array<GLint, 4> area{};
            glGetIntegerv(GL_VIEWPORT, area.data());
            const viewport fit =
                fitCentered({.x = area[0], .y = area[1], .width = area[2], .height = area[3]},
                            {.width = splash_.width, .height = splash_.height});
            glViewport(fit.x, fit.y, fit.width, fit.height);
            glBindTexture(GL_TEXTURE_2D, splashTexture_);
            drawBoundTexture();
            glViewport(area[0], area[1], area[2], area[3]);
            break;
        }
        case windowContent::nothing: juce::OpenGLHelpers::clear(splashBackground); break;
        }
    }

    // Draws the bound texture over the viewport.
    void drawBoundTexture() {
        program_->use();
        glBindBuffer(GL_ARRAY_BUFFER, corners_);
        const auto corner = static_cast<GLuint>(corner_);
        glVertexAttribPointer(corner, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(corner);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(corner);
    }

    void openGLContextClosing() override {
        program_.reset();
        glDeleteTextures(1, &texture_);
        glDeleteTextures(1, &splashTexture_);
        glDeleteBuffers(1, &corners_);
    }

    using clock = std::chrono::steady_clock;

    primitives::engine& engine_;
    int width_ = 0;
    int height_ = 0;
    primitives::image splash_;  // the splash screen's pixels, kept for a new context
    juce::OpenGLContext context_;
    std::unique_ptr<juce::OpenGLShaderProgram> program_;
    GLuint texture_ = 0;
    GLuint splashTexture_ = 0;
    GLuint corners_ = 0;
    GLint corner_ = -1;  // the shader's corner attribute; -1 when it has none
    clock::time_point start_ = clock::now();
};

}  // namespace

// A plain window around the frame view; closing it runs onClose.
class videoWindow::impl final : public juce::DocumentWindow {
public:
    impl(primitives::engine& engine, const splashText& splash, std::function<void()> onClose)
        : juce::DocumentWindow("espresso", splashBackground, juce::DocumentWindow::closeButton),
          view_(engine, splash),
          onClose_(std::move(onClose)) {
        setUsingNativeTitleBar(true);
        // JUCE takes the content by pointer; this window owns it.
        setContentNonOwned(&view_, true);
        setResizable(true, false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    // The content goes before the view it points to.
    ~impl() override { clearContentComponent(); }

    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;
    impl(impl&&) = delete;
    impl& operator=(impl&&) = delete;

    void closeButtonPressed() override {
        if (onClose_) onClose_();
    }

private:
    frameView view_;
    std::function<void()> onClose_;
};

videoWindow::videoWindow(primitives::engine& engine, const splashText& splash, std::function<void()> onClose)
    : impl_(std::make_unique<impl>(engine, splash, std::move(onClose))) {}

videoWindow::~videoWindow() = default;

}  // namespace server
