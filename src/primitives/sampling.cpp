// Buffer primitives: bufferWrite records into a buffer, bufferRead reads
// it at any position (samples.md §6), and samplePlayer plays it from a
// start position at each trigger (§7). Both bind to their buffer
// when built; the plan holds it, so the audio thread never frees it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <espresso/primitives/buffer.h>
#include <espresso/primitives/event.h>
#include <espresso/primitives/primitive.h>
#include <espresso/primitives/random.h>
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

// What interpolation reads past a buffer's ends.
enum class edges : std::uint8_t {
    wrap,    // the other end: a ring
    silent,  // silence: a sample
};

// Channel @p c of @p b at fractional frame @p pos, which is in [0, frames).
float readAt(const sampleBuffer& b, int c, double pos, interpolation mode,
             edges past = edges::wrap) noexcept ESPRESSO_NONBLOCKING {
    const auto line = b.channel(c);
    const auto size = static_cast<std::ptrdiff_t>(line.size());
    const auto i = static_cast<std::ptrdiff_t>(pos);
    const auto frac = static_cast<float>(pos - static_cast<double>(i));
    // Frame k, for k from i - 1 to i + 2.
    const auto at = [&](std::ptrdiff_t k) {
        if (k >= 0 && k < size) return line[static_cast<std::size_t>(k)];
        if (past == edges::silent) return 0.0f;
        return line[static_cast<std::size_t>((k + size) % size)];
    };
    switch (mode) {
    case interpolation::none: return at(std::lround(pos));
    case interpolation::linear: return at(i) + (frac * (at(i + 1) - at(i)));
    case interpolation::cubic: {  // 4-point Hermite, as the delay line's
        const float xm1 = at(i - 1);
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

// Time parameter @p name in seconds, as a double when it is static: a
// float, which a signal holds, would put a start or a position a fraction
// of a frame off. Nothing when it follows a pipe.
std::optional<double> staticSeconds(const nodeContext& c, const signal& s, std::string_view name) {
    return s.isStatic() ? std::optional(c.number(name)) : std::nullopt;
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
          fixed_(staticSeconds(c, position_, "position")),
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
        const double target =
            wrap((fixed_ ? *fixed_ : static_cast<double>(position_.value(0))) * sampleRate_, length);
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
    std::optional<double> fixed_;  // a static position
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

// --- samplePlayer (§7) ------------------------------------------------------

constexpr double retriggerFade = 0.002;  // seconds

enum class loopMode : std::uint8_t { off, forward, pingPong };

class samplePlayer final : public nodeBase {
public:
    explicit samplePlayer(nodeContext& c)
        : nodeBase(c),
          buffer_(c.buffer("buffer")),
          trigger_(c.eventIn("trigger")),
          out_(c.audioOut("out")),
          done_(c.eventOut("done")),
          rate_(c.param("rate")),
          start_(c.param("start")),
          loopStart_(c.param("loop-start")),
          loopEnd_(c.param("loop-end")),
          startAt_(staticSeconds(c, start_, "start")),
          loopStartAt_(staticSeconds(c, loopStart_, "loop-start")),
          loopEndAt_(staticSeconds(c, loopEnd_, "loop-end")),
          root_(c.given("root") ? std::optional(c.number("root")) : std::nullopt),
          loop_(static_cast<loopMode>(c.choice("loop"))),
          mode_(static_cast<interpolation>(c.choice("interpolation"))),
          sampleRate_(c.sampleRate()),
          fadeLength_(std::max(1, static_cast<int>(std::lround(retriggerFade * c.sampleRate())))),
          heads_(static_cast<std::size_t>(std::max(c.blockSize(), 1))),
          oldHeads_(heads_.size()),
          oldGains_(heads_.size()),
          scratch_(c.blockSize()) {}

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        auto& o = static_cast<samplePlayer&>(outgoing);
        const auto last = static_cast<double>(buffer_->frames() - 1);
        if (std::isfinite(o.head_) && std::isfinite(o.noteRate_) && std::isfinite(o.direction_)) {
            playing_ = o.playing_;
            head_ = std::fmin(std::fmax(o.head_, 0.0), last);
            direction_ = o.direction_;
            noteRate_ = o.noteRate_;
            looped_ = o.looped_;
            doneNext_ = o.doneNext_;
        }
        if (o.fadeLeft_ > 0 && std::isfinite(o.oldHead_) && std::isfinite(o.oldStep_)) {
            fadeLeft_ = o.fadeLeft_;
            oldHead_ = std::fmin(std::fmax(o.oldHead_, 0.0), last);
            oldStep_ = o.oldStep_;
        }
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        const int n = ctx.numSamples;
        const auto count = static_cast<std::size_t>(n);
        const sampleBuffer& b = *buffer_;
        const auto frames = static_cast<double>(b.frames());
        // The loop, in frames: loop-end 0 means the end of the buffer.
        const double end = std::fmin(std::fmax(seconds(loopEndAt_, loopEnd_) * sampleRate_, 0.0), frames);
        const double loopEnd = end > 0.0 ? end : frames;
        const double loopStart =
            std::fmin(std::fmax(seconds(loopStartAt_, loopStart_) * sampleRate_, 0.0), loopEnd - 1.0);
        const auto rate = rate_.block(0, n);
        const auto events = trigger_ ? trigger_->events() : std::span<const event>{};
        if (doneNext_ && done_) emit(*done_, trigger(0));
        doneNext_ = false;
        std::size_t next = 0;
        for (std::size_t i = 0; i < count; ++i) {
            for (; next < events.size() && events[next].sampleOffset <= i; ++next)
                start(events[next], frames);
            heads_[i] = playing_ ? head_ : notANumber;
            oldGains_[i] = 0.0f;
            if (fadeLeft_ > 0) {
                oldHeads_[i] = oldHead_;
                oldGains_[i] = static_cast<float>(fadeLeft_) / static_cast<float>(fadeLength_);
                oldHead_ += oldStep_;
                // The fade ends early when the old head runs off the buffer.
                fadeLeft_ = oldHead_ >= 0.0 && oldHead_ < frames ? fadeLeft_ - 1 : 0;
            }
            if (!playing_) continue;
            // fmin and fmax keep a NaN rate from reaching the head.
            const double step =
                std::fmin(std::fmax(static_cast<double>(rate[i]), -1e3), 1e3) * noteRate_ * direction_;
            step_ = step;
            if (!advance(step, loopStart, loopEnd, frames)) {
                // :done falls on the first silent sample, which may be the next block's first.
                if (i + 1 < count && done_) emit(*done_, trigger(static_cast<std::uint32_t>(i + 1)));
                doneNext_ = i + 1 == count;
            }
        }
        const int channels = b.channels();
        const auto out = scratch_.first(count);
        for (int o = 0; o < out_.channels; ++o) {
            const auto sample = [&](double at) {
                return mapChannel(o, out_.channels, channels,
                                  [&](int c) { return readAt(b, c, at, mode_, edges::silent); });
            };
            for (std::size_t i = 0; i < count; ++i) {
                const float g = oldGains_[i];
                const float now = std::isnan(heads_[i]) ? 0.0f : sample(heads_[i]);
                out[i] = g > 0.0f ? (now * (1.0f - g)) + (sample(oldHeads_[i]) * g) : now;
            }
            emit(out_, o, out);
        }
    }

private:
    // A time parameter's value in seconds: static, or the block's value.
    static double seconds(const std::optional<double>& fixed, const signal& s) noexcept ESPRESSO_NONBLOCKING {
        return fixed ? *fixed : static_cast<double>(s.value(0));
    }

    // Starts playback at event @p e: a trigger, a value event, or a note-on.
    void start(const event& e, double frames) noexcept ESPRESSO_NONBLOCKING {
        const bool noteOn = e.type == eventType::noteOn && data2(e) > 0;
        if (!noteOn && e.type != eventType::trigger && e.type != eventType::value) return;
        if (playing_) {
            // The old head carries on and fades out over 2 ms (§7.2).
            oldHead_ = head_;
            oldStep_ = step_;
            fadeLeft_ = fadeLength_;
        }
        playing_ = true;
        looped_ = false;
        direction_ = 1.0;
        head_ = std::fmin(std::fmax(seconds(startAt_, start_) * sampleRate_, 0.0), frames - 1.0);
        noteRate_ = noteOn && root_ ? std::exp2((data1(e) - *root_) / 12.0) : 1.0;
    }

    // Moves the head by @p step, looping or stopping at the ends. False
    // when it ran off the buffer and playback stopped.
    bool advance(double step, double loopStart, double loopEnd, double frames) noexcept ESPRESSO_NONBLOCKING {
        head_ += step;
        const double length = loopEnd - loopStart;
        if (loop_ != loopMode::off && length > 0.0) {
            // Ping-pong turns at the loop's first and last frames, playing
            // neither twice; forward wraps by the loop's length.
            const double last = loopEnd - 1.0;
            if (head_ >= loopStart && head_ < loopEnd) looped_ = true;
            if (loop_ == loopMode::forward ? head_ >= loopEnd : head_ > last) {
                if (loop_ == loopMode::forward) {
                    head_ = loopStart + std::fmod(head_ - loopStart, length);
                } else {
                    head_ = std::fmax((2.0 * last) - head_, loopStart);
                    direction_ = -direction_;
                }
                looped_ = true;
                return true;
            }
            if (head_ < loopStart && looped_) {
                if (loop_ == loopMode::forward) {
                    head_ = loopEnd - std::fmod(loopStart - head_, length);
                } else {
                    head_ = std::fmin((2.0 * loopStart) - head_, last);
                    direction_ = -direction_;
                }
                return true;
            }
        }
        if (head_ >= frames || head_ < 0.0) {
            playing_ = false;
            return false;
        }
        return true;
    }

    std::shared_ptr<const sampleBuffer> buffer_;
    std::shared_ptr<const ePipe> trigger_;
    audioOutput out_;
    std::shared_ptr<ePipe> done_;
    signal rate_;
    signal start_;
    signal loopStart_;
    signal loopEnd_;
    std::optional<double> startAt_;  // static times, exact
    std::optional<double> loopStartAt_;
    std::optional<double> loopEndAt_;
    std::optional<double> root_;
    loopMode loop_ = loopMode::off;
    interpolation mode_ = interpolation::cubic;
    double sampleRate_ = 48000.0;
    int fadeLength_ = 1;
    std::vector<double> heads_;     // per sample; NaN where nothing plays
    std::vector<double> oldHeads_;  // per sample, while the old head fades
    std::vector<float> oldGains_;
    blockBuffer scratch_;
    bool playing_ = false;
    bool doneNext_ = false;  // :done is due at the next block's first sample
    bool looped_ = false;    // the head has entered the loop since it started
    double head_ = 0.0;
    double step_ = 0.0;
    double direction_ = 1.0;
    double noteRate_ = 1.0;
    int fadeLeft_ = 0;
    double oldHead_ = 0.0;
    double oldStep_ = 0.0;

    static constexpr double notANumber = std::numeric_limits<double>::quiet_NaN();
};

// --- grains (§8) ------------------------------------------------------------

constexpr double stealFade = 0.002;  // seconds

enum class grainWindow : std::uint8_t { hann, tukey, triangle };

// The window at @p x, from 0 to 1 over a grain.
float windowAt(grainWindow w, double x) noexcept ESPRESSO_NONBLOCKING {
    switch (w) {
    case grainWindow::hann: return static_cast<float>(0.5 - (0.5 * std::cos(2.0 * std::numbers::pi * x)));
    case grainWindow::triangle: return static_cast<float>(1.0 - std::abs((2.0 * x) - 1.0));
    case grainWindow::tukey: {  // flat in the middle half, cosine tapers at the ends
        constexpr double taper = 0.25;
        const double edge = std::fmin(x, 1.0 - x);
        if (edge >= taper) return 1.0f;
        return static_cast<float>(0.5 - (0.5 * std::cos(std::numbers::pi * edge / taper)));
    }
    }
    return 0.0f;
}

// One grain: where it reads, how fast, how long, and where it sits.
struct grain {
    bool active = false;
    double head = 0.0;  // frames
    double step = 1.0;  // frames per sample; negative backward
    std::int64_t age = 0;
    std::int64_t length = 1;
    float left = 1.0f;  // pan gains
    float right = 1.0f;
    std::uint64_t order = 0;  // when it started, for stealing the oldest
};

// A stolen grain fading out over 2 ms in its voice, its window held.
struct tail {
    int left = 0;  // samples of the fade remaining
    double head = 0.0;
    double step = 1.0;
    float gainLeft = 0.0f;
    float gainRight = 0.0f;
};

class grains final : public nodeBase {
public:
    explicit grains(nodeContext& c)
        : nodeBase(c),
          buffer_(c.buffer("buffer")),
          trigger_(c.eventIn("trigger")),
          out_(c.audioOut("out")),
          density_(c.param("density")),
          duration_(c.param("duration")),
          position_(c.param("position")),
          fixed_(staticSeconds(c, position_, "position")),
          jitter_(c.param("jitter")),
          pitch_(c.param("pitch")),
          pitchJitter_(c.param("pitch-jitter")),
          reverse_(c.param("reverse")),
          spread_(c.param("spread")),
          window_(static_cast<grainWindow>(c.choice("window"))),
          sampleRate_(c.sampleRate()),
          fadeLength_(std::max(1, static_cast<int>(std::lround(stealFade * c.sampleRate())))),
          voices_(static_cast<std::size_t>(c.number("voices"))),
          tails_(voices_.size()),
          random_(c.seed()),
          heads_(static_cast<std::size_t>(std::max(c.blockSize(), 1))),
          left_(c.blockSize()),
          right_(c.blockSize()) {}

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        auto& o = static_cast<grains&>(outgoing);
        const auto length = static_cast<double>(buffer_->frames());
        const std::size_t n = std::min(voices_.size(), o.voices_.size());
        for (std::size_t v = 0; v < n; ++v) {
            if (!std::isfinite(o.voices_[v].head) || !std::isfinite(o.voices_[v].step)) continue;
            voices_[v] = o.voices_[v];
            voices_[v].head = wrap(voices_[v].head, length);
        }
        adoptFinite(phase_, o.phase_);
        if (std::isfinite(o.lastTarget_)) lastTarget_ = wrap(o.lastTarget_, length);
        started_ = o.started_;
        random_ = o.random_;
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        const int n = ctx.numSamples;
        const auto count = static_cast<std::size_t>(n);
        const sampleBuffer& b = *buffer_;
        const auto length = static_cast<double>(b.frames());
        const auto heads = std::span(heads_).first(count);
        headsFor(heads, n, length);
        const auto density = density_.block(0, n);
        const auto duration = duration_.block(0, n);
        const auto jitter = jitter_.block(0, n);
        const auto pitch = pitch_.block(0, n);
        const auto pitchJitter = pitchJitter_.block(0, n);
        const auto reverse = reverse_.block(0, n);
        const auto spread = spread_.block(0, n);
        const auto events = trigger_ ? trigger_->events() : std::span<const event>{};
        const bool stereo = out_.channels >= 2;
        const int channels = b.channels();
        const auto mono = [&](double at) {
            return mapChannel(0, 1, channels, [&](int c) { return readAt(b, c, at, interpolation::cubic); });
        };
        std::size_t next = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto begin = [&] {
                start({.head = heads[i],
                       .duration = duration[i],
                       .jitter = jitter[i],
                       .pitch = pitch[i],
                       .pitchJitter = pitchJitter[i],
                       .reverse = reverse[i],
                       .spread = stereo ? spread[i] : 0.0f},
                      length);
            };
            for (; next < events.size() && events[next].sampleOffset <= i; ++next) begin();
            // At most 1000 grains a second, so at most one a sample; a
            // density of 0 (or NaN) starts none, leaving them to :trigger.
            const double rate = std::fmin(static_cast<double>(density[i]), 1000.0);
            if (rate > 0.0) {
                phase_ += rate / sampleRate_;
                if (phase_ >= 1.0) {
                    phase_ -= std::floor(phase_);
                    begin();
                }
            }
            float l = 0.0f;
            float r = 0.0f;
            for (std::size_t v = 0; v < voices_.size(); ++v) {
                tail& t = tails_[v];
                if (t.left > 0) {
                    const float x =
                        mono(t.head) * static_cast<float>(t.left) / static_cast<float>(fadeLength_);
                    l += x * t.gainLeft;
                    r += x * t.gainRight;
                    t.head = wrap(t.head + t.step, length);
                    --t.left;
                }
                grain& g = voices_[v];
                if (!g.active) continue;
                const float x = mono(g.head) *
                                windowAt(window_, static_cast<double>(g.age) / static_cast<double>(g.length));
                l += x * g.left;
                r += x * g.right;
                g.head = wrap(g.head + g.step, length);
                if (++g.age >= g.length) g.active = false;
            }
            left_[i] = l;
            right_[i] = r;
        }
        if (stereo) {
            emit(out_, 0, left_.first(count));
            emit(out_, 1, right_.first(count));
        } else {
            emit(out_, 0, left_.first(count));  // a mono grain has a gain of 1 on the left
        }
    }

private:
    // What a grain reads when it starts (§8.2).
    struct grainStart {
        double head = 0.0;  // frames
        float duration = 0.1f;
        float jitter = 0.0f;
        float pitch = 0.0f;
        float pitchJitter = 0.0f;
        float reverse = 0.0f;
        float spread = 0.0f;
    };

    // Starts a grain in a free voice, or in the oldest, which fades out.
    void start(const grainStart& s, double length) noexcept ESPRESSO_NONBLOCKING {
        std::size_t voice = voices_.size();
        std::size_t oldest = 0;
        for (std::size_t v = 0; v < voices_.size(); ++v) {
            if (!voices_[v].active) {
                voice = v;
                break;
            }
            if (voices_[v].order < voices_[oldest].order) oldest = v;
        }
        if (voice == voices_.size()) {
            voice = oldest;
            const grain& g = voices_[voice];
            const float w = windowAt(window_, static_cast<double>(g.age) / static_cast<double>(g.length));
            tails_[voice] = {.left = fadeLength_,
                             .head = g.head,
                             .step = g.step,
                             .gainLeft = w * g.left,
                             .gainRight = w * g.right};
        }
        // Clamped as the table bounds them; fmin and fmax pass no NaN.
        const double jitter = std::fmin(std::fmax(static_cast<double>(s.jitter), 0.0), 3600.0);
        const double semitones = std::fmin(std::fmax(static_cast<double>(s.pitch), -48.0), 48.0) +
                                 (std::fmin(std::fmax(static_cast<double>(s.pitchJitter), 0.0), 48.0) *
                                  static_cast<double>(random_.bipolar()));
        const bool backward = random_.uniform() < s.reverse;
        const double pan = std::fmin(std::fmax(static_cast<double>(s.spread), 0.0), 1.0) *
                           static_cast<double>(random_.bipolar());
        const double offset = jitter * sampleRate_ * static_cast<double>(random_.uniform());
        const double seconds = std::fmin(std::fmax(static_cast<double>(s.duration), 0.001), 10.0);
        const double angle = (pan + 1.0) * std::numbers::pi / 4.0;  // equal power
        const bool stereo = out_.channels >= 2;
        voices_[voice] = {.active = true,
                          .head = wrap(s.head + offset, length),
                          .step = std::exp2(semitones / 12.0) * (backward ? -1.0 : 1.0),
                          .age = 0,
                          .length = std::max<std::int64_t>(1, std::llround(seconds * sampleRate_)),
                          .left = stereo ? static_cast<float>(std::cos(angle)) : 1.0f,
                          .right = stereo ? static_cast<float>(std::sin(angle)) : 0.0f,
                          .order = started_++};
    }

    // The block's read positions for new grains, in frames: per sample
    // from an audio position; from a control or static one, moving to the
    // block's value along the shorter way around the buffer, as
    // bufferRead's heads do, or at once when it jumps.
    void headsFor(std::span<double> heads, int n, double length) noexcept ESPRESSO_NONBLOCKING {
        if (position_.isAudio()) {
            const auto x = position_.block(0, n);
            for (std::size_t i = 0; i < heads.size(); ++i) {
                heads[i] = wrap(static_cast<double>(x[i]) * sampleRate_, length);
            }
            return;
        }
        const double target =
            wrap((fixed_ ? *fixed_ : static_cast<double>(position_.value(0))) * sampleRate_, length);
        const double from = lastTarget_;
        lastTarget_ = target;
        const double move = std::isfinite(from) ? around(from, target, length) : 0.0;
        const bool jump = !std::isfinite(from) || std::abs(move) > 2.0 * n;
        for (std::size_t i = 0; i < heads.size(); ++i) {
            heads[i] = jump ? target : wrap(from + (move * static_cast<double>(i) / n), length);
        }
    }

    std::shared_ptr<const sampleBuffer> buffer_;
    std::shared_ptr<const ePipe> trigger_;
    audioOutput out_;
    signal density_;
    signal duration_;
    signal position_;
    std::optional<double> fixed_;
    signal jitter_;
    signal pitch_;
    signal pitchJitter_;
    signal reverse_;
    signal spread_;
    grainWindow window_ = grainWindow::hann;
    double sampleRate_ = 48000.0;
    int fadeLength_ = 1;
    std::vector<grain> voices_;
    std::vector<tail> tails_;
    rng random_;
    std::vector<double> heads_;
    blockBuffer left_;
    blockBuffer right_;
    double phase_ = 1.0;  // the first grain starts at once
    double lastTarget_ = std::nan("");
    std::uint64_t started_ = 0;
};

}  // namespace

std::span<const factoryEntry> bufferFactories() {
    static const auto factories = std::to_array<factoryEntry>({
        {.type = "bufferWrite", .make = make<bufferWrite>},
        {.type = "bufferRead", .make = make<bufferRead>},
        {.type = "samplePlayer", .make = make<samplePlayer>},
        {.type = "grains", .make = make<grains>},
    });
    return factories;
}

}  // namespace primitives
