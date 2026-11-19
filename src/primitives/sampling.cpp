// Buffer primitives: bufferWrite records into a buffer, bufferRead reads
// it at any position (samples.md §6), samplePlayer plays it from a start
// position at each trigger (§7), and grains plays grains of it (§8). Each
// binds to its buffer when built; the plan holds it, so the audio thread
// never frees it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <espresso/primitives/buffer.h>
#include <espresso/primitives/event.h>
#include <espresso/primitives/io.h>
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

// As wrap(), without its division when @p p is within a loop's length of
// it: a head that just stepped past an end. The result is the same.
double rewrap(double p, double length) noexcept ESPRESSO_NONBLOCKING {
    if (p >= length) {
        p -= length;
    } else if (p < 0.0) {
        p += length;
    }
    return p >= 0.0 && p < length ? p : wrap(p, length);
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
    case interpolation::cubic: return hermite(at(i - 1), at(i), at(i + 1), at(i + 2), frac);
    }
    return 0.0f;
}

// A time parameter: its signal, and its value as a double when it is
// static, since a float, which a signal holds, would put a start or a
// position a fraction of a frame off.
struct timeParam {
    timeParam(nodeContext& c, std::string_view name)
        : value(c.param(name)), fixed(value.isStatic() ? std::optional(c.number(name)) : std::nullopt) {}

    // In seconds: the static value, or the block's.
    double seconds() const noexcept ESPRESSO_NONBLOCKING {
        return fixed ? *fixed : static_cast<double>(value.value(0));
    }

    signal value;
    std::optional<double> fixed;
};

// The read heads a position parameter gives a block, in frames on a loop
// (§6.3). An audio position gives one per sample. A control or static
// position gives one per block, the head at the block's end, as a writer's
// :position is: the heads move up to it along the shorter way around the
// loop, sample i at i/n of the way, so a reader at a writer's position
// minus a delay stays that delay behind at every sample, and a writer's
// head wrapping to the start does not sweep back through the buffer. A
// move of more than two blocks is a jump, which the heads take at a given
// speed so they arrive on time.
class blockHeads {
public:
    blockHeads(nodeContext& c, std::string_view name) : position_(c, name) {}

    // Fills @p heads for a block of @p n samples on a loop of @p length
    // frames; a jump moves at @p jumpSpeed frames a sample.
    void fill(std::span<double> heads, int n, double length, double sampleRate,
              double jumpSpeed) noexcept ESPRESSO_NONBLOCKING {
        if (position_.value.isAudio()) {
            const auto x = position_.value.block(0, n);
            for (std::size_t i = 0; i < heads.size(); ++i)
                heads[i] = wrap(static_cast<double>(x[i]) * sampleRate, length);
            return;
        }
        const double target = wrap(position_.seconds() * sampleRate, length);
        const double from = lastTarget_;
        lastTarget_ = target;
        const bool first = !std::isfinite(from);
        const double move = first ? 0.0 : around(from, target, length);
        const bool jump = first || std::abs(move) > 2.0 * n;
        for (std::size_t i = 0; i < heads.size(); ++i) {
            const auto k = static_cast<double>(i);
            if (first) {
                heads[i] = target;
            } else if (jump) {
                heads[i] = wrap(target - ((static_cast<double>(n) - k) * jumpSpeed), length);
            } else {
                heads[i] = wrap(from + (move * k / n), length);
            }
        }
    }

    void adopt(const blockHeads& o, double length) noexcept ESPRESSO_NONBLOCKING {
        if (std::isfinite(o.lastTarget_)) lastTarget_ = wrap(o.lastTarget_, length);
    }

private:
    timeParam position_;
    double lastTarget_ = notANumber;  // the last block-rate position, in frames
};

// A primitive that plays a buffer at heads it moves itself: each sample
// the buffer at its head, silence where the head is NaN, crossfaded from an
// old head while that fades out (§6.4, §7.2).
class bufferReader : public nodeBase {
protected:
    explicit bufferReader(nodeContext& c)
        : nodeBase(c),
          buffer_(c.buffer("buffer")),
          out_(c.audioOut("out")),
          mode_(static_cast<interpolation>(c.choice("interpolation"))),
          heads_(static_cast<std::size_t>(std::max(c.blockSize(), 1))),
          oldHeads_(heads_.size()),
          oldGains_(heads_.size()),
          scratch_(c.blockSize()) {}

    // Writes the first @p count heads to the output, reading past the
    // buffer's ends as @p past says.
    void play(std::size_t count, edges past) noexcept ESPRESSO_NONBLOCKING {
        const sampleBuffer& b = *buffer_;
        const int channels = b.channels();
        const auto out = scratch_.first(count);
        // A mono buffer gives every output channel the same samples.
        const int distinct = channels == 1 ? std::min(out_.channels, 1) : out_.channels;
        for (int o = 0; o < distinct; ++o) {
            const auto sample = [&](double at) {
                return mapChannel(o, out_.channels, channels,
                                  [&](int c) { return readAt(b, c, at, mode_, past); });
            };
            for (std::size_t i = 0; i < count; ++i) {
                const float g = oldGains_[i];
                const float now = std::isnan(heads_[i]) ? 0.0f : sample(heads_[i]);
                out[i] = g > 0.0f ? (now * (1.0f - g)) + (sample(oldHeads_[i]) * g) : now;
            }
            if (channels == 1) {
                emitAll(out_, out);
            } else {
                emit(out_, o, out);
            }
        }
    }

    std::shared_ptr<const sampleBuffer> buffer_;
    audioOutput out_;
    interpolation mode_ = interpolation::cubic;
    std::vector<double> heads_;     // per sample; NaN where nothing plays
    std::vector<double> oldHeads_;  // per sample, while an old head fades
    std::vector<float> oldGains_;   // the old head's share, 0 when none
    blockBuffer scratch_;
};

// True for the events that start a sample or a save: a note-on, a
// trigger, or a value (samples.md §7.2, §18.1).
bool starts(const event& e) noexcept ESPRESSO_NONBLOCKING {
    return (e.type == eventType::noteOn && data2(e) > 0) || e.type == eventType::trigger ||
           e.type == eventType::value;
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

class bufferRead final : public bufferReader {
public:
    explicit bufferRead(nodeContext& c)
        : bufferReader(c),
          position_(c, "position"),
          sampleRate_(c.sampleRate()),
          fadeLength_(samplesFor(seamFade, c.sampleRate())) {}

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        auto& o = static_cast<bufferRead&>(outgoing);
        const auto length = static_cast<double>(buffer_->frames());
        if (std::isfinite(o.last_)) last_ = wrap(o.last_, length);
        position_.adopt(o.position_, length);
        adoptFinite(velocity_, o.velocity_);
        if (o.fadeLeft_ > 0 && allFinite(o.oldHead_, o.oldVelocity_)) {
            fadeLeft_ = o.fadeLeft_;
            oldHead_ = wrap(o.oldHead_, length);
            oldVelocity_ = o.oldVelocity_;
        }
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        const int n = ctx.numSamples;
        const auto count = static_cast<std::size_t>(n);
        const auto length = static_cast<double>(buffer_->frames());
        const auto heads = std::span(heads_).first(count);
        position_.fill(heads, n, length, sampleRate_, velocity_);
        for (std::size_t i = 0; i < count; ++i) {
            const double p = heads[i];
            if (std::isfinite(last_)) {
                const double step = around(last_, p, length);
                if (std::abs(step) > n) {
                    // A jump: the old head carries on and fades out (§6.4).
                    oldHead_ = wrap(last_ + velocity_, length);
                    oldVelocity_ = velocity_;
                    fadeLeft_ = fadeLength_;
                } else {
                    velocity_ = step;
                }
            }
            last_ = p;
            oldGains_[i] = 0.0f;
            if (fadeLeft_ > 0) {
                oldHeads_[i] = oldHead_;
                oldGains_[i] = static_cast<float>(fadeLeft_) / static_cast<float>(fadeLength_);
                oldHead_ = wrap(oldHead_ + oldVelocity_, length);
                --fadeLeft_;
            }
        }
        play(count, edges::wrap);
    }

private:
    blockHeads position_;
    double sampleRate_ = 48000.0;
    int fadeLength_ = 1;
    double last_ = notANumber;  // the last head, in frames; NaN before the first block
    double velocity_ = 0.0;     // frames per sample
    int fadeLeft_ = 0;
    double oldHead_ = 0.0;
    double oldVelocity_ = 0.0;
};

// --- samplePlayer (§7) ------------------------------------------------------

constexpr double retriggerFade = 0.002;  // seconds

enum class loopMode : std::uint8_t { off, forward, pingPong };

class samplePlayer final : public bufferReader {
public:
    explicit samplePlayer(nodeContext& c)
        : bufferReader(c),
          trigger_(c.eventIn("trigger")),
          done_(c.eventOut("done")),
          rate_(c.param("rate")),
          start_(c, "start"),
          loopStart_(c, "loop-start"),
          loopEnd_(c, "loop-end"),
          root_(c.given("root") ? std::optional(c.number("root")) : std::nullopt),
          loop_(static_cast<loopMode>(c.choice("loop"))),
          sampleRate_(c.sampleRate()),
          fadeLength_(samplesFor(retriggerFade, c.sampleRate())) {}

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        auto& o = static_cast<samplePlayer&>(outgoing);
        const auto last = static_cast<double>(buffer_->frames() - 1);
        if (allFinite(o.head_, o.noteRate_, o.direction_)) {
            playing_ = o.playing_;
            head_ = std::fmin(std::fmax(o.head_, 0.0), last);
            direction_ = o.direction_;
            noteRate_ = o.noteRate_;
            looped_ = o.looped_;
            doneNext_ = o.doneNext_;
        }
        if (o.fadeLeft_ > 0 && allFinite(o.oldHead_, o.oldStep_)) {
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
        const double end = std::fmin(std::fmax(loopEnd_.seconds() * sampleRate_, 0.0), frames);
        const double loopEnd = end > 0.0 ? end : frames;
        const double loopStart = std::fmin(std::fmax(loopStart_.seconds() * sampleRate_, 0.0), loopEnd - 1.0);
        const auto rate = rate_.block(0, n);
        const auto events = trigger_ ? trigger_->events() : std::span<const event>{};
        if (doneNext_ && done_) emit(*done_, trigger(0));
        doneNext_ = false;
        std::size_t next = 0;
        for (std::size_t i = 0; i < count; ++i) {
            for (; next < events.size() && events[next].sampleOffset <= i; ++next)
                start(events[next], frames);
            heads_[i] = playing_ ? head_ : static_cast<double>(notANumber);
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
            step_ = std::fmin(std::fmax(static_cast<double>(rate[i]), -1e3), 1e3) * noteRate_ * direction_;
            if (!advance(step_, loopStart, loopEnd, frames)) {
                // :done falls on the first silent sample, which may be the next block's first.
                if (i + 1 < count && done_) emit(*done_, trigger(static_cast<std::uint32_t>(i + 1)));
                doneNext_ = i + 1 == count;
            }
        }
        play(count, edges::silent);
    }

private:
    // Starts playback at event @p e: a trigger, a value event, or a note-on.
    void start(const event& e, double frames) noexcept ESPRESSO_NONBLOCKING {
        if (!starts(e)) return;
        if (playing_) {
            // The old head carries on and fades out over 2 ms (§7.2).
            oldHead_ = head_;
            oldStep_ = step_;
            fadeLeft_ = fadeLength_;
        }
        playing_ = true;
        looped_ = false;
        direction_ = 1.0;
        head_ = std::fmin(std::fmax(start_.seconds() * sampleRate_, 0.0), frames - 1.0);
        noteRate_ = e.type == eventType::noteOn && root_ ? std::exp2((data1(e) - *root_) / 12.0) : 1.0;
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

    std::shared_ptr<const ePipe> trigger_;
    std::shared_ptr<ePipe> done_;
    signal rate_;
    timeParam start_;
    timeParam loopStart_;
    timeParam loopEnd_;
    std::optional<double> root_;
    loopMode loop_ = loopMode::off;
    double sampleRate_ = 48000.0;
    int fadeLength_ = 1;
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
};

// --- grains (§8) ------------------------------------------------------------

constexpr double stealFade = 0.002;  // seconds

enum class grainWindow : std::uint8_t { hann, tukey, triangle };

// The window at @p x, from 0 to 1 over a grain.
float windowAt(grainWindow w, double x) noexcept ESPRESSO_NONBLOCKING {
    switch (w) {
    case grainWindow::hann: return static_cast<float>(0.5 - (0.5 * std::cos(twoPi * x)));
    case grainWindow::triangle: return static_cast<float>(1.0 - std::abs((2.0 * x) - 1.0));
    case grainWindow::tukey: {  // flat in the middle half, cosine tapers at the ends
        constexpr double taper = 0.25;
        const double edge = std::fmin(x, 1.0 - x);
        if (edge >= taper) return 1.0f;
        return static_cast<float>(0.5 - (0.5 * std::cos(pi * edge / taper)));
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
          position_(c, "position"),
          jitter_(c.param("jitter")),
          pitch_(c.param("pitch")),
          pitchJitter_(c.param("pitch-jitter")),
          reverse_(c.param("reverse")),
          spread_(c.param("spread")),
          window_(static_cast<grainWindow>(c.choice("window"))),
          stereo_(out_.channels >= 2),
          sampleRate_(c.sampleRate()),
          fadeLength_(samplesFor(stealFade, c.sampleRate())),
          voices_(static_cast<std::size_t>(c.number("voices"))),
          tails_(voices_.size()),
          random_(c.seed()),
          starts_(static_cast<std::size_t>(std::max(c.blockSize(), 1))),
          left_(c.blockSize()),
          right_(c.blockSize()) {}

    void adoptState(primitive& outgoing) noexcept ESPRESSO_NONBLOCKING override {
        auto& o = static_cast<grains&>(outgoing);
        const auto length = static_cast<double>(buffer_->frames());
        const std::size_t n = std::min(voices_.size(), o.voices_.size());
        for (std::size_t v = 0; v < n; ++v) {
            if (!allFinite(o.voices_[v].head, o.voices_[v].step)) continue;
            voices_[v] = o.voices_[v];
            voices_[v].head = wrap(voices_[v].head, length);
        }
        adoptFinite(phase_, o.phase_);
        position_.adopt(o.position_, length);
        started_ = o.started_;
        random_ = o.random_;
    }

    void process(const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING override {
        const int n = ctx.numSamples;
        const auto count = static_cast<std::size_t>(n);
        const sampleBuffer& b = *buffer_;
        const auto length = static_cast<double>(b.frames());
        const auto heads = std::span(starts_).first(count);
        position_.fill(heads, n, length, sampleRate_, 0.0);  // a jump lands at once
        const auto density = density_.block(0, n);
        const auto duration = duration_.block(0, n);
        const auto jitter = jitter_.block(0, n);
        const auto pitch = pitch_.block(0, n);
        const auto pitchJitter = pitchJitter_.block(0, n);
        const auto reverse = reverse_.block(0, n);
        const auto spread = spread_.block(0, n);
        const auto events = trigger_ ? trigger_->events() : std::span<const event>{};
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
                       .spread = stereo_ ? spread[i] : 0.0f},
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
                    t.head = rewrap(t.head + t.step, length);
                    --t.left;
                }
                grain& g = voices_[v];
                if (!g.active) continue;
                const float x = mono(g.head) *
                                windowAt(window_, static_cast<double>(g.age) / static_cast<double>(g.length));
                l += x * g.left;
                r += x * g.right;
                g.head = rewrap(g.head + g.step, length);
                if (++g.age >= g.length) g.active = false;
            }
            left_[i] = l;
            right_[i] = r;
        }
        emit(out_, 0, left_.first(count));  // a mono grain has a gain of 1 on the left
        if (stereo_) emit(out_, 1, right_.first(count));
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
        const double angle = (pan + 1.0) * pi / 4.0;  // equal power
        voices_[voice] = {.active = true,
                          .head = wrap(s.head + offset, length),
                          .step = std::exp2(semitones / 12.0) * (backward ? -1.0 : 1.0),
                          .age = 0,
                          .length = std::max<std::int64_t>(1, std::llround(seconds * sampleRate_)),
                          .left = stereo_ ? static_cast<float>(std::cos(angle)) : 1.0f,
                          .right = stereo_ ? static_cast<float>(std::sin(angle)) : 0.0f,
                          .order = started_++};
    }

    std::shared_ptr<const sampleBuffer> buffer_;
    std::shared_ptr<const ePipe> trigger_;
    audioOutput out_;
    signal density_;
    signal duration_;
    blockHeads position_;
    signal jitter_;
    signal pitch_;
    signal pitchJitter_;
    signal reverse_;
    signal spread_;
    grainWindow window_ = grainWindow::hann;
    bool stereo_ = false;  // :out has two channels or more, which grains pan into
    double sampleRate_ = 48000.0;
    int fadeLength_ = 1;
    std::vector<grain> voices_;
    std::vector<tail> tails_;
    rng random_;
    std::vector<double> starts_;  // where a grain started at each sample would read
    blockBuffer left_;
    blockBuffer right_;
    double phase_ = 1.0;  // the first grain starts at once
    std::uint64_t started_ = 0;
};

// --- bufferSave (samples.md §18) -----------------------------------------------

// Writes its buffer to a file at each trigger. The audio thread does no
// file I/O: the node copies the buffer into its port's snapshot, at most
// four blocks' worth of frames a block, and the control thread writes the
// file. A trigger while a save is pending is dropped and counted.
class bufferSave final : public nodeBase {
public:
    explicit bufferSave(nodeContext& c)
        : nodeBase(c),
          buffer_(c.buffer("buffer")),
          trigger_(c.eventIn("trigger")),
          perBlock_(4 * static_cast<std::size_t>(c.blockSize())),
          io_(c.io()) {
        c.reserve(buffer_->bytes());  // the snapshot
        port_ = c.io().openSave(c.id(), c.bufferId("buffer"), c.text("file"), buffer_->channels(),
                                buffer_->frames(), c.sampleRate());
        c.keep(port_);
    }

    void process(const blockContext&) noexcept ESPRESSO_NONBLOCKING override {
        if (retiring()) return;  // a replaced node saves nothing
        if (trigger_) {
            for (const event& e : trigger_->events()) {
                if (!starts(e)) continue;
                if (port_->begin()) {  // false while this node copies, too
                    copying_ = true;
                    copied_ = 0;
                } else {
                    io_.countDrop();
                }
            }
        }
        if (!copying_) return;
        const std::size_t frames = buffer_->frames();
        const std::size_t n = std::min(perBlock_, frames - copied_);
        for (int c = 0; c < buffer_->channels(); ++c) {
            std::ranges::copy(buffer_->channel(c).subspan(copied_, n),
                              port_->channel(c).begin() + static_cast<std::ptrdiff_t>(copied_));
        }
        copied_ += n;
        if (copied_ == frames) {
            port_->finish();
            copying_ = false;
        }
    }

private:
    std::shared_ptr<const sampleBuffer> buffer_;
    std::shared_ptr<const ePipe> trigger_;
    std::size_t perBlock_;
    ioHub& io_;
    std::shared_ptr<savePort> port_;
    bool copying_ = false;
    std::size_t copied_ = 0;  // frames of the snapshot copied so far
};

}  // namespace

std::span<const factoryEntry> bufferFactories() {
    static const auto factories = std::to_array<factoryEntry>({
        {.type = "bufferWrite", .make = make<bufferWrite>},
        {.type = "bufferRead", .make = make<bufferRead>},
        {.type = "samplePlayer", .make = make<samplePlayer>},
        {.type = "grains", .make = make<grains>},
        {.type = "bufferSave", .make = make<bufferSave>},
    });
    return factories;
}

}  // namespace primitives
