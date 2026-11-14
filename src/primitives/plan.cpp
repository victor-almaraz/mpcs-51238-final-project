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

const factoryEntry* findFactory(std::string_view type) {
    static const std::vector<factoryEntry> all = [] {
        std::vector<factoryEntry> v;
        for (auto table : {oscillatorFactories(), filterFactories(), modulationFactories(), delayFactories(),
                           mathFactories(), eventFactories(), deviceFactories()}) {
            v.insert(v.end(), table.begin(), table.end());
        }
        return v;
    }();
    const auto it = std::find_if(all.begin(), all.end(), [&](const factoryEntry& f) { return f.type == type; });
    return it == all.end() ? nullptr : &*it;
}

// Type, parameter bindings, and port shapes (§9.2.2). Equal keys mean the
// incoming node can take over the outgoing node's state.
std::string shapeKey(const metadata::nodeSpec& n, const metadata::graphSpec& g) {
    auto channels = [&](pipeRef r) {
        const auto* d = metadata::findPipe(g, r);
        return d ? d->channels : 1;
    };
    std::string key = n.type;
    for (const auto& [name, v] : n.params) {
        key += '|' + name + ':';
        if (const auto* r = std::get_if<pipeRef>(&v)) {
            key += metadata::toString(r->kind);
        } else if (const auto* l = std::get_if<std::vector<double>>(&v)) {
            key += "list" + std::to_string(l->size());
        } else {
            key += "static";
        }
    }
    for (const auto* ports : {&n.inputs, &n.outputs}) {
        key += ports == &n.inputs ? "|in" : "|out";
        for (const auto& [name, refs] : *ports) {
            key += '|' + name;
            for (auto r : refs) key += ':' + std::string(metadata::toString(r.kind)) + std::to_string(channels(r));
        }
    }
    return key;
}

} // namespace

// --- nodeContext --------------------------------------------------------

const paramValue* nodeContext::value(std::string_view name) const {
    const auto it = node_.params.find(name);
    return it == node_.params.end() ? nullptr : &it->second;
}

const metadata::paramSpec& nodeContext::paramSpec(std::string_view name) const {
    const auto* p = spec_.param(name);
    if (!p) fail("internal: " + std::string(spec_.type) + " has no parameter " + std::string(name));
    return *p;
}

double nodeContext::number(std::string_view name) const {
    const auto& spec = paramSpec(name);
    const auto* v = value(name);
    if (!v) return spec.defaultValue;
    if (const auto* d = std::get_if<double>(v)) return *d;
    fail("parameter " + std::string(name) + " must be a static number");
}

std::string nodeContext::text(std::string_view name) const {
    const auto& spec = paramSpec(name);
    const auto* v = value(name);
    if (!v) return std::string(spec.defaultText);
    if (const auto* s = std::get_if<std::string>(v)) return *s;
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
    const auto* v = value(name);
    if (!v) return {};
    if (const auto* l = std::get_if<std::vector<double>>(v)) return *l;
    fail("parameter " + std::string(name) + " must be a list of numbers");
}

pipe* nodeContext::resolve(pipeRef ref) const {
    pipe* p = env_.pipes.find(ref);
    if (!p) fail(metadata::toString(ref) + " is beyond the engine's pipe table");
    return p;
}

signal nodeContext::param(std::string_view name) {
    const auto& spec = paramSpec(name);
    signal s;
    s.setConstant(static_cast<float>(spec.defaultValue));
    if (const auto* v = value(name)) {
        if (const auto* d = std::get_if<double>(v)) {
            s.setConstant(static_cast<float>(*d));
        } else if (const auto* r = std::get_if<pipeRef>(v)) {
            pipe* p = resolve(*r);
            if (r->kind == pipeKind::audio) {
                s.bind(static_cast<const aPipe*>(p));
            } else if (r->kind == pipeKind::control) {
                s.bind(static_cast<const cPipe*>(p));
            } else {
                fail("parameter " + std::string(name) + " cannot read " + metadata::toString(*r));
            }
        } else {
            fail("parameter " + std::string(name) + " must be a number or a pipe");
        }
    }
    s.prepare(env_.info.blockSize);
    return s;
}

const std::vector<pipeRef>* nodeContext::refs(bool input, std::string_view port) const {
    const auto& ports = input ? node_.inputs : node_.outputs;
    const auto it = ports.find(port);
    return it == ports.end() || it->second.empty() ? nullptr : &it->second;
}

std::size_t nodeContext::inputCount(std::string_view port) const {
    const auto* r = refs(true, port);
    return r ? r->size() : 0;
}

int nodeContext::declaredChannels(pipeRef ref) const {
    const auto* d = metadata::findPipe(graph_, ref);
    return d ? d->channels : 1;
}

int nodeContext::outputChannels(std::string_view port) const {
    const auto* r = refs(false, port);
    return r ? declaredChannels(r->front()) : 1;
}

const aPipe* nodeContext::audioIn(std::string_view port) {
    const auto* r = refs(true, port);
    if (!r) return nullptr;
    if (r->front().kind != pipeKind::audio) fail("input " + std::string(port) + " must be an audio pipe");
    return static_cast<const aPipe*>(resolve(r->front()));
}

aPipe* nodeContext::audioOut(std::string_view port) {
    const auto* r = refs(false, port);
    if (!r) return nullptr;
    if (r->front().kind != pipeKind::audio) fail("output " + std::string(port) + " must be an audio pipe");
    return static_cast<aPipe*>(resolve(r->front()));
}

const ePipe* nodeContext::eventIn(std::string_view port) {
    const auto* r = refs(true, port);
    if (!r) return nullptr;
    if (r->front().kind != pipeKind::event) fail("input " + std::string(port) + " must be an event pipe");
    return static_cast<const ePipe*>(resolve(r->front()));
}

ePipe* nodeContext::eventOut(std::string_view port) {
    const auto outs = eventOuts(port);
    return outs.empty() ? nullptr : outs.front();
}

std::vector<ePipe*> nodeContext::eventOuts(std::string_view port) {
    std::vector<ePipe*> out;
    if (const auto* r = refs(false, port)) {
        for (auto ref : *r) {
            if (ref.kind != pipeKind::event) fail("output " + std::string(port) + " must be an event pipe");
            out.push_back(static_cast<ePipe*>(resolve(ref)));
        }
    }
    return out;
}

cPipe* nodeContext::controlOut(std::string_view port) {
    const auto* r = refs(false, port);
    if (!r) return nullptr;
    if (r->front().kind != pipeKind::control) fail("output " + std::string(port) + " must be a control pipe");
    return static_cast<cPipe*>(resolve(r->front()));
}

signal nodeContext::signalIn(std::string_view port, std::size_t which) {
    signal s;
    const auto* r = refs(true, port);
    if (r && which < r->size()) {
        const pipeRef ref = (*r)[which];
        pipe* p = resolve(ref);
        if (ref.kind == pipeKind::audio) {
            s.bind(static_cast<const aPipe*>(p));
        } else if (ref.kind == pipeKind::control) {
            s.bind(static_cast<const cPipe*>(p));
        } else {
            fail("input " + std::string(port) + " must be an audio or control pipe");
        }
    }
    s.prepare(env_.info.blockSize);
    return s;
}

signalOut nodeContext::signalOutput(std::string_view port) {
    signalOut out;
    const auto* r = refs(false, port);
    if (!r) return out;
    const pipeRef ref = r->front();
    pipe* p = resolve(ref);
    out.channels = declaredChannels(ref);
    if (ref.kind == pipeKind::audio) {
        out.audio = static_cast<aPipe*>(p);
    } else if (ref.kind == pipeKind::control) {
        out.control = static_cast<cPipe*>(p);
    } else {
        fail("output " + std::string(port) + " must be an audio or control pipe");
    }
    return out;
}

// --- plan ---------------------------------------------------------------

bool plan::isFeedback(pipeRef ref) const noexcept {
    return std::binary_search(feedback.begin(), feedback.end(), ref);
}

std::unique_ptr<plan> buildPlan(const metadata::graphSpec& graph, std::int32_t planId, buildEnv env) {
    auto result = std::make_unique<plan>();
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
    for (const auto& n : graph.nodes) {
        const auto* spec = metadata::findPrimitive(n.type);
        const auto* f = findFactory(n.type);
        if (!spec || !f) throw buildError(n.id, "the engine has no primitive " + n.type);
        nodeContext ctx(n, *spec, graph, env);
        auto node = f->make(ctx);
        node->prepare(env.info);
        nodeInfo info{shapeKey(n, graph), metadata::pipesRead(n), metadata::pipesWritten(n)};
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
