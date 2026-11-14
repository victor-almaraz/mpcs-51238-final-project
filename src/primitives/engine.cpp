// Engine: block execution, plan swaps, crossfades, quantization, and the
// beat clock (primitives.md §7.5, §9, §10).
//
// Swap protocol. The control thread publishes one transition at a time
// in `pending_` and records its quantization target in `pendingTarget_`
// first. The audio thread takes a transition with compare-exchange; the
// control thread withdraws one with exchange. Whichever succeeds owns it,
// so the audio thread never dereferences a transition the control thread
// may free. A transition is built against the plan that will be active
// when it is taken (`believedActive_`), which the control thread knows
// because it is the only publisher.

#include <proj/primitives/denormals.h>
#include <proj/primitives/engine.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <queue>
#include <unordered_map>

namespace primitives {

using metadata::pipeRef;

std::optional<quantization> parseQuantization(std::string_view name) noexcept {
    if (name == "none") return quantization::none;
    if (name == "beat") return quantization::beat;
    if (name == "bar") return quantization::bar;
    return std::nullopt;
}

// A swap from `outgoing` to `incoming`, with the merged execution order
// for the fade (§9.3.1). The audio thread takes one only when no fade is
// running.
struct transition {
    enum class role : std::uint8_t { steady, rising, falling };
    struct entry {
        primitive* node;
        role what;
        bool ramp;  // applies the fade gain; see makeTransition
    };

    std::unique_ptr<plan> incoming;  // released to the audio thread at the swap
    plan* outgoing = nullptr;        // active when the transition was built
    std::unique_ptr<plan> owned;     // the outgoing plan, owned from the swap on
    std::vector<entry> order;
    std::vector<std::pair<primitive*, primitive*>> adopt;  // incoming, outgoing
    std::vector<pipeRef> used;
    std::int64_t fadeSamples = 0;
    std::int64_t fadeDone = 0;
};

namespace {

// Merges the incoming nodes with the outgoing plan's retiring nodes in an
// order that keeps every writer of a pipe ahead of its readers. Ties keep
// each plan's own order. Retiring nodes caught in a cycle are cut.
std::unique_ptr<transition> makeTransition(std::unique_ptr<plan> incoming, plan* outgoing, double fade,
                                           double sampleRate) {
    using role = transition::role;
    auto t = std::make_unique<transition>();
    t->outgoing = outgoing;
    t->fadeSamples = std::max<std::int64_t>(0, std::llround(fade * sampleRate));

    struct item {
        primitive* node;
        role what;
        const nodeInfo* info;
        const plan* owner;
        std::size_t rank;
    };
    std::vector<item> items;
    std::vector<bool> adopted(outgoing ? outgoing->nodes.size() : 0, false);
    std::unordered_map<std::string_view, std::size_t> outgoingIds;
    if (outgoing) {
        for (std::size_t j = 0; j < outgoing->nodes.size(); ++j) outgoingIds.emplace(outgoing->nodes[j]->id(), j);
    }
    if (incoming) {
        for (std::size_t i = 0; i < incoming->nodes.size(); ++i) {
            primitive* node = incoming->nodes[i].get();
            role what = role::rising;
            if (const auto it = outgoingIds.find(node->id()); it != outgoingIds.end()) {
                const std::size_t j = it->second;
                if (outgoing->info[j].shape == incoming->info[i].shape) {  // §9.2.2
                    t->adopt.emplace_back(node, outgoing->nodes[j].get());
                    adopted[j] = true;
                    what = role::steady;
                }
            }
            items.push_back({node, what, &incoming->info[i], incoming.get(), 2 * i});
        }
    }
    if (outgoing) {
        for (std::size_t j = 0; j < outgoing->nodes.size(); ++j) {
            if (!adopted[j]) items.push_back({outgoing->nodes[j].get(), role::falling, &outgoing->info[j], outgoing, 2 * j + 1});
        }
    }

    // Fade gain is applied once per signal path (§9.3.2). A fading node
    // whose audio inputs all come from nodes fading the same way passes
    // its input through unscaled; any other fading node applies the ramp.
    // Chains of new or retiring nodes are therefore scaled once, and nodes
    // reading a pipe that mixes old and new signals split it between them.
    std::map<pipeRef, std::vector<std::size_t>> audioWriters;
    for (std::size_t k = 0; k < items.size(); ++k) {
        for (auto w : items[k].info->writes) {
            if (w.kind == metadata::pipeKind::audio) audioWriters[w].push_back(k);
        }
    }
    std::vector<bool> ramps(items.size(), false);
    for (std::size_t k = 0; k < items.size(); ++k) {
        if (items[k].what == role::steady) continue;
        bool fedOnlyBySameRole = false;
        bool other = false;
        for (auto r : items[k].info->reads) {
            if (r.kind != metadata::pipeKind::audio) continue;
            const auto it = audioWriters.find(r);
            if (it == audioWriters.end()) continue;
            for (std::size_t w : it->second) {
                if (w == k) continue;
                fedOnlyBySameRole = true;
                if (items[w].what != items[k].what) other = true;
            }
        }
        ramps[k] = !fedOnlyBySameRole || other;
    }

    // Edges from each writer of a pipe to each of its readers.
    std::map<pipeRef, std::vector<std::size_t>> writers;
    for (std::size_t k = 0; k < items.size(); ++k) {
        for (auto w : items[k].info->writes) {
            if (!items[k].owner->isFeedback(w)) writers[w].push_back(k);
        }
    }
    std::vector<std::vector<std::size_t>> successors(items.size());
    std::vector<std::size_t> indegree(items.size(), 0);
    for (std::size_t k = 0; k < items.size(); ++k) {
        for (auto r : items[k].info->reads) {
            if (items[k].owner->isFeedback(r)) continue;
            const auto it = writers.find(r);
            if (it == writers.end()) continue;
            for (std::size_t w : it->second) {
                if (w == k) continue;
                successors[w].push_back(k);
                ++indegree[k];
            }
        }
    }

    auto later = [&](std::size_t a, std::size_t b) { return items[a].rank > items[b].rank; };
    std::priority_queue<std::size_t, std::vector<std::size_t>, decltype(later)> ready(later);
    for (std::size_t k = 0; k < items.size(); ++k) {
        if (indegree[k] == 0) ready.push(k);
    }
    std::vector<bool> done(items.size(), false);
    std::size_t remaining = items.size();
    auto release = [&](std::size_t k) {
        done[k] = true;
        --remaining;
        for (std::size_t s : successors[k]) {
            if (--indegree[s] == 0 && !done[s]) ready.push(s);
        }
    };
    while (remaining > 0) {
        if (ready.empty()) {
            // A cycle through retiring nodes. The nodes left are on a cycle
            // or downstream of one; peel off those with no remaining
            // successor, then cut the earliest retiring node on a cycle.
            std::vector<bool> onCycle(items.size(), false);
            for (std::size_t k = 0; k < items.size(); ++k) onCycle[k] = !done[k];
            for (bool changed = true; changed;) {
                changed = false;
                for (std::size_t k = 0; k < items.size(); ++k) {
                    if (onCycle[k] && std::none_of(successors[k].begin(), successors[k].end(),
                                                   [&](std::size_t x) { return onCycle[x]; })) {
                        onCycle[k] = false;
                        changed = true;
                    }
                }
            }
            std::size_t cut = items.size();
            for (std::size_t k = 0; k < items.size(); ++k) {
                if (onCycle[k] && items[k].what == role::falling && (cut == items.size() || items[k].rank < items[cut].rank)) cut = k;
            }
            if (cut == items.size()) {  // cannot happen for validated plans
                for (std::size_t k = 0; k < items.size(); ++k) {
                    if (!done[k]) t->order.push_back({items[k].node, items[k].what, ramps[k]}), done[k] = true;
                }
                break;
            }
            release(cut);
            continue;
        }
        const std::size_t k = ready.top();
        ready.pop();
        if (done[k]) continue;
        t->order.push_back({items[k].node, items[k].what, ramps[k]});
        release(k);
    }

    for (const plan* p : {static_cast<const plan*>(incoming.get()), static_cast<const plan*>(outgoing)}) {
        if (p) t->used.insert(t->used.end(), p->used.begin(), p->used.end());
    }
    std::sort(t->used.begin(), t->used.end());
    t->used.erase(std::unique(t->used.begin(), t->used.end()), t->used.end());
    t->incoming = std::move(incoming);
    return t;
}

} // namespace

engine::engine(engineOptions options) : options_(options), pipes_(options.pipes, options.audio) {}

engine::~engine() {
    delete pending_.exchange(nullptr);
    delete fading_;
    delete active_;
    transition* t = nullptr;
    while (retired_.pop(t)) delete t;
}

std::unique_ptr<plan> engine::build(const metadata::graphSpec& graph, std::int32_t planId) {
    return buildPlan(graph, planId, {pipes_, io_, options_.audio});
}

// --- Control thread -----------------------------------------------------

void engine::discard(transition* t, std::vector<notice>& out) {
    if (t->incoming) out.push_back({notice::kind::retired, t->incoming->id});
    delete t;
}

double engine::quantizeTarget(quantization quant) const noexcept {
    const double now = beat();
    switch (quant) {
    case quantization::none: return NAN;
    case quantization::beat: return std::floor(now) + 1.0;
    case quantization::bar: {
        const double meter = std::max(1, meter_.load(std::memory_order_relaxed));
        return (std::floor(now / meter) + 1.0) * meter;
    }
    }
    return NAN;
}

void engine::play(std::unique_ptr<plan> next, double fade, quantization quant) {
    // Withdraw a transition the audio thread has not taken. If there is
    // none, the last published one was taken and its plan is active.
    transition* old = pending_.exchange(nullptr, std::memory_order_acq_rel);
    if (!old && published_) believedActive_ = publishedIncoming_;
    published_ = false;

    auto t = makeTransition(std::move(next), believedActive_, fade, options_.audio.sampleRate);
    pendingTarget_.store(quantizeTarget(quant), std::memory_order_release);
    publishedIncoming_ = t->incoming.get();
    published_ = true;
    pending_.store(t.release(), std::memory_order_release);
    // Freed only now, so the new transition cannot reuse its address while
    // the audio thread may still hold the old pointer value.
    if (old) discard(old, discarded_);
}

void engine::stop(double fade) { play(nullptr, fade, quantization::none); }

bool engine::setControl(std::int32_t index, float value) {
    if (!pipes_.contains({metadata::pipeKind::control, index})) return false;
    if (controls_.push({index, value})) return true;
    controlDrops_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

float engine::control(std::int32_t index) const {
    const pipeRef ref{metadata::pipeKind::control, index};
    return pipes_.contains(ref) ? pipes_.control(index).value(0) : 0.0f;
}

engineCounters engine::counters() const {
    engineCounters c;
    c.missedDeadlines = missed_.load(std::memory_order_relaxed);
    c.drops = io_.drops() + controlDrops_.load(std::memory_order_relaxed);
    c.overflows = pipes_.overflows();
    c.load = load_.load(std::memory_order_relaxed);
    c.activePlan = activePlan_.load(std::memory_order_relaxed);
    return c;
}

std::vector<notice> engine::collect() {
    std::vector<notice> out;
    notice n;
    while (notices_.pop(n)) out.push_back(n);
    transition* t = nullptr;
    while (retired_.pop(t)) {
        if (t->owned) out.push_back({notice::kind::retired, t->owned->id});
        delete t;
    }
    out.insert(out.end(), discarded_.begin(), discarded_.end());
    discarded_.clear();
    return out;
}

// --- Audio thread -------------------------------------------------------

void engine::swapIn(transition* t) noexcept {
    for (auto [in, out] : t->adopt) in->adoptState(*out);
    plan* next = t->incoming.release();
    if (active_) {
        for (ioPort* p : active_->ports) p->setActive(false);
    }
    if (next) {
        for (const auto& d : next->pipes) pipes_.configure(d);
        if (next->tempo) pipes_.control(metadata::tempoPipe).setAll(static_cast<float>(*next->tempo));
        meter_.store(next->meter, std::memory_order_relaxed);
        for (ioPort* p : next->ports) p->setActive(true);
        notices_.push({notice::kind::activated, next->id});
    }
    t->owned.reset(active_);  // takes ownership; owned is empty, so nothing is freed
    active_ = next;
    activePlan_.store(next ? next->id : 0, std::memory_order_relaxed);
    fading_ = t;
    t->fadeDone = 0;
    if (t->fadeSamples == 0) finishFade();  // before the block computes its pipe list
}

void engine::finishFade() noexcept {
    if (active_) {
        for (auto& node : active_->nodes) node->setFade(1.0f, 1.0f, false);
    }
    retire(fading_);
    fading_ = nullptr;
}

void engine::retire(transition* t) noexcept {
    // The audio thread never frees. A full queue means the control thread
    // has stalled for over a thousand swaps; the transition is leaked.
    if (!retired_.push(t)) leaked_.fetch_add(1, std::memory_order_relaxed);
}

void engine::runBlock(const deviceBuffers& device, int offset, int n, double hostTime) noexcept {
    controlWrite w;
    while (controls_.pop(w)) pipes_.control(w.index).setAll(w.value);

    const double sampleRate = options_.audio.sampleRate;
    auto readTempo = [&] {
        const double t = pipes_.control(metadata::tempoPipe).value(0);
        return t > 0.0 ? std::clamp(t, 1e-3, 1e4) : 120.0;
    };
    double tempo = readTempo();
    double beatEnd = beatPosition_ + tempo / 60.0 * n / sampleRate;

    // A swap waits for the current fade to finish, so no node's gain jumps.
    transition* p = fading_ ? nullptr : pending_.load(std::memory_order_acquire);
    if (p) {
        const double target = pendingTarget_.load(std::memory_order_acquire);
        // Swap at the start of the block that contains the target (§9.4.1).
        if ((std::isnan(target) || target < beatEnd) &&
            pending_.compare_exchange_strong(p, nullptr, std::memory_order_acq_rel)) {
            swapIn(p);
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
    ctx.device = &device;
    ctx.deviceOffset = offset;

    const std::span<const pipeRef> used = fading_ ? std::span<const pipeRef>(fading_->used)
                                          : active_ ? std::span<const pipeRef>(active_->used)
                                                    : std::span<const pipeRef>{};
    pipes_.beginBlock(used, n);
    if (fading_) {
        using role = transition::role;
        const auto total = static_cast<double>(fading_->fadeSamples);
        const auto g0 = static_cast<float>(static_cast<double>(fading_->fadeDone) / total);
        const auto g1 = static_cast<float>(std::min(1.0, static_cast<double>(fading_->fadeDone + n) / total));
        for (const auto& [node, what, ramp] : fading_->order) {
            switch (what) {
            case role::steady: node->setFade(1.0f, 1.0f, false); break;
            case role::rising: ramp ? node->setFade(g0, g1, false) : node->setFade(1.0f, 1.0f, false); break;
            case role::falling: ramp ? node->setFade(1.0f - g0, 1.0f - g1, true) : node->setFade(1.0f, 1.0f, true); break;
            }
            node->process(ctx);
        }
        fading_->fadeDone += n;
    } else if (active_) {
        for (auto& node : active_->nodes) node->process(ctx);
    }
    pipes_.endBlock(used);
    // Retire only after the last use of the transition's pipe list: the
    // control thread may free it as soon as it is queued.
    if (fading_ && fading_->fadeDone >= fading_->fadeSamples) finishFade();

    beatPosition_ = beatEnd;
    sampleClock_ += n;
    beat_.store(beatPosition_, std::memory_order_relaxed);
}

void engine::process(const deviceBuffers& device, int numSamples, double hostTime) noexcept {
    const denormalGuard guard;
    const auto start = std::chrono::steady_clock::now();
    if (device.outputs) {
        for (int ch = 0; ch < device.numOutputs; ++ch) {
            if (device.outputs[ch]) std::fill(device.outputs[ch], device.outputs[ch] + numSamples, 0.0f);
        }
    }
    const int blockSize = options_.audio.blockSize;
    const double sampleRate = options_.audio.sampleRate;
    for (int offset = 0; offset < numSamples; offset += blockSize) {
        runBlock(device, offset, std::min(blockSize, numSamples - offset), hostTime + offset / sampleRate);
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double period = numSamples / sampleRate;
    if (period > 0.0) {
        if (elapsed > period) missed_.fetch_add(1, std::memory_order_relaxed);
        const float ratio = static_cast<float>(elapsed / period);
        load_.store(0.9f * load_.load(std::memory_order_relaxed) + 0.1f * ratio, std::memory_order_relaxed);
    }
}

} // namespace primitives
