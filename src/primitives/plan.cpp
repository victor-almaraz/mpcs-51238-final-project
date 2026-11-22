// Plan building: primitive construction from node descriptions, pipe
// resolution, and the data swaps need (primitives.md §9, §10).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/metadata/primitiveSpec.h>
#include <espresso/primitives/buffer.h>
#include <espresso/primitives/frame.h>
#include <espresso/primitives/pipe.h>
#include <espresso/primitives/plan.h>
#include <espresso/primitives/primitive.h>
#include <espresso/primitives/realtime.h>

#include "costs.h"
#include "node.h"
#include "spectral.h"
#include "tasks.h"

namespace primitives {

using metadata::paramValue;
using metadata::pipeKind;
using metadata::pipeRef;

namespace {

metadata::optionalRef<factoryEntry> findFactory(std::string_view type) {
    for (const auto table :
         {oscillatorFactories(), filterFactories(), modulationFactories(), delayFactories(), mathFactories(),
          eventFactories(), deviceFactories(), imageFactories(), bufferFactories(), spectralFactories()}) {
        const auto it = std::ranges::find_if(table, [&](const factoryEntry& f) { return f.type == type; });
        if (it != table.end()) return std::cref(*it);
    }
    return std::nullopt;
}

// IDs of the nodes writing each audio pipe of a graph.
using writerIds = std::map<pipeRef, std::vector<std::string_view>>;

writerIds audioWriters(const metadata::graphSpec& g) {
    writerIds out;
    for (const auto& n : g.nodes) {
        for (auto r : metadata::pipesWritten(n)) {
            if (r.kind == pipeKind::audio) out[r].push_back(n.id);
        }
    }
    for (auto& [ref, ids] : out) std::ranges::sort(ids);
    return out;
}

// True when @p n belongs to the video clock domain: it reads or writes a
// video pipe (§3, §5.5.2).
bool isVideo(std::span<const pipeRef> reads, std::span<const pipeRef> writes) {
    const auto video = [](pipeRef r) { return r.kind == pipeKind::video; };
    return std::ranges::any_of(reads, video) || std::ranges::any_of(writes, video);
}

// The single input and output video pipes of drawing generator @p n, or
// nothing when @p n is not one (videoOut has no output).
std::optional<std::pair<pipeRef, pipeRef>> drawsFromTo(const metadata::nodeSpec& n) {
    const auto in = n.inputs.find("in");
    const auto out = n.outputs.find("out");
    if (in == n.inputs.end() || out == n.outputs.end() || in->second.size() != 1 || out->second.size() != 1)
        return std::nullopt;
    const pipeRef from = in->second.front();
    const pipeRef to = out->second.front();
    if (from.kind != pipeKind::video || to.kind != pipeKind::video || from == to) return std::nullopt;
    return std::pair{from, to};
}

// The links of @p g's drawing chains, each mapped to the pipe its chain
// ends in (§5.5.5). A link is a video pipe that one drawing generator
// writes and one reads as its input, and that nothing else reads. The
// chain's nodes then all draw into the end pipe's frame, in node order:
// each starts from the frame the one before it drew, so the end frame is
// the one chained execution draws, with one copy instead of one per node.
// Validation keeps video pipes out of feedback and puts every reader
// after the pipe's writers; a chain whose end has more than one writer
// keeps its copies, as another writer could run between its nodes.
std::map<pipeRef, pipeRef> chainEnds(const metadata::graphSpec& g) {
    std::map<pipeRef, int> writers;
    std::map<pipeRef, int> readers;
    std::map<pipeRef, pipeRef> next;  // a drawing generator's input to its output
    for (const auto& n : g.nodes) {
        for (const auto r : metadata::pipesWritten(n)) ++writers[r];
        for (const auto r : metadata::pipesRead(n)) ++readers[r];
        if (const auto io = drawsFromTo(n)) next.emplace(io->first, io->second);
    }
    const auto once = [](const std::map<pipeRef, int>& counts, pipeRef r) {
        const auto it = counts.find(r);
        return it != counts.end() && it->second == 1;
    };
    const auto isLink = [&](pipeRef r) { return next.contains(r) && once(readers, r) && once(writers, r); };
    std::map<pipeRef, pipeRef> ends;
    for (const pipeRef link : std::views::keys(next)) {
        if (!isLink(link)) continue;
        pipeRef end = link;
        // Each step moves to a later node; the bound guards an unvalidated cycle.
        for (std::size_t step = 0; step <= g.nodes.size() && isLink(end); ++step) end = next.at(end);
        if (!isLink(end) && once(writers, end)) ends.emplace(link, end);
    }
    return ends;
}

// The channel count @p g declares for pipe @p r, clamped to what a pipe
// can hold; 1 when undeclared.
int channelsOf(const metadata::graphSpec& g, pipeRef r) {
    const auto d = metadata::findPipe(g, r);
    return std::clamp(d ? d->get().channels : 1, 1, metadata::maxChannels);
}

// Type, parameter bindings, port shapes, and the nodes feeding each audio
// input (§9.2.2). Equal keys mean the incoming node can take over the
// outgoing node's state; a node whose audio comes from different nodes
// crossfades instead of switching sources at once.
std::string shapeKey(const metadata::nodeSpec& n, const metadata::graphSpec& g, const writerIds& writers) {
    std::string key = n.type;
    for (const auto& [name, v] : n.params) {
        key += '|' + name + ':';
        if (const auto r = metadata::as<pipeRef>(v)) {
            key += metadata::toString(r->get().kind);
        } else if (const auto l = metadata::as<std::vector<double>>(v)) {
            key += "list" + std::to_string(l->get().size());
        } else {
            key += "static";
        }
    }
    for (const auto& [label, ports] : {std::pair{"|in", std::cref(n.inputs)}, std::pair{"|out", std::cref(n.outputs)}}) {
        key += label;
        for (const auto& [name, refs] : ports.get()) {
            key += '|' + name;
            for (auto r : refs)
                key += ':' + std::string(metadata::toString(r.kind)) + std::to_string(channelsOf(g, r));
        }
    }
    for (auto r : metadata::pipesRead(n)) {
        if (r.kind != pipeKind::audio) continue;
        key += "|from";
        if (const auto it = writers.find(r); it != writers.end()) {
            for (auto id : it->second) key += ':' + std::string(id);
        }
    }
    return key;
}

// The builder's own backstop for a node that skipped IR validation
// (ir::validate): required ports, port pipes, numeric ranges, and the
// primitive's constraints. A failure here would otherwise be a null pipe
// or an out-of-range index on the thread that runs the node.
void checkBuildable(const metadata::nodeSpec& n, const metadata::primitiveSpec& spec,
                    const metadata::graphSpec& graph) {
    for (const bool input : {true, false}) {
        const auto& bound = input ? n.inputs : n.outputs;
        for (const auto& p : input ? spec.inputs : spec.outputs) {
            const auto it = bound.find(p.name);
            if (p.required && (it == bound.end() || it->second.empty())) {
                throw buildError(n.id, std::string(input ? "missing input " : "missing output ") +
                                           std::string(p.name));
            }
        }
        for (const auto& [name, refs] : bound) {
            if (refs.empty()) throw buildError(n.id, (input ? "input " : "output ") + name + " has no pipes");
        }
    }
    for (const auto& p : spec.params) {
        const auto it = n.params.find(p.name);
        if (it == n.params.end()) continue;
        const auto v = metadata::as<double>(it->second);
        if (!v) continue;
        // Written as a test for inclusion so that NaN fails it.
        const bool inRange = v->get() >= p.min && v->get() <= p.max;
        if (!inRange) throw buildError(n.id, "parameter " + std::string(p.name) + " is out of range");
    }
    if (const auto broken = metadata::checkConstraints(spec, n, graph, metadata::naming::ir);
        !broken.empty()) {
        throw buildError(n.id, broken.front());
    }
}

} // namespace

// --- nodeContext --------------------------------------------------------

metadata::optionalRef<paramValue> nodeContext::value(std::string_view name) const {
    const auto it = node_.params.find(name);
    if (it == node_.params.end()) return std::nullopt;
    return std::cref(it->second);
}

const metadata::paramSpec& nodeContext::paramSpec(std::string_view name) const {
    const auto p = spec_.param(name);
    if (!p) fail("internal: " + std::string(spec_.type) + " has no parameter " + std::string(name));
    return *p;
}

double nodeContext::number(std::string_view name) const {
    const auto& spec = paramSpec(name);
    const auto v = value(name);
    if (!v) return spec.defaultValue;
    if (const auto d = metadata::as<double>(*v)) return *d;
    fail("parameter " + std::string(name) + " must be a static number");
}

std::string nodeContext::text(std::string_view name) const {
    const auto& spec = paramSpec(name);
    const auto v = value(name);
    if (!v) return std::string(spec.defaultText);
    if (const auto s = metadata::as<std::string>(*v)) return s->get();
    fail("parameter " + std::string(name) + " must be text");
}

int nodeContext::choice(std::string_view name) const {
    const auto& spec = paramSpec(name);
    const std::string t = text(name);
    const auto it = std::ranges::find(spec.choices, t);
    if (it == spec.choices.end()) fail("parameter " + std::string(name) + ": unknown choice " + t);
    return static_cast<int>(it - spec.choices.begin());
}

std::vector<double> nodeContext::list(std::string_view name) const {
    paramSpec(name);
    const auto v = value(name);
    if (!v) return {};
    if (const auto l = metadata::as<std::vector<double>>(*v)) return l->get();
    fail("parameter " + std::string(name) + " must be a list of numbers");
}

std::vector<std::string> nodeContext::textList(std::string_view name) const {
    paramSpec(name);
    const auto v = value(name);
    if (!v) return {};
    if (const auto l = metadata::as<std::vector<std::string>>(*v)) return l->get();
    fail("parameter " + std::string(name) + " must be a list of strings");
}

std::optional<metadata::rgba> nodeContext::color(std::string_view name) const {
    const auto& spec = paramSpec(name);
    const auto v = value(name);
    if (!v) return spec.defaultColor;
    if (const auto t = metadata::as<std::string>(*v); t && t->get() == "none" && spec.noneAllowed)
        return std::nullopt;
    if (const auto l = metadata::as<std::vector<double>>(*v); l && metadata::isColor(l->get())) {
        const auto& c = l->get();
        return metadata::rgba{c[0], c[1], c[2], c[3]};
    }
    if (std::holds_alternative<pipeRef>(v->get()))
        fail("parameter " + std::string(name) + " must be a static color here");
    fail("parameter " + std::string(name) + " must be four numbers from 0 to 1" +
         (spec.noneAllowed ? " or none" : ""));
}

std::int32_t nodeContext::bufferIndex(std::string_view name) const {
    paramSpec(name);
    const auto v = value(name);
    const auto b = v ? metadata::as<metadata::bufferRef>(*v) : std::nullopt;
    if (!b) fail("parameter " + std::string(name) + " must name a buffer");
    if (!build_.buffers.contains(b->get().index)) fail(metadata::toString(b->get()) + " is not declared");
    return b->get().index;
}

std::shared_ptr<sampleBuffer> nodeContext::buffer(std::string_view name) const {
    return build_.buffers.at(bufferIndex(name));
}

const std::string& nodeContext::bufferId(std::string_view name) const {
    const auto index = bufferIndex(name);
    const auto it =
        std::ranges::find_if(graph_.buffers, [&](const auto& b) { return b.buffer.index == index; });
    return it->id;  // bufferIndex() found it declared
}

std::shared_ptr<const wavetableSet>& nodeContext::wavetables(std::string_view name, std::size_t size) {
    return build_.wavetables[{bufferIndex(name), size}];
}

frameOutput nodeContext::frameOut(std::string_view port) {
    const auto r = refs(false, port);
    if (!r || r->get().empty()) fail("output " + std::string(port) + " needs an event pipe");
    auto& ring = build_.frames[r->get().front()];
    if (ring) fail(metadata::toString(r->get().front()) + " already has a frame writer");
    ring = std::make_shared<frameRing>();
    return {.events = eventOut(port), .ring = ring};
}

frameInput nodeContext::frameIn(std::string_view port) {
    const auto r = refs(true, port);
    if (!r || r->get().empty()) fail("input " + std::string(port) + " needs an event pipe");
    const auto it = build_.frames.find(r->get().front());
    if (it == build_.frames.end() || !it->second->shaped()) {
        fail("input " + std::string(port) + " reads " + metadata::toString(r->get().front()) +
             ", which no frame output before it writes");
    }
    return {.events = eventIn(port), .ring = it->second};
}

image nodeContext::loadImage(std::string_view name) const {
    const std::string path = text(name);
    if (path.empty()) fail("parameter " + std::string(name) + " names no image file");
    if (!env_.loadImage) fail("this engine cannot load image files");
    std::string error;
    auto decoded = env_.loadImage(path, error);
    if (!decoded) fail("cannot load " + path + ": " + error);
    return std::move(*decoded);
}

std::shared_ptr<pipe> nodeContext::resolve(pipeRef ref) const {
    if (const auto end = chainEnds_.find(ref); end != chainEnds_.end()) ref = end->second;
    env_.pipes.make(ref);  // a chain's end may be named only by a later node
    auto p = env_.pipes.find(ref);
    if (!p) fail(metadata::toString(ref) + " is beyond the engine's pipe table");
    return p;
}

std::shared_ptr<pipe> nodeContext::resolveOutput(pipeRef ref) {
    if (const auto it = build_.privateOutputs.find(ref); it != build_.privateOutputs.end()) {
        build_.boundPrivate.insert(ref);
        return it->second;
    }
    return resolve(ref);
}

namespace {

// Binds @p s (a signal or a frameValue) to the audio or control pipe
// @p p; false for other kinds.
template <typename T>
bool bindTo(T& s, pipeRef ref, const std::shared_ptr<pipe>& p) {
    if (ref.kind == pipeKind::audio) {
        s.bind(std::static_pointer_cast<const aPipe>(p));
    } else if (ref.kind == pipeKind::control) {
        s.bind(std::static_pointer_cast<const cPipe>(p));
    } else {
        return false;
    }
    return true;
}

} // namespace

template <typename T, typename Make>
T nodeContext::numeric(std::string_view name, Make make) {
    const auto& spec = paramSpec(name);
    const auto v = value(name);
    if (!v) return make(static_cast<float>(spec.defaultValue));
    if (const auto d = metadata::as<double>(*v)) return make(static_cast<float>(d->get()));
    const auto r = metadata::as<pipeRef>(*v);
    if (!r) fail("parameter " + std::string(name) + " must be a number or a pipe");
    T bound = make(0.0f);
    if (!bindTo(bound, *r, resolve(*r)))
        fail("parameter " + std::string(name) + " cannot read " + metadata::toString(*r));
    return bound;
}

signal nodeContext::param(std::string_view name) {
    return numeric<signal>(name, [this](float x) { return signal(x, blockSize()); });
}

frameColor nodeContext::colorParam(std::string_view name) {
    paramSpec(name);
    if (const auto v = value(name)) {
        if (const auto r = metadata::as<pipeRef>(*v)) {
            if (r->get().kind != pipeKind::control)
                fail("parameter " + std::string(name) + " must follow a control pipe");
            return frameColor(std::static_pointer_cast<const cPipe>(resolve(*r)));
        }
    }
    return frameColor(color(name));
}

frameValue nodeContext::frameParam(std::string_view name) {
    return numeric<frameValue>(name, [](float x) { return frameValue(x); });
}

signal nodeContext::paramOr(std::string_view name, float fallback) {
    return spec_.param(name) ? param(name) : signal(fallback, blockSize());
}

metadata::optionalRef<std::vector<pipeRef>> nodeContext::refs(bool input, std::string_view port) const {
    const auto& ports = input ? node_.inputs : node_.outputs;
    const auto it = ports.find(port);
    if (it == ports.end() || it->second.empty()) return std::nullopt;
    return std::cref(it->second);
}

std::size_t nodeContext::inputCount(std::string_view port) const {
    const auto r = refs(true, port);
    return r ? r->get().size() : 0;
}

int nodeContext::declaredChannels(pipeRef ref) const { return channelsOf(graph_, ref); }

int nodeContext::outputChannels(std::string_view port) const {
    const auto r = refs(false, port);
    return r ? declaredChannels(r->get().front()) : 1;
}

int nodeContext::inputChannels(std::string_view port) const {
    const auto r = refs(true, port);
    return r && !r->get().empty() ? declaredChannels(r->get().front()) : 1;
}

// The first pipe bound to @p port, checked to be of @p kind, downcast to P.
template <typename P>
std::shared_ptr<P> nodeContext::bound(bool input, std::string_view port, pipeKind kind, std::string_view what) {
    const auto r = refs(input, port);
    if (!r) return nullptr;
    const pipeRef ref = r->get().front();
    if (ref.kind != kind) fail(std::string(input ? "input " : "output ") + std::string(port) + " must be " + std::string(what));
    return std::static_pointer_cast<P>(input ? resolve(ref) : resolveOutput(ref));
}

std::shared_ptr<const aPipe> nodeContext::audioIn(std::string_view port) {
    return bound<const aPipe>(true, port, pipeKind::audio, "an audio pipe");
}

audioOutput nodeContext::audioOut(std::string_view port) {
    return {.pipe = bound<aPipe>(false, port, pipeKind::audio, "an audio pipe"),
            .channels = outputChannels(port)};
}

std::shared_ptr<const ePipe> nodeContext::eventIn(std::string_view port) {
    return bound<const ePipe>(true, port, pipeKind::event, "an event pipe");
}

std::shared_ptr<const vPipe> nodeContext::videoIn(std::string_view port) {
    return bound<const vPipe>(true, port, pipeKind::video, "a video pipe");
}

std::shared_ptr<vPipe> nodeContext::videoOut(std::string_view port) {
    return bound<vPipe>(false, port, pipeKind::video, "a video pipe");
}

std::shared_ptr<ePipe> nodeContext::eventOut(std::string_view port) {
    return bound<ePipe>(false, port, pipeKind::event, "an event pipe");
}

std::vector<std::shared_ptr<ePipe>> nodeContext::eventOuts(std::string_view port) {
    std::vector<std::shared_ptr<ePipe>> out;
    if (const auto r = refs(false, port)) {
        for (auto ref : r->get()) {
            if (ref.kind != pipeKind::event) fail("output " + std::string(port) + " must be an event pipe");
            out.push_back(std::static_pointer_cast<ePipe>(resolve(ref)));
        }
    }
    return out;
}

controlOutput nodeContext::controlOut(std::string_view port) {
    return {.pipe = bound<cPipe>(false, port, pipeKind::control, "a control pipe"),
            .channels = outputChannels(port)};
}

signal nodeContext::signalIn(std::string_view port, std::size_t which) {
    signal s(0.0f, blockSize());
    const auto r = refs(true, port);
    if (r && which < r->get().size()) {
        const pipeRef ref = r->get()[which];
        if (!bindTo(s, ref, resolve(ref)))
            fail("input " + std::string(port) + " must be an audio or control pipe");
    }
    return s;
}

signalOut nodeContext::signalOutput(std::string_view port) {
    signalOut out;
    const auto r = refs(false, port);
    if (!r) return out;
    const pipeRef ref = r->get().front();
    out.channels = declaredChannels(ref);
    if (ref.kind == pipeKind::audio) {
        out.audio = std::static_pointer_cast<aPipe>(resolveOutput(ref));
    } else if (ref.kind == pipeKind::control) {
        out.control = std::static_pointer_cast<cPipe>(resolve(ref));
    } else {
        fail("output " + std::string(port) + " must be an audio or control pipe");
    }
    return out;
}

// --- Buffers (samples.md §5) ----------------------------------------------

namespace {

constexpr double bytesPerMegabyte = 1024.0 * 1024.0;

// @p bytes in whole megabytes, rounded up. A total past any real memory
// reads as "more than" a bound, so the cast to an integer stays defined.
std::string megabytes(double bytes) {
    constexpr double most = 1e12;  // megabytes
    const double mb = std::ceil(bytes / bytesPerMegabyte);
    if (!(mb <= most)) return "more than " + std::to_string(static_cast<long long>(most)) + " MB";
    return std::to_string(static_cast<long long>(mb)) + " MB";
}

// Why a plan whose memory, @p total bytes with what is already live,
// passes the pool's @p limit is refused (§5.6).
std::string memoryRefusal(double total, std::size_t limit) {
    return "the plan's buffers, frames, and spectral history need " + megabytes(total) +
           " with those already playing; the server allows " + megabytes(static_cast<double>(limit)) +
           " (--buffer-memory)";
}

[[noreturn]] void failBuffer(const metadata::bufferDecl& d, const std::string& message) {
    throw buildError(buildError::subject::buffer, d.id, message);
}

// A buffer the plan makes rather than adopts.
struct newBuffer {
    std::size_t at = 0;  // position in the plan's buffers
    bufferPool::key key;
    int channels = 1;
    double sampleRate = 0.0;
    std::size_t frames = 0;
    std::shared_ptr<const decodedAudio> audio;
};

// The buffers @p graph declares, in order: a live buffer of the same ID,
// shape, and file is adopted with its contents (§5.4); the others are made
// silent and filled from their files. Nothing is allocated until the whole
// set fits the pool's bound with the buffers already alive (§5.6).
std::vector<std::shared_ptr<sampleBuffer>> makeBuffers(const metadata::graphSpec& graph, buildEnv& env) {
    std::vector<std::shared_ptr<sampleBuffer>> out(graph.buffers.size());
    std::vector<newBuffer> made;
    double needed = 0.0;  // in double: a long declaration must not overflow
    for (std::size_t i = 0; i < graph.buffers.size(); ++i) {
        const auto& d = graph.buffers[i];
        auto key = bufferPool::keyOf(d);
        if (auto live = env.buffers.find(key)) {
            out[i] = std::move(live);
            continue;
        }
        std::shared_ptr<const decodedAudio> audio;
        if (d.file) {
            if (!env.loadSample) failBuffer(d, "this engine cannot load audio files");
            std::string error;
            audio = env.loadSample(*d.file, d.resample ? env.info.sampleRate : 0.0, error);
            if (!audio) failBuffer(d, "cannot load " + *d.file + ": " + error);
            if (audio->frames() == 0 && !d.seconds) failBuffer(d, *d.file + " has no audio");
        }
        // A buffer that keeps its file's samples keeps its rate (§4.5.1),
        // and counts its seconds at it.
        const double rate = audio && !d.resample ? audio->sampleRate : env.info.sampleRate;
        if (!(rate > 0.0)) failBuffer(d, "cannot tell the sample rate of " + d.file.value_or("its file"));
        const double frames =
            d.seconds ? std::round(*d.seconds * rate) : static_cast<double>(audio ? audio->frames() : 0);
        const double bytes = frames * static_cast<double>(bufferBytes(d.channels, 1));
        needed += bytes;
        if (needed > static_cast<double>(env.buffers.limit())) continue;  // refused below; no size_t from it
        made.push_back({.at = i,
                        .key = std::move(key),
                        .channels = d.channels,
                        .sampleRate = rate,
                        .frames = static_cast<std::size_t>(std::max(1.0, frames)),
                        .audio = std::move(audio)});
    }
    const double total = static_cast<double>(env.buffers.liveBytes()) + needed;
    if (total > static_cast<double>(env.buffers.limit())) {
        throw buildError({}, memoryRefusal(total, env.buffers.limit()));
    }
    for (auto& m : made) {
        auto b = std::make_shared<sampleBuffer>(m.channels, m.frames, m.sampleRate);
        if (m.audio) b->fill(*m.audio);
        env.buffers.add(m.key, b);
        out[m.at] = std::move(b);
    }
    return out;
}

}  // namespace

double nodeContext::memoryWith(std::size_t bytes) const noexcept {
    return static_cast<double>(build_.live) + static_cast<double>(build_.reserved) +
           static_cast<double>(bytes);
}

void nodeContext::reserve(std::size_t bytes) {
    if (!tryReserve(bytes)) fail(memoryRefusal(memoryWith(bytes), env_.buffers.limit()));
}

bool nodeContext::tryReserve(std::size_t bytes) noexcept {
    if (memoryWith(bytes) > static_cast<double>(env_.buffers.limit())) return false;
    build_.reserved += bytes;
    return true;
}

// --- plan ---------------------------------------------------------------

namespace {

// The buffers a plan being built marked written (samples.md §6.6), unmarked
// again unless keep() is called: a refused plan leaves live buffers as they
// were, but for levels it dropped, which a later reader makes again.
class writtenMarks {
public:
    writtenMarks() = default;
    writtenMarks(const writtenMarks&) = delete;
    writtenMarks& operator=(const writtenMarks&) = delete;
    writtenMarks(writtenMarks&&) = delete;
    writtenMarks& operator=(writtenMarks&&) = delete;
    ~writtenMarks() {
        for (sampleBuffer& b : marked_) b.unmarkWritten();
    }

    // Marks @p b written, if it was not.
    void mark(sampleBuffer& b) {
        if (b.written()) return;
        b.markWritten();
        marked_.emplace_back(b);
    }
    // The plan was built: its marks stay.
    void keep() noexcept { marked_.clear(); }

private:
    std::vector<std::reference_wrapper<sampleBuffer>> marked_;
};

// Adds the private pipes of a shared audio pipe's writers into it, in the
// writers' order (threads.md §4.2). Each writer added its block once into
// a cleared private pipe, so the sum has the bits the writers would have
// left in the shared pipe themselves. A private pipe that forwards
// (aPipe::forward) holds nothing: its writer added into the shared pipe
// itself, in this order (§4.2.5). A group forwards all or none: the
// builder gives every pipe of a group a target or none, and the engine
// turns forwarding on and off for every private pipe of a plan at once.
class summing final : public primitive {
public:
    summing(pipeRef shared, std::shared_ptr<aPipe> out, std::vector<std::shared_ptr<const aPipe>> in)
        : primitive(metadata::toString(shared) + " sum", "sum"), out_(std::move(out)), in_(std::move(in)) {}

    void process(const blockContext& /*ctx*/) noexcept ESPRESSO_NONBLOCKING override {
        // The first pipe tells for the group, so the others are not read:
        // a plan can have hundreds, each in a cache line of its own.
        if (in_.empty() || in_.front()->forwarding()) return;
        for (const auto& p : in_) {
            for (int c = 0; c < p->channels(); ++c) emit(*out_, c, p->read(c));
        }
    }

private:
    std::shared_ptr<aPipe> out_;
    std::vector<std::shared_ptr<const aPipe>> in_;
};

// The private pipes of a graph's audio writers (threads.md §4.2), by node
// index: every writer of an audio pipe but its first gets one, of the
// pipe's declared channels and with no feedback buffer; the shared pipes
// summed after each node, its last writer; and the shared pipes whose
// private pipes may forward (§4.2.5), sorted.
struct privatePlan {
    std::vector<std::map<pipeRef, std::shared_ptr<aPipe>>> outputs;
    std::vector<std::vector<pipeRef>> sumsAfter;
    std::vector<pipeRef> forwards;
};

// A writer of an audio pipe: its node, and whether it names the pipe at
// more than one output, and so adds to it more than once a block.
struct audioWriter {
    std::size_t node = 0;
    bool repeats = false;
};

// True when the private pipes of shared pipe @p ref, whose writers are
// @p writers in plan order, may forward (threads.md §4.2.5): every writer
// with a private pipe adds once a block, and no node from the first such
// writer to the last writer reads @p ref, apart from reads of a feedback
// pipe, which take the other buffer. Run alone in plan order, each writer
// then adds its samples into the shared pipe where the summing node would
// have added them, and nothing reads the pipe in between. A first writer
// that adds twice writes the shared pipe itself either way. @p readers
// holds the nodes that read each audio pipe.
bool mayForward(const metadata::graphSpec& graph, pipeRef ref, std::span<const audioWriter> writers,
                const std::map<pipeRef, std::vector<std::size_t>>& readers) {
    if (std::ranges::any_of(writers.subspan(1), &audioWriter::repeats)) return false;
    if (const auto d = metadata::findPipe(graph, ref); d && d->get().feedback) return true;
    const auto it = readers.find(ref);
    if (it == readers.end()) return true;
    const std::size_t from = writers[1].node;
    const std::size_t to = writers.back().node;
    return std::ranges::none_of(it->second, [&](std::size_t r) { return r >= from && r <= to; });
}

privatePlan privateOutputs(const metadata::graphSpec& graph, const prepareInfo& info) {
    std::map<pipeRef, std::vector<audioWriter>> writers;
    std::map<pipeRef, std::vector<std::size_t>> readers;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        // A pipe named at two outputs gets one private pipe, which the node
        // adds to twice a block: counted before the names are made unique.
        auto writes = metadata::pipesWritten(graph.nodes[i]);
        std::ranges::sort(writes);
        for (auto at = writes.begin(); at != writes.end();) {
            const pipeRef r = *at;
            const auto next = std::ranges::find_if(at, writes.end(), [r](pipeRef w) { return w != r; });
            if (r.kind == pipeKind::audio) writers[r].push_back({.node = i, .repeats = next - at > 1});
            at = next;
        }
        for (const auto r : metadata::pipesRead(graph.nodes[i])) {
            if (r.kind == pipeKind::audio) readers[r].push_back(i);
        }
    }
    privatePlan out{.outputs = std::vector<std::map<pipeRef, std::shared_ptr<aPipe>>>(graph.nodes.size()),
                    .sumsAfter = std::vector<std::vector<pipeRef>>(graph.nodes.size()),
                    .forwards = {}};
    for (const auto& [ref, nodes] : writers) {
        if (nodes.size() < 2) continue;
        const int channels = channelsOf(graph, ref);
        for (std::size_t k = 1; k < nodes.size(); ++k) {
            auto p = std::make_shared<aPipe>(info.blockSize, channels, false);
            p->configure(channels, false);
            out.outputs[nodes[k].node].emplace(ref, std::move(p));
        }
        out.sumsAfter[nodes.back().node].push_back(ref);
        if (mayForward(graph, ref, nodes, readers)) out.forwards.push_back(ref);  // in order: a map's keys
    }
    return out;
}

}  // namespace

bool plan::isFeedback(pipeRef ref) const noexcept { return std::ranges::binary_search(feedback, ref); }

std::shared_ptr<plan> buildPlan(const metadata::graphSpec& graph, std::int32_t planId, buildEnv env) {
    auto result = std::make_shared<plan>();
    result->id = planId;
    result->tempo = graph.tempo;
    result->meter = graph.meter;
    result->pagePort = graph.pagePort;
    result->pipes = graph.pipes;
    result->buffers = makeBuffers(graph, env);
    planBuild build;
    build.live = env.buffers.liveBytes();  // this plan's buffers included
    for (std::size_t i = 0; i < graph.buffers.size(); ++i)
        build.buffers[graph.buffers[i].buffer.index] = result->buffers[i];
    // Before any reader is made: a buffer this plan writes gets no levels
    // (samples.md §6.6). A refused plan takes its marks back.
    writtenMarks marks;
    for (const auto& [b, writers] : metadata::writersByBuffer(graph)) {
        if (const auto it = build.buffers.find(b.index); it != build.buffers.end()) marks.mark(*it->second);
    }
    for (const auto& d : graph.pipes) {
        if (!env.pipes.contains(d.pipe)) throw buildError({}, metadata::toString(d.pipe) + " is beyond the engine's pipe table");
        env.pipes.make(d.pipe);
        result->used.push_back(d.pipe);
        if (d.feedback) result->feedback.push_back(d.pipe);
    }
    std::sort(result->feedback.begin(), result->feedback.end());
    const writerIds writers = audioWriters(graph);
    const auto ends = chainEnds(graph);
    auto privates = privateOutputs(graph, env.info);
    // The private pipes each shared pipe's writers bound, in order.
    std::map<pipeRef, std::vector<std::shared_ptr<const aPipe>>> sumInputs;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        const auto& n = graph.nodes[i];
        const auto spec = metadata::findPrimitive(n.type);
        const auto f = findFactory(n.type);
        if (!spec || !f) throw buildError(n.id, "the engine has no primitive " + n.type);
        checkBuildable(n, *spec, graph);
        // Every pipe the node names exists before it binds them, and before
        // the plan plays: a port it leaves unbound still starts its blocks.
        auto reads = metadata::pipesRead(n);
        auto writes = metadata::pipesWritten(n);
        for (const auto r : reads) env.pipes.make(r);
        for (const auto w : writes) env.pipes.make(w);
        build.privateOutputs = std::move(privates.outputs[i]);
        build.boundPrivate.clear();
        nodeContext ctx(n, *spec, graph, env, ends, build);
        auto node = f->get().make(ctx);
        node->prepare(env.info);
        if (isVideo(reads, writes)) {
            // videoOut draws nothing: the plan records the frame it shows.
            if (n.type != "videoOut") {
                result->videoNodes.push_back(std::move(node));
            } else if (!result->display) {
                result->display =
                    std::static_pointer_cast<const vPipe>(env.pipes.find(n.inputs.at("in").front()));
            }
            continue;
        }
        nodeInfo info{.shape = shapeKey(n, graph, writers),
                      .reads = std::move(reads),
                      .writes = std::move(writes),
                      .bufferReads = {},
                      .bufferWrites = {},
                      .ports = {},
                      .writesDevice = ctx.writesDevice(),
                      .reportsFaults = ctx.reportsFaults(),
                      .privateWrites = {},
                      .privateReads = {},
                      .cost = costOf(n.type) * env.info.blockSize};
        const auto access = metadata::buffersOf(n);
        for (const auto& [refs, out] : {std::pair{std::cref(access.reads), std::ref(info.bufferReads)},
                                        std::pair{std::cref(access.writes), std::ref(info.bufferWrites)}}) {
            for (const auto b : refs.get()) {
                if (const auto it = build.buffers.find(b.index); it != build.buffers.end())
                    out.get().push_back(it->second);
            }
        }
        result->used.insert(result->used.end(), info.reads.begin(), info.reads.end());
        result->used.insert(result->used.end(), info.writes.begin(), info.writes.end());
        const auto ports = ctx.takePorts();
        info.ports.assign(ports.begin(), ports.end());
        result->ports.insert(result->ports.end(), ports.begin(), ports.end());
        // A private pipe the node did not bind (an unused port) is dropped.
        // Every private pipe of a group that may forward gets its target:
        // forwarding part of a group would reorder its sum (§4.2.5).
        for (const auto ref : build.boundPrivate) {
            const auto& p = build.privateOutputs.at(ref);
            if (std::ranges::binary_search(privates.forwards, ref) && env.pipes.contains(ref))
                p->forwardTo(env.pipes.audio(ref.index));
            info.privateWrites.push_back({.shared = ref, .pipe = p});
            sumInputs[ref].push_back(p);
            result->privatePipes.push_back(p);
        }
        result->nodes.push_back(std::move(node));
        result->info.push_back(std::move(info));
        // After a shared pipe's last writer, its summing node.
        for (const auto ref : privates.sumsAfter[i]) {
            const auto in = sumInputs.find(ref);
            if (in == sumInputs.end()) continue;
            result->nodes.push_back(std::make_shared<summing>(
                ref, std::static_pointer_cast<aPipe>(env.pipes.find(ref)), in->second));
            // It writes the shared pipe and reads only the private ones.
            result->info.push_back({.shape = "sum",
                                    .reads = {},
                                    .writes = {ref},
                                    .bufferReads = {},
                                    .bufferWrites = {},
                                    .ports = {},
                                    .writesDevice = false,
                                    .reportsFaults = false,
                                    .privateWrites = {},
                                    .privateReads = in->second,
                                    .cost = costOf("sum") * env.info.blockSize});
        }
    }
    auto& used = result->used;
    std::ranges::sort(used);
    used.erase(std::ranges::unique(used).begin(), used.end());
    // The tempo pipe persists and is never zeroed or swapped.
    std::erase(used, pipeRef{.kind = pipeKind::control, .index = metadata::tempoPipe});
    result->seconds.assign(result->nodes.size(), 0.0);
    const std::vector<std::reference_wrapper<const nodeInfo>> order(result->info.begin(), result->info.end());
    result->schedule = scheduleOf(order, result->feedback, result->used, env.schedule);
    marks.keep();
    return result;
}

std::vector<std::string_view> registeredTypes() {
    std::vector<std::string_view> out;
    for (const auto& spec : metadata::primitiveSpecs()) {
        if (findFactory(spec.type)) out.push_back(spec.type);
    }
    return out;
}

} // namespace primitives
