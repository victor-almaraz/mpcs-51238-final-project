// Engine: block execution, plan swaps, crossfades, quantization, and the
// beat clock (primitives.md §7.5, §9, §10).
//
// Swap protocol. A transition moves between the threads as a unique_ptr
// through two lock-free queues, so it has exactly one owner at any time:
// the control thread builds it and pushes it to `submitted_`; the audio
// thread keeps it as `pending_` until its quantization target, then runs
// its fade, then pushes it to `retired_`; the control thread frees it.
// A newer transition replaces a pending one, which goes back unused.
//
// Each play() takes a sequence number, which identifies its swap in every
// message between the threads; plan IDs need not be unique. When the
// audio thread swaps a transition in, it queues the number to
// `activated_`.
//
// A transition is built against the plan the control thread last saw
// become active (`confirmed_`). If the audio thread activated another
// plan meanwhile, the transition is stale: it goes back, and the control
// thread rebuilds it against the plan now active, unless a newer play()
// has superseded it. No thread ever reads a transition it does not own.
//
// Queue sizing. `outstanding_` counts the transitions outside the control
// thread, and the control thread sends one only while that count is
// below maxOutstanding. Every transition in submitted_ or retired_ is
// outstanding, and every number in activated_ belongs to a transition
// that is outstanding or that the current absorb() has just taken back
// and is about to drain; so no push to these queues can fail.
//
// Plans and primitives are shared: transitions refer to the plans and
// nodes they cross between. The control thread holds a reference to every
// plan the audio thread may still use, so the audio thread only ever moves
// or drops a reference that is not the last.

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/denormals.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/frame.h>
#include <espresso/primitives/pipeTable.h>
#include <espresso/primitives/plan.h>
#include <espresso/primitives/primitive.h>
#include <espresso/primitives/realtime.h>
#include <espresso/primitives/spscQueue.h>

#include "transition.h"
#include "workers.h"

namespace primitives {

using metadata::pipeRef;
using role = transition::role;

namespace {

// The beat clock is a sum of per-block increments, so a beat that falls
// on a block boundary may be reached a rounding error early or late. Beat
// comparisons allow for this; it is far below one sample at any tempo and
// sample rate the engine runs at.
constexpr double beatTolerance = 1e-9;

// The tempo the beat clock runs at for tempo pipe value @p t: a positive
// value, bounded, or 120 BPM (primitives.md §7.5).
constexpr double clockTempo(double t) noexcept { return t > 0.0 ? std::clamp(t, 1e-3, 1e4) : 120.0; }

engineOptions checked(engineOptions options) {
    if (options.audio.blockSize <= 0) throw std::invalid_argument("engine: the block size must be positive");
    if (options.threads < 1 || options.threads > maxThreads) {
        throw std::invalid_argument("engine: the thread count must be from 1 to " +
                                    std::to_string(maxThreads));
    }
    if (options.workerSpin < std::chrono::microseconds::zero() ||
        options.workerSpin > std::chrono::seconds(1)) {
        throw std::invalid_argument("engine: a worker spins from 0 to 1 s");
    }
    if (!std::isfinite(options.audio.sampleRate) || options.audio.sampleRate <= 0.0) {
        throw std::invalid_argument("engine: the sample rate must be positive and finite");
    }
    if (const bool atLeastZero = options.schedule.minimumTaskCost >= 0.0; !atLeastZero) {
        throw std::invalid_argument("engine: the least task cost must be a number, at least 0");
    }
    if (const bool atLeastZero = options.schedule.minimumParallelSaving >= 0.0; !atLeastZero) {
        throw std::invalid_argument("engine: the least parallel saving must be a number, at least 0");
    }
    // Schedules are made for the engine's own thread count.
    options.schedule.threads = options.threads;
    return options;  // moved out: the loader need not be copied
}

// The work on pipes that falls to each node of a block (threads.md
// §6.8): the pipes it starts before it runs and those it publishes after,
// on the thread that runs it. @p forwarding is true when every private
// pipe of the work that has a target forwards: the active plan's, while
// the engine has turned forwarding on for it (threads.md §4.2.5).
class nodePipes {
public:
    nodePipes(const pipeWork& work, pipeTable& pipes, int numSamples,
              bool forwarding) noexcept ESPRESSO_NONBLOCKING
        : work_(work),
          pipes_(pipes),
          numSamples_(numSamples),
          mask_(static_cast<std::uint8_t>(forwarding
                                              ? pipeWork::touchesPipes
                                              : pipeWork::touchesPipes | pipeWork::touchesForwarding)) {}

    // Whether node @p node starts or publishes any pipe. A private pipe
    // that forwards is never read, so it is not cleared (threads.md
    // §4.2.5): while the plan forwards, a node that would start only such
    // pipes does nothing around its process() and reads none of them, so
    // a plan of many writers does not fetch a cache line for each.
    bool touches(std::uint32_t node) const noexcept ESPRESSO_NONBLOCKING {
        return (work_.touches[node] & mask_) != 0;
    }

    // Before node @p node runs. Most nodes start no pipe, so the call
    // into the table is made only for those that do.
    void start(std::uint32_t node) const noexcept ESPRESSO_NONBLOCKING {
        if (const auto starts = work_.starts.of(node); !starts.empty())
            pipes_.beginBlock(starts, numSamples_);
        for (const auto& p : work_.privateStarts.of(node)) {
            if (!p->forwarding()) p->beginBlock(numSamples_);
        }
    }
    // After node @p node has run; most nodes publish no pipe.
    void publish(std::uint32_t node) const noexcept ESPRESSO_NONBLOCKING {
        if (const auto publishes = work_.publishes.of(node); !publishes.empty()) pipes_.publish(publishes);
    }

private:
    const pipeWork& work_;
    pipeTable& pipes_;
    int numSamples_ = 0;
    std::uint8_t mask_ = 0;  // the bits of pipeWork::touches this block acts on
};

// Runs the nodes of a block with @p Step, each with its work on pipes
// around it (threads.md §6.8): its starts, the step, then its publishes,
// on the thread that runs it. The step runs a node of a plan (planStep)
// or of a crossfade's merged order (fadeStep); a template parameter, so
// that it inlines into each node's run.
template <typename Step>
class blockRunner final : public nodeRunner {
public:
    blockRunner(nodePipes pipes, Step step) noexcept ESPRESSO_NONBLOCKING : pipes_(pipes), step_(step) {}

    void run(std::uint32_t node) noexcept ESPRESSO_NONBLOCKING override { runNode(node); }
    void runAll(std::span<const std::uint32_t> nodes) noexcept ESPRESSO_NONBLOCKING override {
        for (const auto node : nodes) runNode(node);
    }

private:
    void runNode(std::uint32_t node) noexcept ESPRESSO_NONBLOCKING {
        const bool touches = pipes_.touches(node);
        if (touches) pipes_.start(node);
        step_(node);
        if (touches) pipes_.publish(node);
    }

    nodePipes pipes_;
    Step step_;
};

// Runs a node of a plan, timing it when @p seconds is not empty.
class planStep {
public:
    planStep(std::span<const std::shared_ptr<primitive>> nodes, std::span<double> seconds,
             const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING : nodes_(nodes),
                                                                      seconds_(seconds),
                                                                      ctx_(ctx) {}

    void operator()(std::uint32_t node) noexcept ESPRESSO_NONBLOCKING {
        if (seconds_.empty()) {
            nodes_[node]->process(ctx_);
        } else {
            const auto begin = steadyNow();
            nodes_[node]->process(ctx_);
            seconds_[node] += std::chrono::duration<double>(steadyNow() - begin).count();
        }
    }

private:
    std::span<const std::shared_ptr<primitive>> nodes_;
    std::span<double> seconds_;
    const blockContext& ctx_;
};

// The gains a fading node applies over a block whose fade position runs
// from @p g0 to @p g1 (§9.3.2).
std::pair<float, float> fadeGains(const transition::entry& e, float g0,
                                  float g1) noexcept ESPRESSO_NONBLOCKING {
    if (!e.ramp) return {1.0f, 1.0f};
    if (e.what == role::falling) return {1.0f - g0, 1.0f - g1};
    return {g0, g1};
}

// Runs a node of a crossfade's merged order after setting its fade for the
// block, whose fade position runs from g0 to g1; `last` marks the fade's
// last block. The gains depend only on the entry and the block, so the
// thread that runs a node sets them.
class fadeStep {
public:
    struct position {
        float g0 = 0.0f;
        float g1 = 0.0f;
        bool last = false;
    };

    fadeStep(std::span<const transition::entry> order, position at,
             const blockContext& ctx) noexcept ESPRESSO_NONBLOCKING : order_(order),
                                                                      at_(at),
                                                                      ctx_(ctx) {}

    void operator()(std::uint32_t node) noexcept ESPRESSO_NONBLOCKING {
        const auto& e = order_[node];
        const bool falling = e.what == role::falling;
        const auto [a0, a1] = fadeGains(e, at_.g0, at_.g1);
        e.node->setFade(a0, a1, falling, falling && at_.last);
        e.node->process(ctx_);
    }

private:
    std::span<const transition::entry> order_;
    position at_;
    const blockContext& ctx_;
};

// Turns forwarding of plan @p p's private pipes on or off (threads.md
// §4.2.5). Called on the audio thread between blocks, when no worker holds
// a task: the next block's tasks reach the workers through the pool's
// queue, whose pushes publish the change.
void forwardPrivatePipes(const plan& p, bool on) noexcept ESPRESSO_NONBLOCKING {
    for (const auto& q : p.privatePipes) q->forward(on);
}

// Pushes to a queue that the sizing invariant (see the top of this file)
// keeps from filling.
template <typename T, typename U>
void pushSized(spscQueue<T>& queue, U&& item) noexcept ESPRESSO_NONBLOCKING {
    [[maybe_unused]] const bool pushed = queue.push(std::forward<U>(item));
    ESPRESSO_ASSUME_NONBLOCKING_BEGIN  // assert only aborts, on a broken sizing invariant
    assert(pushed);
    ESPRESSO_ASSUME_NONBLOCKING_END
}

} // namespace

std::optional<quantization> parseQuantization(std::string_view name) noexcept {
    if (name == "none") return quantization::none;
    if (name == "beat") return quantization::beat;
    if (name == "bar") return quantization::bar;
    return std::nullopt;
}

engine::engine(engineOptions options)
    : options_(checked(std::move(options))),
      pipes_(options_.pipes, options_.audio),
      buffers_(options_.bufferMemory),
      workers_(options_.threads > 1
                   ? std::make_unique<workerPool>(options_.threads, options_.workerStart, options_.workerSpin)
                   : nullptr),
      submitted_(maxOutstanding),
      retired_(maxOutstanding) {}

engine::~engine() = default;  // the audio thread has stopped; every owner frees its own

std::shared_ptr<plan> engine::build(const metadata::graphSpec& graph, std::int32_t planId) {
    return buildPlan(graph, planId,
                     {.pipes = pipes_,
                      .io = io_,
                      .info = options_.audio,
                      .loadImage = options_.loadImage,
                      .buffers = buffers_,
                      .loadSample = options_.loadSample,
                      .schedule = options_.schedule});
}

// --- Control thread -----------------------------------------------------

double engine::grid(quantization quant) const noexcept {
    switch (quant) {
    case quantization::none: return 0.0;
    case quantization::beat: return 1.0;
    case quantization::bar: return static_cast<double>(std::max(1, meter_.load(std::memory_order_relaxed)));
    }
    return 0.0;
}

std::unique_ptr<transition> engine::transitionTo(std::shared_ptr<plan> next, double fade, quantization quant,
                                                 std::uint64_t seq) const {
    auto t = makeTransition(std::move(next), confirmed_, fade, quant, options_.audio.sampleRate,
                            options_.schedule);
    t->seq = seq;
    return t;
}

// Sends @p t when fewer than maxOutstanding transitions are out, aiming
// its quantization target at the next grid point from now. Leaves @p t
// untouched otherwise.
bool engine::send(std::unique_ptr<transition>& t) {
    if (outstanding_ >= maxOutstanding) return false;
    t->grid = grid(t->quant);
    t->target = t->grid > 0.0 ? (std::floor(beat() / t->grid) + 1.0) * t->grid : NAN;
    if (!submitted_.push(std::move(t)))
        return false;  // cannot fail: submitted_ holds only outstanding transitions
    ++outstanding_;
    return true;
}

void engine::submit(std::unique_ptr<transition> t) {
    // The audio thread is not taking transitions: keep only the newest.
    if (unsent_) {
        withdraw(*unsent_);
        unsent_.reset();
    }
    if (!send(t)) unsent_ = std::move(t);
}

// The plan of transition @p t will not play: report it gone.
void engine::withdraw(const transition& t) {
    if (t.incoming) reported_.push_back({.what = notice::kind::retired, .plan = t.incoming->id});
    std::erase_if(inFlight_, [&](const flight& f) { return f.seq == t.seq; });
}

void engine::activate(std::uint64_t seq) {
    const auto it = std::ranges::find_if(inFlight_, [&](const flight& f) { return f.seq == seq; });
    if (it == inFlight_.end()) return;  // every swap the audio thread takes is in flight
    confirmed_ = std::move(it->incoming);
    if (confirmed_) reported_.push_back({.what = notice::kind::activated, .plan = confirmed_->id});
    inFlight_.erase(it);
    if (options_.pipes.video > 0) {  // a render thread can show it
        videoUnsent_ = confirmed_;   // replaces an older plan that never reached the render thread
        sendVideo();
    }
}

// Sends the newest activated plan to the render thread when there is room.
void engine::sendVideo() {
    if (!videoUnsent_ || videoOutstanding_ >= maxVideoOutstanding) return;
    if (videoSubmitted_.push(std::move(*videoUnsent_))) {
        ++videoOutstanding_;
        videoUnsent_.reset();
    }
}

void engine::settle(std::unique_ptr<transition> t) {
    switch (t->state) {
    case transition::fate::finished:  // incoming now holds the plan that was replaced
        if (t->outgoing) reported_.push_back({.what = notice::kind::retired, .plan = t->outgoing->id});
        break;
    case transition::fate::replaced: withdraw(*t); break;
    case transition::fate::stale:
        // Rebuild only the latest play: an older one was superseded while
        // it was on its way back, and must not replace the newer one.
        if (t->seq == latestSeq_) {
            submit(transitionTo(std::move(t->incoming), t->fade, t->quant, t->seq));
        } else {
            withdraw(*t);
        }
        break;
    case transition::fate::pending: break;
    }
}

void engine::absorb() {
    // Take back transitions before draining activations: a transition
    // was swapped in, if at all, before it was queued back, so the drain
    // below reports its activation before its retirement, and a stale one
    // is rebuilt against the plan the activations leave confirmed.
    std::vector<std::unique_ptr<transition>> back;
    for (std::unique_ptr<transition> t; retired_.pop(t);) {
        --outstanding_;
        back.push_back(std::move(t));
    }
    for (std::uint64_t seq = 0; activated_.pop(seq);) activate(seq);
    for (auto& t : back) settle(std::move(t));
    if (unsent_) send(unsent_);  // stays unsent while there is still no room
    // Plans the render thread replaced are freed here, on the control
    // thread, before the next pop (which never frees).
    for (std::shared_ptr<plan> done; videoRetired_.pop(done); done.reset()) --videoOutstanding_;
    sendVideo();
}

void engine::play(std::shared_ptr<plan> next, double fade, quantization quant) {
    absorb();
    fade = std::isfinite(fade) ? std::clamp(fade, 0.0, maxFade) : 0.0;
    inFlight_.push_back({.seq = ++latestSeq_, .incoming = next});
    submit(transitionTo(std::move(next), fade, quant, latestSeq_));
}

void engine::stop(double fade) { play(nullptr, fade, quantization::none); }

bool engine::setControl(std::int32_t index, float value) {
    if (!pipes_.contains({.kind = metadata::pipeKind::control, .index = index}) || !std::isfinite(value))
        return false;
    if (controls_.push({.index = index, .value = value})) return true;
    controlDrops_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

float engine::control(std::int32_t index) const {
    const pipeRef ref{.kind = metadata::pipeKind::control, .index = index};
    return pipes_.contains(ref) ? pipes_.control(index).value(0) : 0.0f;
}

double engine::tempo() const { return clockTempo(control(metadata::tempoPipe)); }

engineCounters engine::counters() const {
    engineCounters c;
    c.missedDeadlines = missed_.load(std::memory_order_relaxed);
    c.drops = io_.drops() + controlDrops_.load(std::memory_order_relaxed);
    c.overflows = pipes_.overflows();
    c.load = load_.load(std::memory_order_relaxed);
    c.threads = options_.threads;
    c.waitedBlocks = workers_ ? workers_->waitedBlocks() : 0;
    c.threadLoads.reserve(static_cast<std::size_t>(options_.threads));
    for (int i = 0; i < options_.threads; ++i) {
        c.threadLoads.push_back(threadLoads_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed));
    }
    c.activePlan = activePlan_.load(std::memory_order_relaxed);
    c.faults = io_.faults();
    return c;
}

std::vector<notice> engine::collect() {
    absorb();
    return std::exchange(reported_, {});
}

// --- Render thread ------------------------------------------------------

metadata::optionalRef<frame> engine::renderFrame(double time) noexcept ESPRESSO_NONBLOCKING {
    // Swap in the newest plan; each one replaced goes back to be freed.
    for (std::size_t i = 0; i < maxVideoOutstanding; ++i) {
        ESPRESSO_ASSUME_NONBLOCKING_BEGIN  // empty at its end: the pop failed, or the push moved it out
        std::shared_ptr<plan> next;
        ESPRESSO_ASSUME_NONBLOCKING_END
        if (!videoSubmitted_.pop(next)) break;
        std::swap(videoActive_, next);
        pushSized(videoRetired_, std::move(next));
    }
    if (!videoActive_) return std::nullopt;
    blockContext ctx;
    ctx.sampleRate = options_.audio.sampleRate;
    ctx.hostTime = time;
    for (const auto& node : videoActive_->videoNodes) node->process(ctx);
    if (!videoActive_->display) return std::nullopt;
    return std::cref(videoActive_->display->image());
}

// --- Audio thread -------------------------------------------------------

// Gives a transition back to the control thread, which frees it.
void engine::handBack(std::unique_ptr<transition>&& t) noexcept ESPRESSO_NONBLOCKING {
    pushSized(retired_, std::move(t));
}

// True when the pending swap @p t lands in the block that ends at beat
// @p beatEnd: at once without quantization, otherwise in the block that
// contains its target (§9.4.1). A target that passed while the swap
// waited for a fade moves to the next grid point.
bool engine::due(transition& t, double beatEnd) const noexcept ESPRESSO_NONBLOCKING {
    if (std::isnan(t.target)) return true;
    const double now = beatPosition_ - beatTolerance;
    if (t.waited && t.target < now) t.target += std::ceil((now - t.target) / t.grid) * t.grid;
    return t.target < beatEnd - beatTolerance;
}

void engine::swapIn(std::unique_ptr<transition>&& t) noexcept ESPRESSO_NONBLOCKING {
    // Every swap runs at least one block of crossfade, whose merged order
    // interleaves the two plans' writers: neither plan forwards until the
    // fade has finished (threads.md §4.2.5).
    if (t->incoming) forwardPrivatePipes(*t->incoming, false);
    if (active_) forwardPrivatePipes(*active_, false);
    for (const auto& [in, out] : t->adopt) in->adoptState(*out);
    // Activate the incoming ports before deactivating the outgoing ones, so
    // every input node always has an active port and no packet is lost in
    // between (ioHub delivers to one active port per node).
    if (const auto& next = t->incoming) {
        for (const auto& d : next->pipes) pipes_.configure(d);
        if (next->tempo) pipes_.control(metadata::tempoPipe).setAll(static_cast<float>(*next->tempo));
        meter_.store(next->meter, std::memory_order_relaxed);
        for (const auto& p : next->ports) p->setActive(true);
    }
    if (active_) {
        const auto& kept = t->incoming ? t->incoming->ports : active_->ports;  // none kept on stop
        for (const auto& p : active_->ports) {
            if (!t->incoming || std::ranges::find(kept, p) == kept.end()) p->setActive(false);
        }
    }
    pushSized(activated_, t->seq);
    activePlan_.store(t->incoming ? t->incoming->id : 0, std::memory_order_relaxed);
    // Exchange, not assign: the old plan's reference moves into the
    // transition, so no reference count changes on this thread.
    std::swap(active_, t->incoming);
    std::swap(fading_, t);  // fading_ was empty: a swap waits for the fade to finish
}

void engine::finishFade() noexcept ESPRESSO_NONBLOCKING {
    if (active_) {
        for (const auto& node : active_->nodes) node->setFade(1.0f, 1.0f, false);
        // The plan runs alone from the next block until the next swap. Its
        // private pipes forward when its blocks run in plan order
        // (threads.md §4.2.5), which its builder decided.
        forwardPrivatePipes(*active_, active_->runsInPlanOrder);
    }
    fading_->state = transition::fate::finished;
    handBack(std::move(fading_));
}

void engine::runBlock(const deviceBuffers& device, int offset, int n,
                      double hostTime) noexcept ESPRESSO_NONBLOCKING {
    // Each loop takes at most what its queue held at the start of the block.
    controlWrite w;
    for (std::size_t i = 0; i < controls_.capacity() && controls_.pop(w); ++i)
        pipes_.control(w.index).setAll(w.value);

    // A newer transition replaces one still waiting; the older goes back.
    ESPRESSO_ASSUME_NONBLOCKING_BEGIN  // empty at its end: each transition popped moves on to pending_
    std::unique_ptr<transition> arrived;
    ESPRESSO_ASSUME_NONBLOCKING_END
    for (std::size_t i = 0; i < submitted_.capacity() && submitted_.pop(arrived); ++i) {
        if (pending_) {
            pending_->state = transition::fate::replaced;
            handBack(std::move(pending_));
        }
        std::swap(pending_, arrived);  // pending_ was empty or handed back
    }

    const double sampleRate = options_.audio.sampleRate;
    auto readTempo = [&] { return clockTempo(pipes_.control(metadata::tempoPipe).value(0)); };
    double tempo = readTempo();
    double beatEnd = beatPosition_ + tempo / 60.0 * n / sampleRate;

    // A swap waits for the current fade to finish, so no node's gain jumps.
    if (pending_) {
        if (fading_) {
            pending_->waited = true;
        } else if (pending_->outgoing != active_) {
            // Built against a plan that is no longer active: rebuild.
            pending_->state = transition::fate::stale;
            handBack(std::move(pending_));
        } else if (due(*pending_, beatEnd)) {
            swapIn(std::move(pending_));
            tempo = readTempo();  // the incoming plan may set it
            beatEnd = beatPosition_ + tempo / 60.0 * n / sampleRate;
        }
    }

    blockContext ctx;
    ctx.blockStartSample = sampleClock_;
    ctx.numSamples = n;
    ctx.sampleRate = sampleRate;
    ctx.beat = beatPosition_;
    ctx.tempo = tempo;
    ctx.meter = meter_.load(std::memory_order_relaxed);
    ctx.hostTime = hostTime;
    ctx.device = device;
    ctx.deviceOffset = offset;

    if (fading_ || active_) {
        // The fade's schedule, or the active plan's.
        nodeSchedule& schedule = fading_ ? fading_->schedule() : active_->schedule;
        // The block's work on pipes outside its nodes (threads.md §6.8):
        // the pipes no node starts, before any runs, and the ends after.
        const pipeWork& work = schedule.pipes;
        pipes_.beginBlock(work.serialStarts, n);
        for (const auto& p : work.serialPrivateStarts) p->beginBlock(n);
        // Only the active plan running alone forwards: every swap turns
        // forwarding off for both plans until its fade has finished, and
        // finishFade() turns it on for the plan left when it runs in plan
        // order.
        const bool forwarding = !fading_ && active_->runsInPlanOrder;
        const nodePipes pipes(work, pipes_, n, forwarding);
        // On the pool, or with one thread in the schedule's serial order.
        const auto run = [&](nodeRunner& runner) noexcept ESPRESSO_NONBLOCKING {
            if (workers_) {
                workers_->run(schedule, runner);
            } else {
                runSerially(schedule, runner);
            }
        };
        if (fading_) {
            // A fade of zero samples still runs one block, in which retiring
            // nodes are silent but can end their held notes.
            const auto total = static_cast<double>(fading_->fadeSamples);
            const auto g0 =
                total > 0.0 ? static_cast<float>(static_cast<double>(fading_->fadeDone) / total) : 1.0f;
            const auto g1 =
                total > 0.0
                    ? static_cast<float>(std::min(1.0, static_cast<double>(fading_->fadeDone + n) / total))
                    : 1.0f;
            const bool last = fading_->fadeDone + n >= fading_->fadeSamples;
            blockRunner runner(pipes, fadeStep(fading_->order, {.g0 = g0, .g1 = g1, .last = last}, ctx));
            run(runner);
            fading_->fadeDone += n;
        } else {
            blockRunner runner(
                pipes,
                planStep(active_->nodes,
                         options_.profile ? std::span<double>(active_->seconds) : std::span<double>(), ctx));
            run(runner);
        }
        pipes_.endBlock(work.serialEnds);
    }
    // Retire only after the last use of the transition's pipe work: the
    // control thread may free it as soon as it is queued.
    if (fading_ && fading_->fadeDone >= fading_->fadeSamples) finishFade();

    beatPosition_ = beatEnd;
    sampleClock_ += n;
    beat_.store(beatPosition_, std::memory_order_relaxed);
}

void engine::process(const deviceBuffers& device, int numSamples,
                     double hostTime) noexcept ESPRESSO_NONBLOCKING {
    const denormalGuard guard;
    const auto start = steadyNow();
    const auto frames = static_cast<std::size_t>(std::max(numSamples, 0));
    for (const auto& out : device.outputs) {
        const auto block = out.first(std::min(frames, out.size()));
        std::ranges::fill(block, 0.0f);
    }
    const int blockSize = options_.audio.blockSize;
    const double sampleRate = options_.audio.sampleRate;
    for (int offset = 0; offset < numSamples; offset += blockSize) {
        runBlock(device, offset, std::min(blockSize, numSamples - offset), hostTime + offset / sampleRate);
    }
    // Several sinks may share a device channel, each within its ceiling;
    // the device never receives more than full scale or a non-finite value.
    for (const auto& out : device.outputs) {
        for (float& x : out.first(std::min(frames, out.size()))) x = std::isfinite(x) ? std::clamp(x, -1.0f, 1.0f) : 0.0f;
    }
    const double elapsed = std::chrono::duration<double>(steadyNow() - start).count();
    const double period = numSamples / sampleRate;
    if (period > 0.0) {
        if (elapsed > period) missed_.fetch_add(1, std::memory_order_relaxed);
        const auto ratio = static_cast<float>(elapsed / period);
        auto smooth = [](std::atomic<float>& into, float value) noexcept ESPRESSO_NONBLOCKING {
            into.store(0.9f * into.load(std::memory_order_relaxed) + 0.1f * value, std::memory_order_relaxed);
        };
        smooth(load_, ratio);
        // Each thread's time running nodes in this callback (threads.md §8).
        if (workers_) {
            std::array<std::uint64_t, maxThreads> busy{};
            const auto threads = static_cast<std::size_t>(workers_->threads());
            workers_->takeBusy(std::span(busy).first(threads));
            for (std::size_t i = 0; i < threads; ++i) {
                smooth(threadLoads_[i], static_cast<float>(static_cast<double>(busy[i]) * 1e-9 / period));
            }
        } else {
            threadLoads_[0].store(load_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
    }
}

}  // namespace primitives
