// Plan building: primitive construction from node descriptions, pipe
// resolution, and the data swaps need (primitives.md §9, §10).

#include "node.h"

#include <algorithm>
#include <map>

namespace primitives {

using metadata::paramValue;
using metadata::pipeKind;
using metadata::pipeRef;

namespace {

metadata::optionalRef<factoryEntry> findFactory(std::string_view type) {
    static const std::vector<factoryEntry> all = [] {
        std::vector<factoryEntry> v;
        for (auto table : {oscillatorFactories(), filterFactories(), modulationFactories(), delayFactories(),
                           mathFactories(), eventFactories(), deviceFactories()}) {
            v.insert(v.end(), table.begin(), table.end());
        }
        return v;
    }();
    const auto it = std::find_if(all.begin(), all.end(), [&](const factoryEntry& f) { return f.type == type; });
    if (it == all.end()) return std::nullopt;
    return std::cref(*it);
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
    for (auto& [ref, ids] : out) std::sort(ids.begin(), ids.end());
    return out;
}

// Type, parameter bindings, port shapes, and the nodes feeding each audio
// input (§9.2.2). Equal keys mean the incoming node can take over the
// outgoing node's state; a node whose audio comes from different nodes
// crossfades instead of switching sources at once.
std::string shapeKey(const metadata::nodeSpec& n, const metadata::graphSpec& g, const writerIds& writers) {
    auto channels = [&](pipeRef r) {
        const auto d = metadata::findPipe(g, r);
        return d ? d->get().channels : 1;
    };
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
            for (auto r : refs) key += ':' + std::string(metadata::toString(r.kind)) + std::to_string(channels(r));
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
    if (const auto s = metadata::as<std::string>(*v)) return *s;
    fail("parameter " + std::string(name) + " must be text");
}

int nodeContext::choice(std::string_view name) const {
    const auto& spec = paramSpec(name);
    const std::string t = text(name);
    const auto it = std::find(spec.choices.begin(), spec.choices.end(), t);
    if (it == spec.choices.end()) fail("parameter " + std::string(name) + ": unknown choice " + t);
    return static_cast<int>(it - spec.choices.begin());
}

std::vector<double> nodeContext::list(std::string_view name) const {
    paramSpec(name);
    const auto v = value(name);
    if (!v) return {};
    if (const auto l = metadata::as<std::vector<double>>(*v)) return *l;
    fail("parameter " + std::string(name) + " must be a list of numbers");
}

std::shared_ptr<pipe> nodeContext::resolve(pipeRef ref) const {
    auto p = env_.pipes.find(ref);
    if (!p) fail(metadata::toString(ref) + " is beyond the engine's pipe table");
    return p;
}

namespace {

// Binds @p s to the audio or control pipe @p p; false for other kinds.
bool bindSignal(signal& s, pipeRef ref, std::shared_ptr<pipe> p) {
    if (ref.kind == pipeKind::audio) {
        s.bind(std::static_pointer_cast<const aPipe>(std::move(p)));
    } else if (ref.kind == pipeKind::control) {
        s.bind(std::static_pointer_cast<const cPipe>(std::move(p)));
    } else {
        return false;
    }
    return true;
}

} // namespace

signal nodeContext::param(std::string_view name) {
    const auto& spec = paramSpec(name);
    signal s;
    s.setConstant(static_cast<float>(spec.defaultValue));
    if (const auto v = value(name)) {
        if (const auto d = metadata::as<double>(*v)) {
            s.setConstant(static_cast<float>(d->get()));
        } else if (const auto r = metadata::as<pipeRef>(*v)) {
            if (!bindSignal(s, *r, resolve(*r))) fail("parameter " + std::string(name) + " cannot read " + metadata::toString(*r));
        } else {
            fail("parameter " + std::string(name) + " must be a number or a pipe");
        }
    }
    s.prepare(env_.info.blockSize);
    return s;
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

int nodeContext::declaredChannels(pipeRef ref) const {
    const auto d = metadata::findPipe(graph_, ref);
    return d ? d->get().channels : 1;
}

int nodeContext::outputChannels(std::string_view port) const {
    const auto r = refs(false, port);
    return r ? declaredChannels(r->get().front()) : 1;
}

// The first pipe bound to @p port, checked to be of @p kind, downcast to P.
template <typename P>
std::shared_ptr<P> nodeContext::bound(bool input, std::string_view port, pipeKind kind, std::string_view what) {
    const auto r = refs(input, port);
    if (!r) return nullptr;
    const pipeRef ref = r->get().front();
    if (ref.kind != kind) fail(std::string(input ? "input " : "output ") + std::string(port) + " must be " + std::string(what));
    return std::static_pointer_cast<P>(resolve(ref));
}

std::shared_ptr<const aPipe> nodeContext::audioIn(std::string_view port) {
    return bound<const aPipe>(true, port, pipeKind::audio, "an audio pipe");
}

std::shared_ptr<aPipe> nodeContext::audioOut(std::string_view port) {
    return bound<aPipe>(false, port, pipeKind::audio, "an audio pipe");
}

std::shared_ptr<const ePipe> nodeContext::eventIn(std::string_view port) {
    return bound<const ePipe>(true, port, pipeKind::event, "an event pipe");
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

std::shared_ptr<cPipe> nodeContext::controlOut(std::string_view port) {
    return bound<cPipe>(false, port, pipeKind::control, "a control pipe");
}

signal nodeContext::signalIn(std::string_view port, std::size_t which) {
    signal s;
    const auto r = refs(true, port);
    if (r && which < r->get().size()) {
        const pipeRef ref = r->get()[which];
        if (!bindSignal(s, ref, resolve(ref))) fail("input " + std::string(port) + " must be an audio or control pipe");
    }
    s.prepare(env_.info.blockSize);
    return s;
}

signalOut nodeContext::signalOutput(std::string_view port) {
    signalOut out;
    const auto r = refs(false, port);
    if (!r) return out;
    const pipeRef ref = r->get().front();
    out.channels = declaredChannels(ref);
    if (ref.kind == pipeKind::audio) {
        out.audio = std::static_pointer_cast<aPipe>(resolve(ref));
    } else if (ref.kind == pipeKind::control) {
        out.control = std::static_pointer_cast<cPipe>(resolve(ref));
    } else {
        fail("output " + std::string(port) + " must be an audio or control pipe");
    }
    return out;
}

// --- plan ---------------------------------------------------------------

bool plan::isFeedback(pipeRef ref) const noexcept {
    return std::binary_search(feedback.begin(), feedback.end(), ref);
}

std::shared_ptr<plan> buildPlan(const metadata::graphSpec& graph, std::int32_t planId, buildEnv env) {
    auto result = std::make_shared<plan>();
    result->id = planId;
    result->tempo = graph.tempo;
    result->meter = graph.meter;
    result->pipes = graph.pipes;
    for (const auto& d : graph.pipes) {
        if (!env.pipes.contains(d.pipe)) throw buildError({}, metadata::toString(d.pipe) + " is beyond the engine's pipe table");
        result->used.push_back(d.pipe);
        if (d.feedback) result->feedback.push_back(d.pipe);
    }
    std::sort(result->feedback.begin(), result->feedback.end());
    const writerIds writers = audioWriters(graph);
    for (const auto& n : graph.nodes) {
        const auto spec = metadata::findPrimitive(n.type);
        const auto f = findFactory(n.type);
        if (!spec || !f) throw buildError(n.id, "the engine has no primitive " + n.type);
        nodeContext ctx(n, *spec, graph, env);
        auto node = f->get().make(ctx);
        node->prepare(env.info);
        nodeInfo info{shapeKey(n, graph, writers), metadata::pipesRead(n), metadata::pipesWritten(n)};
        result->used.insert(result->used.end(), info.reads.begin(), info.reads.end());
        result->used.insert(result->used.end(), info.writes.begin(), info.writes.end());
        const auto ports = ctx.takePorts();
        result->ports.insert(result->ports.end(), ports.begin(), ports.end());
        result->nodes.push_back(std::move(node));
        result->info.push_back(std::move(info));
    }
    auto& used = result->used;
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());
    // The tempo pipe persists and is never zeroed or swapped.
    std::erase(used, pipeRef{pipeKind::control, metadata::tempoPipe});
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
