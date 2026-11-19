// Buffer primitives: bufferWrite records into a buffer and bufferRead
// reads it at any position (samples.md §6). Both bind to their buffer
// when built; the plan holds it, so the audio thread never frees it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <espresso/primitives/buffer.h>
#include <espresso/primitives/primitive.h>
#include <espresso/primitives/realtime.h>

#include "node.h"

namespace primitives {

namespace {

// @p p wrapped into [0, @p length): a read position on a loop. NaN and
// infinities read the start.
double wrap(double p, double length) noexcept ESPRESSO_NONBLOCKING {
    double w = std::fmod(p, length);
    if (w < 0.0) w += length;
    return w >= 0.0 && w < length ? w : 0.0;
}

// The shorter way from @p a to @p b around a loop of @p length, in
// (-length/2, length/2]: half way around counts as forward, the way heads
// move.
double around(double a, double b, double length) noexcept ESPRESSO_NONBLOCKING {
    const double d = b - a;
    return d - (length * std::ceil((d / length) - 0.5));
}

enum class interpolation : std::uint8_t { cubic, linear, none };

// Channel @p c of @p b at fractional frame @p pos, which is in [0, frames).
float readAt(const sampleBuffer& b, int c, double pos, interpolation mode) noexcept ESPRESSO_NONBLOCKING {
    const auto line = b.channel(c);
    const std::size_t size = line.size();
    const auto i = static_cast<std::size_t>(pos);
    const auto frac = static_cast<float>(pos - static_cast<double>(i));
    const auto at = [&](std::size_t k) { return line[k % size]; };
    switch (mode) {
    case interpolation::none: return at(static_cast<std::size_t>(std::lround(pos)));
    case interpolation::linear: return at(i) + (frac * (at(i + 1) - at(i)));
    case interpolation::cubic: {  // 4-point Hermite, as the delay line's
        const float xm1 = at(i + size - 1);
        const float x0 = at(i);
        const float x1 = at(i + 1);
        const float x2 = at(i + 2);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - (2.5f * x0) + (2.0f * x1) - (0.5f * x2);
        const float c3 = (0.5f * (x2 - xm1)) + (1.5f * (x0 - x1));
        return (((((c3 * frac) + c2) * frac) + c1) * frac) + x0;
    }
    }
    return 0.0f;
}

// --- bufferWrite (§6.1, §6.2) ---------------------------------------------

constexpr double recordRamp = 0.005;  // seconds from 0 to 1

class bufferWrite final : public nodeBase {
public:
    explicit bufferWrite(nodeContext& c)
        : nodeBase(c),
          buffer_(c.buffer("buffer")),
          in_(c.signalIn("in")),
          reset_(c.eventIn("reset")),
          position_(c.controlOut("position")),
          record_(c.param("record")),
          keep_(c.param("keep")),
          once_(c.choice("mode") == 1),
          rampStep_(static_cast<float>(1.0 / std::max(1.0, recordRamp * c.sampleRate()))),
          sampleRate_(c.sampleRate()) {}

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        auto& o = static_cast<bufferWrite&>(outgoing);
        // The head stays inside this node's buffer, whatever the other's.
        head_ = std::min(o.head_, buffer_->frames() - 1);
        stopped_ = o.stopped_ && once_;
        adoptFinite(ramp_, o.ramp_);
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        const int n = ctx.numSamples;
        const auto count = static_cast<std::size_t>(n);
        sampleBuffer& b = *buffer_;
        const int channels = b.channels();
        const int inChannels = std::clamp(in_.channels(), 1, maxChannels);
        std::array<std::span<const float>, maxChannels> x{};
        for (int c = 0; c < inChannels; ++c) x[static_cast<std::size_t>(c)] = in_.block(c, n);
        const auto record = record_.block(0, n);
        const auto keep = keep_.block(0, n);
        const auto resets = reset_ ? reset_->events() : std::span<const event>{};
        std::size_t nextReset = 0;
        for (std::size_t i = 0; i < count; ++i) {
            for (; nextReset < resets.size() && resets[nextReset].sampleOffset <= i; ++nextReset) {
                head_ = 0;
                stopped_ = false;
            }
            // fmin and fmax, unlike clamp, turn a NaN into 0.
            const float target = std::fmin(std::fmax(record[i], 0.0f), 1.0f);
            ramp_ =
                ramp_ < target ? std::fmin(ramp_ + rampStep_, target) : std::fmax(ramp_ - rampStep_, target);
            // Steps of 1/n do not sum to 1 exactly in float: snap to the end
            // of the ramp, so it takes n samples and no stray last one.
            if (std::abs(ramp_ - target) < rampStep_ * 1e-3f) ramp_ = target;
            // A retiring writer leaves the buffer to the plan replacing it (§5.5).
            if (ramp_ <= 0.0f || stopped_ || retiring()) continue;
            const float r = ramp_;
            const float k = std::fmin(std::fmax(keep[i], 0.0f), 1.0f);
            const float hold = 1.0f - r + (r * k);
            for (int c = 0; c < channels; ++c) {
                const float in = mapChannel(c, channels, inChannels,
                                            [&](int from) { return x[static_cast<std::size_t>(from)][i]; });
                float& old = b.channel(c)[head_];
                const float v = (old * hold) + (in * r);
                old = std::isfinite(v) ? v : 0.0f;  // a non-finite input leaves no lasting trace
            }
            if (++head_ >= b.frames()) {
                if (once_) {
                    head_ = b.frames() - 1;
                    stopped_ = true;
                } else {
                    head_ = 0;
                }
            }
        }
        emitAll(position_, static_cast<float>(static_cast<double>(head_) / sampleRate_));
    }

private:
    std::shared_ptr<sampleBuffer> buffer_;
    signal in_;
    std::shared_ptr<const ePipe> reset_;
    controlOutput position_;
    signal record_;
    signal keep_;
    bool once_ = false;
    float rampStep_ = 1.0f;
    double sampleRate_ = 48000.0;
    std::size_t head_ = 0;
    float ramp_ = 0.0f;
    bool stopped_ = false;  // a once-mode head at the end
};

// --- bufferRead (§6.3, §6.4) ----------------------------------------------

constexpr double seamFade = 0.010;  // seconds

class bufferRead final : public nodeBase {
public:
    explicit bufferRead(nodeContext& c)
        : nodeBase(c),
          buffer_(c.buffer("buffer")),
          out_(c.audioOut("out")),
          position_(c.param("position")),
          mode_(static_cast<interpolation>(c.choice("interpolation"))),
          sampleRate_(c.sampleRate()),
          fadeLength_(std::max(1, static_cast<int>(std::lround(seamFade * c.sampleRate())))),
          positions_(static_cast<std::size_t>(std::max(c.blockSize(), 1))),
          scratch_(c.blockSize()) {}

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        auto& o = static_cast<bufferRead&>(outgoing);
        const auto length = static_cast<double>(buffer_->frames());
        if (std::isfinite(o.last_)) last_ = wrap(o.last_, length);
        if (std::isfinite(o.lastTarget_)) lastTarget_ = wrap(o.lastTarget_, length);
        adoptFinite(velocity_, o.velocity_);
        if (o.fadeLeft_ > 0 && std::isfinite(o.oldHead_) && std::isfinite(o.oldVelocity_)) {
            fadeLeft_ = o.fadeLeft_;
            oldHead_ = wrap(o.oldHead_, length);
            oldVelocity_ = o.oldVelocity_;
        }
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        const int n = ctx.numSamples;
        const auto count = static_cast<std::size_t>(n);
        const sampleBuffer& b = *buffer_;
        const auto length = static_cast<double>(b.frames());
        const auto pos = std::span(positions_).first(count);
        headsFor(pos, n, length);
        const int channels = b.channels();
        for (int o = 0; o < out_.channels; ++o) {
            // Each output channel replays the block's heads and seam fade.
            double previous = last_;
            double oldHead = oldHead_;
            double oldVelocity = oldVelocity_;
            double velocity = velocity_;
            int fadeLeft = fadeLeft_;
            const auto out = scratch_.first(count);
            for (std::size_t i = 0; i < count; ++i) {
                const double p = pos[i];
                if (std::isfinite(previous)) {
                    const double step = around(previous, p, length);
                    if (std::abs(step) > n) {
                        // A jump: the old head carries on and fades out (§6.4).
                        oldHead = wrap(previous + velocity, length);
                        oldVelocity = velocity;
                        fadeLeft = fadeLength_;
                    } else {
                        velocity = step;
                    }
                }
                previous = p;
                const auto sample = [&](double at) {
                    return mapChannel(o, out_.channels, channels,
                                      [&](int c) { return readAt(b, c, at, mode_); });
                };
                float y = sample(p);
                if (fadeLeft > 0) {
                    const auto g = static_cast<float>(fadeLeft) / static_cast<float>(fadeLength_);
                    y = (y * (1.0f - g)) + (sample(oldHead) * g);
                    oldHead = wrap(oldHead + oldVelocity, length);
                    --fadeLeft;
                }
                out[i] = y;
            }
            emit(out_, o, out);
            if (o + 1 == out_.channels) {
                last_ = previous;
                oldHead_ = oldHead;
                oldVelocity_ = oldVelocity;
                velocity_ = velocity;
                fadeLeft_ = fadeLeft;
            }
        }
        if (out_.channels == 0 && count > 0) last_ = pos[count - 1];
    }

private:
    // Fills @p pos with the block's read heads, in frames on the loop. An
    // audio position gives one per sample. A control or static position
    // gives one per block, the head at the block's end, as a writer's
    // :position is: the heads move up to it along the shorter way around
    // the loop, sample i at i/n of the way, so a reader at a writer's
    // position minus a delay stays that delay behind at every sample, and
    // a writer's head wrapping to the start does not sweep back through
    // the buffer. A move of more than two blocks is a jump, which the
    // heads take at their last speed so they arrive on time.
    void headsFor(std::span<double> pos, int n, double length) noexcept {
        if (position_.isAudio()) {
            const auto x = position_.block(0, n);
            for (std::size_t i = 0; i < pos.size(); ++i)
                pos[i] = wrap(static_cast<double>(x[i]) * sampleRate_, length);
            return;
        }
        const double target = wrap(static_cast<double>(position_.value(0)) * sampleRate_, length);
        const double from = lastTarget_;
        lastTarget_ = target;
        const bool first = !std::isfinite(from);
        const double move = first ? 0.0 : around(from, target, length);
        const bool jump = first || std::abs(move) > 2.0 * n;
        for (std::size_t i = 0; i < pos.size(); ++i) {
            const auto k = static_cast<double>(i);
            if (first) {
                pos[i] = target;
            } else if (jump) {
                pos[i] = wrap(target - ((static_cast<double>(n) - k) * velocity_), length);
            } else {
                pos[i] = wrap(from + (move * k / n), length);
            }
        }
    }

    std::shared_ptr<const sampleBuffer> buffer_;
    audioOutput out_;
    signal position_;
    interpolation mode_ = interpolation::cubic;
    double sampleRate_ = 48000.0;
    int fadeLength_ = 1;
    std::vector<double> positions_;
    blockBuffer scratch_;
    double last_ = std::nan("");        // the last head, in frames; NaN before the first block
    double lastTarget_ = std::nan("");  // the last block-rate position, in frames
    double velocity_ = 0.0;             // frames per sample
    int fadeLeft_ = 0;
    double oldHead_ = 0.0;
    double oldVelocity_ = 0.0;
};

}  // namespace

std::span<const factoryEntry> bufferFactories() {
    static const auto factories = std::to_array<factoryEntry>({
        {.type = "bufferWrite", .make = make<bufferWrite>},
        {.type = "bufferRead", .make = make<bufferRead>},
    });
    return factories;
}

}  // namespace primitives
