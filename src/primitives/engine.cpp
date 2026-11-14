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
// A transition is built against the plan the control thread last saw
// become active (`confirmed_`). If the audio thread activated another
// plan meanwhile, the transition is stale: it goes back, and the control
// thread rebuilds it against the plan now active. No thread ever reads a
// transition it does not own.
//
// Plans and primitives are shared: transitions refer to the plans and
// nodes they cross between. The control thread holds a reference to every
// plan the audio thread may still use, so the audio thread only ever moves
// or drops a reference that is not the last.

#include <proj/metadata/primitiveSpec.h>
#include <proj/primitives/denormals.h>
#include <proj/primitives/engine.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <queue>
#include <tuple>
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
        std::shared_ptr<primitive> node;
        role what;
        bool ramp;  // applies the fade gain; see fadeRamps
    };
    // Why the audio thread handed the transition back.
    enum class fate : std::uint8_t { pending, replaced, stale, finished };

    std::shared_ptr<plan> incoming;  // becomes the active plan at the swap
    std::shared_ptr<plan> outgoing;  // the plan it was built against, kept until the transition is freed
    std::vector<entry> order;
    std::vector<std::pair<std::shared_ptr<primitive>, std::shared_ptr<primitive>>> adopt;  // incoming, outgoing
    std::vector<pipeRef> used;
    double fade = 0.0;
    quantization quant = quantization::none;
    double target = NAN;  // beat to swap at; NaN swaps at the next block
    double grid = 0.0;    // beats between grid points; 0 without quantization
    std::int64_t fadeSamples = 0;
    std::int64_t fadeDone = 0;
    fate state = fate::pending;
};

namespace {

using role = transition::role;

// A node taking part in a transition.
struct item {
    std::shared_ptr<primitive> node;
    role what;
    std::reference_wrapper<const nodeInfo> info;
    std::reference_wrapper<const plan> owner;
    std::size_t rank;  // incoming nodes at even ranks, retiring at odd: ties keep each plan's order
};

// Matches incoming nodes to outgoing ones by ID and shape (§9.2): matched
// nodes are steady and adopt state; the rest of the incoming plan rises
// and the rest of the outgoing plan falls.
std::vector<item> assignRoles(const std::shared_ptr<plan>& incoming, const std::shared_ptr<plan>& outgoing,
                              std::vector<std::pair<std::shared_ptr<primitive>, std::shared_ptr<primitive>>>& adopt) {
    std::vector<item> items;
    std::vector<bool> adopted(outgoing ? outgoing->nodes.size() : 0, false);
    std::unordered_map<std::string_view, std::size_t> outgoingIds;
    if (outgoing) {
        for (std::size_t j = 0; j < outgoing->nodes.size(); ++j) outgoingIds.emplace(outgoing->nodes[j]->id(), j);
    }
    if (incoming) {
        for (std::size_t i = 0; i < incoming->nodes.size(); ++i) {
            const auto& node = incoming->nodes[i];
            role what = role::rising;
            if (const auto it = outgoingIds.find(node->id()); it != outgoingIds.end()) {
                const std::size_t j = it->second;
                if (outgoing->info[j].shape == incoming->info[i].shape) {  // §9.2.2
                    adopt.emplace_back(node, outgoing->nodes[j]);
                    adopted[j] = true;
                    what = role::steady;
                }
            }
            items.push_back({node, what, std::cref(incoming->info[i]), std::cref(*incoming), 2 * i});
        }
    }
    if (outgoing) {
        for (std::size_t j = 0; j < outgoing->nodes.size(); ++j) {
            if (!adopted[j]) {
                items.push_back({outgoing->nodes[j], role::falling, std::cref(outgoing->info[j]), std::cref(*outgoing), 2 * j + 1});
            }
        }
    }
    return items;
}

// Fade gain is applied once per signal path (§9.3.2), where the path
// leaves the fading subgraph: a fading node ramps when a node in another
// role reads its audio, or when it writes the device (a sink). Nodes
// inside a fading subgraph pass audio through unscaled, so chains,
// nonlinear stages (a carrier modulated by a new modulator), and signal
// circulating in a feedback loop are all scaled exactly once. Where old
// and new plans share a pipe, each reader applies its own ramp, which
// splits the mixed signal between them.
std::vector<bool> fadeRamps(const std::vector<item>& items) {
    std::map<pipeRef, std::vector<std::size_t>> audioReaders;
    for (std::size_t k = 0; k < items.size(); ++k) {
        for (auto r : items[k].info.get().reads) {
            if (r.kind == metadata::pipeKind::audio) audioReaders[r].push_back(k);
        }
    }
    std::vector<bool> ramps(items.size(), false);
    for (std::size_t k = 0; k < items.size(); ++k) {
        if (items[k].what == role::steady) continue;
        const auto spec = metadata::findPrimitive(items[k].node->type());
        bool exit = spec && spec->get().kind == metadata::role::sink;
        for (auto w : items[k].info.get().writes) {
            if (w.kind != metadata::pipeKind::audio) continue;
            const auto it = audioReaders.find(w);
            if (it == audioReaders.end()) continue;
            for (std::size_t reader : it->second) {
                if (reader != k && items[reader].what != items[k].what) exit = true;
            }
        }
        ramps[k] = exit;
    }
    return ramps;
}

// Orders @p items so every writer of a pipe runs before its readers
// (§9.3.1). A cycle through retiring nodes is broken at its earliest
// retiring node, which runs where the cycle is cut.
std::vector<transition::entry> mergeOrder(const std::vector<item>& items, const std::vector<bool>& ramps) {
    std::vector<transition::entry> order;
    // Edges from each writer of a pipe to each of its readers.
    std::map<pipeRef, std::vector<std::size_t>> writers;
    for (std::size_t k = 0; k < items.size(); ++k) {
        for (auto w : items[k].info.get().writes) {
            if (!items[k].owner.get().isFeedback(w)) writers[w].push_back(k);
        }
    }
    std::vector<std::vector<std::size_t>> successors(items.size());
    std::vector<std::size_t> indegree(items.size(), 0);
    for (std::size_t k = 0; k < items.size(); ++k) {
        for (auto r : items[k].info.get().reads) {
            if (items[k].owner.get().isFeedback(r)) continue;
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
                    if (!done[k]) order.push_back({items[k].node, items[k].what, ramps[k]}), done[k] = true;
                }
                break;
            }
            // It runs, reading what the cycle left from the previous block.
            order.push_back({items[cut].node, items[cut].what, ramps[cut]});
            release(cut);
            continue;
        }
        const std::size_t k = ready.top();
        ready.pop();
        if (done[k]) continue;
        order.push_back({items[k].node, items[k].what, ramps[k]});
        release(k);
    }
    return order;
}

// Builds the swap from @p outgoing to @p incoming.
std::unique_ptr<transition> makeTransition(std::shared_ptr<plan> incoming, std::shared_ptr<plan> outgoing, double fade,
                                           quantization quant, double sampleRate) {
    auto t = std::make_unique<transition>();
    t->fade = fade;
    t->quant = quant;
    t->fadeSamples = std::max<std::int64_t>(0, std::llround(fade * sampleRate));
    const auto items = assignRoles(incoming, outgoing, t->adopt);
    t->order = mergeOrder(items, fadeRamps(items));
    for (const auto& p : {incoming, outgoing}) {
        if (p) t->used.insert(t->used.end(), p->used.begin(), p->used.end());
    }
    std::sort(t->used.begin(), t->used.end());
    t->used.erase(std::unique(t->used.begin(), t->used.end()), t->used.end());
    t->incoming = std::move(incoming);
    t->outgoing = std::move(outgoing);
    return t;
}

} // namespace

engine::engine(engineOptions options)
    : options_(options), pipes_(options.pipes, options.audio), submitted_(64), retired_(1024) {}

engine::~engine() = default;  // the audio thread has stopped; every owner frees its own

std::shared_ptr<plan> engine::build(const metadata::graphSpec& graph, std::int32_t planId) {
    return buildPlan(graph, planId, {pipes_, io_, options_.audio});
}

// --- Control thread -----------------------------------------------------

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

void engine::submit(std::unique_ptr<transition> t) {
    t->target = quantizeTarget(t->quant);
    t->grid = t->quant == quantization::beat  ? 1.0
              : t->quant == quantization::bar ? static_cast<double>(std::max(1, meter_.load(std::memory_order_relaxed)))
                                              : 0.0;
    if (t->incoming) inFlight_.push_back(t->incoming);
    if (unsent_ || !submitted_.push(std::move(t))) {
        // The audio thread is not taking transitions: keep only the newest.
        if (unsent_ && unsent_->incoming) {
            reported_.push_back({notice::kind::retired, unsent_->incoming->id});
            std::erase(inFlight_, unsent_->incoming);
        }
        if (t) unsent_ = std::move(t);
    }
}

void engine::absorb() {
    if (unsent_ && submitted_.push(std::move(unsent_))) unsent_.reset();
    // Notices first: a stale transition is rebuilt against the plan the
    // notices report active.
    for (bool more = true; more;) {
        more = false;
        notice n;
        while (notices_.pop(n)) {
            more = true;
            if (n.what == notice::kind::stopped) {
                confirmed_.reset();
                continue;
            }
            const auto it = std::find_if(inFlight_.begin(), inFlight_.end(), [&](const auto& p) { return p->id == n.plan; });
            if (it == inFlight_.end()) continue;
            confirmed_ = *it;
            inFlight_.erase(it);
            reported_.push_back(n);
        }
        std::unique_ptr<transition> t;
        while (retired_.pop(t)) {
            more = true;
            switch (t->state) {
            case transition::fate::finished:  // incoming now holds the plan that was replaced
                if (t->outgoing) reported_.push_back({notice::kind::retired, t->outgoing->id});
                break;
            case transition::fate::replaced:
                if (t->incoming) {
                    reported_.push_back({notice::kind::retired, t->incoming->id});
                    std::erase(inFlight_, t->incoming);
                }
                break;
            case transition::fate::stale: {
                auto next = std::move(t->incoming);
                if (next) std::erase(inFlight_, next);
                const double fade = t->fade;
                const auto quant = t->quant;
                t.reset();
                submit(makeTransition(std::move(next), confirmed_, fade, quant, options_.audio.sampleRate));
                break;
            }
            case transition::fate::pending: break;
            }
        }
    }
}

void engine::play(std::shared_ptr<plan> next, double fade, quantization quant) {
    absorb();
    submit(makeTransition(std::move(next), confirmed_, fade, quant, options_.audio.sampleRate));
}

void engine::stop(double fade) { play(nullptr, fade, quantization::none); }

bool engine::setControl(std::int32_t index, float value) {
    if (!pipes_.contains({metadata::pipeKind::control, index}) || !std::isfinite(value)) return false;
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
    c.faults = io_.faults();
    return c;
}

std::vector<notice> engine::collect() {
    absorb();
    return std::exchange(reported_, {});
}

// --- Audio thread -------------------------------------------------------

// Gives a transition back to the control thread. If the queue is full, the
// transition waits in the backlog; the audio thread never frees one.
void engine::handBack(std::unique_ptr<transition>&& t) noexcept {
    if (retired_.push(std::move(t))) return;
    if (!backlog_) {
        backlog_ = std::move(t);
    } else {
        // Both full: the control thread has stalled for over a thousand
        // swaps. Leaking is the only choice that neither frees nor blocks.
        leaked_.fetch_add(1, std::memory_order_relaxed);
        std::ignore = t.release();
    }
}

void engine::swapIn(std::unique_ptr<transition> t) noexcept {
    for (const auto& [in, out] : t->adopt) in->adoptState(*out);
    if (active_) {
        for (const auto& p : active_->ports) p->setActive(false);
    }
    if (const auto& next = t->incoming) {
        for (const auto& d : next->pipes) pipes_.configure(d);
        if (next->tempo) pipes_.control(metadata::tempoPipe).setAll(static_cast<float>(*next->tempo));
        meter_.store(next->meter, std::memory_order_relaxed);
        for (const auto& p : next->ports) p->setActive(true);
    }
    notices_.push(t->incoming ? notice{notice::kind::activated, t->incoming->id} : notice{notice::kind::stopped, 0});
    activePlan_.store(t->incoming ? t->incoming->id : 0, std::memory_order_relaxed);
    // Exchange, not assign: the old plan's reference moves into the
    // transition, so no reference count changes on this thread.
    std::swap(active_, t->incoming);
    fading_ = std::move(t);
    fading_->fadeDone = 0;
}

void engine::finishFade() noexcept {
    if (active_) {
        for (const auto& node : active_->nodes) node->setFade(1.0f, 1.0f, false);
    }
    fading_->state = transition::fate::finished;
    handBack(std::move(fading_));
    fading_.reset();  // moved from; resetting an empty pointer frees nothing
}

void engine::runBlock(const deviceBuffers& device, int offset, int n, double hostTime) noexcept {
    controlWrite w;
    while (controls_.pop(w)) pipes_.control(w.index).setAll(w.value);
    if (backlog_ && retired_.push(std::move(backlog_))) backlog_.reset();

    // A newer transition replaces one still waiting; the older goes back.
    std::unique_ptr<transition> arrived;
    while (submitted_.pop(arrived)) {
        if (pending_) {
            pending_->state = transition::fate::replaced;
            handBack(std::move(pending_));
        }
        pending_ = std::move(arrived);
    }

    const double sampleRate = options_.audio.sampleRate;
    auto readTempo = [&] {
        const double t = pipes_.control(metadata::tempoPipe).value(0);
        return t > 0.0 ? std::clamp(t, 1e-3, 1e4) : 120.0;
    };
    double tempo = readTempo();
    double beatEnd = beatPosition_ + tempo / 60.0 * n / sampleRate;

    // A swap waits for the current fade to finish, so no node's gain jumps.
    // A quantized target that passes meanwhile moves to the next grid point.
    if (pending_ && fading_) waitedForFade_ = true;
    if (pending_ && !fading_) {
        if (pending_->outgoing != active_) {
            // Built against a plan that is no longer active: rebuild.
            pending_->state = transition::fate::stale;
            handBack(std::move(pending_));
            pending_.reset();
        } else {
            double target = pending_->target;
            if (waitedForFade_ && pending_->grid > 0.0 && target < beatPosition_) {
                target += std::ceil((beatPosition_ - target) / pending_->grid) * pending_->grid;
            }
            // Swap at the start of the block that contains the target (§9.4.1).
            if (std::isnan(target) || target < beatEnd) {
                swapIn(std::move(pending_));
                pending_.reset();
                waitedForFade_ = false;
                tempo = readTempo();  // the incoming plan may set it
                beatEnd = beatPosition_ + tempo / 60.0 * n / sampleRate;
            }
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

    const std::span<const pipeRef> used = fading_ ? std::span<const pipeRef>(fading_->used)
                                          : active_ ? std::span<const pipeRef>(active_->used)
                                                    : std::span<const pipeRef>{};
    pipes_.beginBlock(used, n);
    if (fading_) {
        // A fade of zero samples still runs one block, in which retiring
        // nodes are silent but can end their held notes.
        const auto total = static_cast<double>(fading_->fadeSamples);
        const auto g0 = total > 0.0 ? static_cast<float>(static_cast<double>(fading_->fadeDone) / total) : 1.0f;
        const auto g1 = total > 0.0 ? static_cast<float>(std::min(1.0, static_cast<double>(fading_->fadeDone + n) / total)) : 1.0f;
        const bool last = fading_->fadeDone + n >= fading_->fadeSamples;
        for (const auto& [node, what, ramp] : fading_->order) {
            switch (what) {
            case role::steady: node->setFade(1.0f, 1.0f, false); break;
            case role::rising: ramp ? node->setFade(g0, g1, false) : node->setFade(1.0f, 1.0f, false); break;
            case role::falling:
                ramp ? node->setFade(1.0f - g0, 1.0f - g1, true, last) : node->setFade(1.0f, 1.0f, true, last);
                break;
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
    const auto frames = static_cast<std::size_t>(numSamples);
    for (const auto& out : device.outputs) {
        const auto block = out.first(std::min(frames, out.size()));
        std::fill(block.begin(), block.end(), 0.0f);
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
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double period = numSamples / sampleRate;
    if (period > 0.0) {
        if (elapsed > period) missed_.fetch_add(1, std::memory_order_relaxed);
        const float ratio = static_cast<float>(elapsed / period);
        load_.store(0.9f * load_.load(std::memory_order_relaxed) + 0.1f * ratio, std::memory_order_relaxed);
    }
}

} // namespace primitives
