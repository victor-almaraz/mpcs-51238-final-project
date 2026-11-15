/// @file
/// The engine: per-block execution, plan swaps with state adoption and
/// crossfades, quantization, the beat clock, and counters
/// (`primitives.md` §7.5, §9, §10, §11).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <proj/metadata/graph.h>
#include <proj/primitives/frame.h>
#include <proj/primitives/io.h>
#include <proj/primitives/pipeTable.h>
#include <proj/primitives/plan.h>
#include <proj/primitives/primitive.h>
#include <proj/primitives/realtime.h>
#include <proj/primitives/spscQueue.h>

namespace primitives {

/// Where a swap lands (§9.4).
enum class quantization : std::uint8_t {
    none,  ///< At the next block
    beat,  ///< At the block containing the next beat
    bar,   ///< At the block containing the next bar line
};

/// The quantization named `none`, `beat`, or `bar`.
std::optional<quantization> parseQuantization(std::string_view name) noexcept;

/// Longest crossfade, in seconds. engine::play() shortens longer fades.
inline constexpr double maxFade = 3600.0;

/// Engine construction settings.
struct engineOptions {
    /// Sample rate and block size.
    prepareInfo audio;
    /// Pipe table capacity.
    pipeTable::capacity pipes;
    /// Decodes image files for `imagePlayer` on the builder thread
    /// (`primitives` does no file I/O, §1.2). Empty: images cannot load.
    imageLoader loadImage = nullptr;
};

/// Diagnostics (§11.1).
struct engineCounters {
    /// Blocks whose processing exceeded their period.
    std::uint64_t missedDeadlines = 0;
    /// Packets and control values dropped by full queues.
    std::uint64_t drops = 0;
    /// Event pipe overflows.
    std::uint64_t overflows = 0;
    /// Audio thread load, from 0 to 1, smoothed.
    float load = 0.0f;
    /// The active plan, or 0.
    std::int32_t activePlan = 0;
    /// Signal faults reported by sinks, such as non-finite samples.
    std::uint64_t faults = 0;
};

/// A plan state change, reported to the control thread.
struct notice {
    /// What happened.
    enum class kind : std::uint8_t {
        activated,  ///< The plan became active (`/plan.active`)
        retired,    ///< The plan is gone; its pipe indices are free (`/plan.retired`)
    };
    /// What happened.
    kind what = kind::activated;
    /// The plan.
    std::int32_t plan = 0;
};

struct transition;

/// Runs plans. Control-thread methods and audio-thread methods are
/// marked; they may run concurrently with each other. Offline renders
/// call both from one thread.
// The queues' cache-line alignment pads the class; their head and tail
// lines stay separate (spscQueue).
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding): cache-line separation
class engine {
public:
    /// Allocates the pipe table and queues. Throws std::invalid_argument
    /// when the block size is not positive or the sample rate is not a
    /// positive finite number.
    explicit engine(engineOptions options = {});
    /// Frees every plan. The audio thread must have stopped.
    ~engine();

    engine(const engine&) = delete;
    engine& operator=(const engine&) = delete;

    /// Control thread: builds a plan. Throws buildError.
    std::shared_ptr<plan> build(const metadata::graphSpec& graph, std::int32_t planId);

    /// Control thread: swaps to @p next after @p fade seconds of
    /// crossfade, at the position given by @p quant. Replaces a swap that
    /// is still waiting for its quantization target. A negative or
    /// non-finite fade is 0; a fade longer than maxFade is maxFade. Plan
    /// IDs need not be unique: the engine tells swaps apart by call.
    void play(std::shared_ptr<plan> next, double fade, quantization quant);

    /// Control thread: fades out the active plan over @p fade seconds.
    void stop(double fade);

    /// Control thread: sets every channel of control pipe @p index at the
    /// next block. Returns false when the index is out of range, the value
    /// is not finite, or the queue is full.
    bool setControl(std::int32_t index, float value);

    /// Any thread: channel 0 of control pipe @p index, or 0 when out of
    /// range.
    float control(std::int32_t index) const;

    /// Any thread: the counters.
    engineCounters counters() const;

    /// Control thread: frees retired plans and returns what changed since
    /// the last call, in order: a plan is activated before it retires,
    /// and plans activate in the order they were played.
    std::vector<notice> collect();

    /// The I/O ports of sources and sinks.
    ioHub& io() noexcept { return io_; }

    /// Sample rate and block size.
    const prepareInfo& info() const noexcept { return options_.audio; }

    /// The pipes, for inspection between blocks (tests and offline
    /// renders). Not safe while the audio thread runs.
    const pipeTable& pipes() const noexcept { return pipes_; }

    /// Any thread: beat position at the end of the last block.
    double beat() const noexcept { return beat_.load(std::memory_order_relaxed); }

    /// Audio thread: runs @p numSamples frames, in blocks of at most
    /// `info().blockSize`. @p hostTime is the device clock at the first
    /// frame, in seconds.
    void process(const deviceBuffers& device, int numSamples, double hostTime) noexcept PROJ_NONBLOCKING;

    /// Render thread: takes the latest active plan's video part, runs its
    /// video nodes once (§3), and returns the frame its visual layer
    /// shows, if it has one. @p time is the frame's time, in seconds.
    /// Plans reach the render thread only when the engine has video pipes
    /// (`pipeTable::capacity::video`).
    metadata::optionalRef<frame> renderFrame(double time) noexcept PROJ_NONBLOCKING;

private:
    struct controlWrite {
        std::int32_t index = 0;
        float value = 0.0f;
    };

    // A played swap that has neither activated nor been withdrawn. Its
    // plan reference keeps the incoming plan alive on the control thread.
    struct flight {
        std::uint64_t seq = 0;
        std::shared_ptr<plan> incoming;  // null for stop()
    };

    // Most plans the control thread has sent to the render thread and not
    // yet had back, counting the one the render thread shows; both video
    // queues hold this many, so the render thread's push cannot fail.
    static constexpr std::size_t maxVideoOutstanding = 8;

    // Most transitions outside the control thread at once: in submitted_,
    // pending, fading, or in retired_. Every queue from which the audio
    // thread or the control thread must not fail to push holds this many,
    // so those pushes cannot fail (see engine.cpp).
    static constexpr std::size_t maxOutstanding = 64;

    void runBlock(const deviceBuffers& device, int offset, int n, double hostTime) noexcept PROJ_NONBLOCKING;
    bool due(transition& t, double beatEnd) const noexcept PROJ_NONBLOCKING;
    void swapIn(std::unique_ptr<transition>&& t) noexcept PROJ_NONBLOCKING;
    void finishFade() noexcept PROJ_NONBLOCKING;
    void handBack(std::unique_ptr<transition>&& t) noexcept PROJ_NONBLOCKING;
    void absorb();
    void activate(std::uint64_t seq);
    void sendVideo();
    void settle(std::unique_ptr<transition> t);
    void withdraw(const transition& t);
    void submit(std::unique_ptr<transition> t);
    bool send(std::unique_ptr<transition>& t);
    std::unique_ptr<transition> transitionTo(std::shared_ptr<plan> next, double fade, quantization quant,
                                             std::uint64_t seq) const;
    double grid(quantization quant) const noexcept;

    // Declared first, destroyed last: plans and their primitives refer to
    // the pipes and ports these own.
    engineOptions options_;
    pipeTable pipes_;
    ioHub io_;

    // Swap protocol. Transitions move between the threads as unique_ptrs
    // through these queues, so each one has exactly one owner at a time
    // and is always freed on the control thread.
    // Sized in the constructor, where transition is a complete type.
    spscQueue<std::unique_ptr<transition>> submitted_;        // control to audio
    spscQueue<std::unique_ptr<transition>> retired_;          // audio to control
    spscQueue<std::uint64_t> activated_{maxOutstanding};      // audio to control: seq of each swap taken
    spscQueue<controlWrite> controls_{4096};                  // control to audio
    // Video handoff: the control thread sends each plan the audio thread
    // activates (null after a stop) to the render thread, which swaps it in
    // at its next frame and hands the previous one back to be freed.
    spscQueue<std::shared_ptr<plan>> videoSubmitted_{maxVideoOutstanding};  // control to render
    spscQueue<std::shared_ptr<plan>> videoRetired_{maxVideoOutstanding};    // render to control

    // Control thread only.
    std::shared_ptr<plan> confirmed_;     // the active plan, as the audio thread last reported
    std::vector<flight> inFlight_;        // in play order
    std::unique_ptr<transition> unsent_;  // waits for room to send
    std::vector<notice> reported_;        // notices for the next collect()
    std::uint64_t latestSeq_ = 0;         // the last play()
    std::size_t outstanding_ = 0;         // transitions sent and not yet back
    std::optional<std::shared_ptr<plan>>
        videoUnsent_;                   // the newest plan for the render thread, waiting for room
    std::size_t videoOutstanding_ = 1;  // the render thread's current plan, then each one sent

    // Audio thread only. Smart pointers here are only moved; the control
    // thread holds another reference to every plan, so none is freed here.
    std::shared_ptr<plan> active_;
    std::unique_ptr<transition> pending_;  // waiting for its quantization target or a fade
    std::unique_ptr<transition> fading_;
    std::int64_t sampleClock_ = 0;
    double beatPosition_ = 0.0;

    // Render thread only. Moved, never released: its old value goes back
    // to the control thread.
    std::shared_ptr<plan> videoActive_;

    // Shared counters.
    std::atomic<double> beat_{0.0};
    std::atomic<int> meter_{4};
    std::atomic<std::int32_t> activePlan_{0};
    std::atomic<std::uint64_t> missed_{0};
    std::atomic<std::uint64_t> controlDrops_{0};
    std::atomic<float> load_{0.0f};
};

} // namespace primitives
