/// @file
/// The engine: per-block execution, plan swaps with state adoption and
/// crossfades, quantization, the beat clock, and counters
/// (`primitives.md` §7.5, §9, §10, §11).

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <proj/metadata/graph.h>
#include <proj/primitives/io.h>
#include <proj/primitives/pipeTable.h>
#include <proj/primitives/plan.h>
#include <proj/primitives/primitive.h>
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

/// Engine construction settings.
struct engineOptions {
    /// Sample rate and block size.
    prepareInfo audio;
    /// Pipe table capacity.
    pipeTable::capacity pipes;
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
class engine {
public:
    /// Allocates the pipe table and queues.
    explicit engine(engineOptions options = {});
    /// Frees every plan. The audio thread must have stopped.
    ~engine();

    engine(const engine&) = delete;
    engine& operator=(const engine&) = delete;

    /// Control thread: builds a plan. Throws buildError.
    std::unique_ptr<plan> build(const metadata::graphSpec& graph, std::int32_t planId);

    /// Control thread: swaps to @p next after @p fade seconds of
    /// crossfade, at the position given by @p quant. Replaces a swap that
    /// is still waiting for its quantization target.
    void play(std::unique_ptr<plan> next, double fade, quantization quant);

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
    /// the last call, in order.
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
    void process(const deviceBuffers& device, int numSamples, double hostTime) noexcept;

private:
    struct controlWrite {
        std::int32_t index = 0;
        float value = 0.0f;
    };

    void runBlock(const deviceBuffers& device, int offset, int n, double hostTime) noexcept;
    void swapIn(transition* t) noexcept;
    void finishFade() noexcept;
    void retire(transition* t) noexcept;
    static void discard(transition* t, std::vector<notice>& out);
    double quantizeTarget(quantization quant) const noexcept;

    engineOptions options_;
    pipeTable pipes_;
    ioHub io_;

    // Control thread to audio thread. The target beat is written before
    // the transition is published; NaN swaps at the next block.
    std::atomic<transition*> pending_{nullptr};
    std::atomic<double> pendingTarget_{0.0};
    std::atomic<double> pendingGrid_{0.0};  // beats between grid points; 0 without quantization
    spscQueue<controlWrite> controls_{4096};
    // Audio thread to control thread.
    spscQueue<transition*> retired_{1024};
    spscQueue<notice> notices_{1024};

    // Control thread only: the plan the audio thread runs once the last
    // published transition is taken.
    plan* believedActive_ = nullptr;
    plan* publishedIncoming_ = nullptr;
    bool published_ = false;
    std::vector<notice> discarded_;  // plans withdrawn before they ran

    // Audio thread only.
    plan* active_ = nullptr;
    transition* fading_ = nullptr;
    std::int64_t sampleClock_ = 0;
    double beatPosition_ = 0.0;
    bool waitedForFade_ = false;  // the pending swap waited for a fade

    // Shared counters.
    std::atomic<double> beat_{0.0};
    std::atomic<int> meter_{4};
    std::atomic<std::int32_t> activePlan_{0};
    std::atomic<std::uint64_t> missed_{0};
    std::atomic<std::uint64_t> controlDrops_{0};
    std::atomic<std::uint64_t> leaked_{0};
    std::atomic<float> load_{0.0f};
};

} // namespace primitives
