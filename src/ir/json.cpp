// IR JSON reading and writing (interpreter.md §12.1).

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>
#include <espresso/metadata/text.h>

namespace ir {

namespace {

using metadata::bufferDecl;
using metadata::bufferRef;
using metadata::graphSpec;
using metadata::nodeSpec;
using metadata::paramValue;
using metadata::pipeRef;
using nlohmann::json;

// --- Schema validation --------------------------------------------------

class collectErrors : public nlohmann::json_schema::basic_error_handler {
public:
    void error(const json::json_pointer& ptr, const json&, const std::string& message) override {
        basic_error_handler::error(ptr, json(), message);
        found.emplace_back(ptr.to_string(), message);
    }
    std::vector<std::pair<std::string, std::string>> found;
};

const nlohmann::json_schema::json_validator& validator() {
    static const nlohmann::json_schema::json_validator v(json::parse(schema()));
    return v;
}

// --- JSON to graph ------------------------------------------------------

// Thrown when a schema-valid document cannot be converted; caught by read().
struct conversionError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

pipeRef toRef(const json& j) {
    // A reference is an object with one member, {"<kind>": index}.
    if (!j.is_object() || j.size() != 1) throw conversionError("a pipe reference must have one member");
    const auto member = j.begin();
    const auto kind = metadata::parsePipeKind(member.key());
    if (!kind) throw conversionError("unknown pipe type: " + member.key());
    return {.kind = *kind, .index = member.value().get<std::int32_t>()};
}

json fromRef(pipeRef r) { return json{{std::string(metadata::toString(r.kind)), r.index}}; }

std::vector<pipeRef> toRefs(const json& j) {
    std::vector<pipeRef> out;
    if (j.is_array()) {
        for (const auto& r : j) out.push_back(toRef(r));
    } else {
        out.push_back(toRef(j));
    }
    return out;
}

paramValue toParam(const json& j) {
    if (j.is_number()) return j.get<double>();
    if (j.is_string()) return j.get<std::string>();
    if (j.is_array()) return j.get<std::vector<double>>();
    if (j.contains("buffer")) return bufferRef{.index = j.at("buffer").get<std::int32_t>()};
    return toRef(j);
}

json fromParam(const paramValue& v) {
    return std::visit(
        [](const auto& x) -> json {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, pipeRef>) {
                return fromRef(x);
            } else if constexpr (std::is_same_v<T, bufferRef>) {
                return json{{"buffer", x.index}};
            } else {
                return x;
            }
        },
        v);
}

// The span @p j describes, or nothing when a member is missing or of the
// wrong type. Raises no JSON errors: it also reads spans of documents that
// failed schema validation, to attribute their errors.
std::optional<metadata::sourceSpan> toSpan(const json& j) {
    if (!j.is_object()) return std::nullopt;
    const auto file = j.find("file");
    const auto line = j.find("line");
    const auto column = j.find("column");
    if (file == j.end() || !file->is_string() || line == j.end() || !line->is_number_integer() ||
        column == j.end() || !column->is_number_integer()) {
        return std::nullopt;
    }
    // An optional integer member: zero when absent, nothing when mistyped.
    const auto integerOr0 = [&](const std::string& name) -> std::optional<std::int64_t> {
        const auto it = j.find(name);
        if (it == j.end()) return std::int64_t{0};
        if (!it->is_number_integer()) return std::nullopt;
        return it->get<std::int64_t>();
    };
    const auto offset = integerOr0("offset");
    const auto length = integerOr0("length");
    if (!offset || !length) return std::nullopt;
    return metadata::sourceSpan{.file = file->get<std::string>(),
                                .line = line->get<int>(),
                                .column = column->get<int>(),
                                .offset = *offset,
                                .length = *length};
}

json fromSpan(const metadata::sourceSpan& s) {
    return {
        {"file", s.file}, {"line", s.line}, {"column", s.column}, {"offset", s.offset}, {"length", s.length}};
}

json fromPorts(const decltype(nodeSpec::inputs)& ports) {
    json obj = json::object();
    for (const auto& [name, refs] : ports) {
        json list = json::array();
        for (const auto& r : refs) list.push_back(fromRef(r));
        obj[name] = std::move(list);
    }
    return obj;
}

json fromNode(const nodeSpec& n) {
    json node{{"id", n.id}, {"type", n.type}};
    if (!n.params.empty()) {
        json params = json::object();
        for (const auto& [name, value] : n.params) params[name] = fromParam(value);
        node["params"] = std::move(params);
    }
    if (!n.inputs.empty()) node["inputs"] = fromPorts(n.inputs);
    if (!n.outputs.empty()) node["outputs"] = fromPorts(n.outputs);
    if (n.src) node["src"] = fromSpan(*n.src);
    return node;
}

json fromBuffer(const bufferDecl& b) {
    json decl{{"buffer", b.buffer.index}, {"id", b.id}, {"channels", b.channels}};
    if (b.seconds) decl["seconds"] = *b.seconds;
    if (b.file) decl["file"] = *b.file;
    if (!b.resample) decl["resample"] = false;
    if (b.src) decl["src"] = fromSpan(*b.src);
    return decl;
}

bufferDecl toBuffer(const json& j) {
    bufferDecl b{.buffer = {.index = j.at("buffer").get<std::int32_t>()},
                 .id = j.at("id").get<std::string>(),
                 .channels = j.value("channels", 1),
                 .seconds = std::nullopt,
                 .file = std::nullopt,
                 .resample = j.value("resample", true),
                 .src = std::nullopt};
    if (const auto it = j.find("seconds"); it != j.end()) b.seconds = it->get<double>();
    if (const auto it = j.find("file"); it != j.end()) b.file = it->get<std::string>();
    if (const auto it = j.find("src"); it != j.end()) {
        b.src = toSpan(*it);
        if (!b.src) throw conversionError("buffer " + b.id + ": malformed src");
    }
    return b;
}

nodeSpec toNode(const json& j) {
    nodeSpec n;
    n.id = j.at("id").get<std::string>();
    n.type = j.at("type").get<std::string>();
    if (const auto it = j.find("params"); it != j.end()) {
        for (const auto& [name, value] : it->items()) n.params.emplace(name, toParam(value));
    }
    if (const auto it = j.find("inputs"); it != j.end()) {
        for (const auto& [name, value] : it->items()) n.inputs.emplace(name, toRefs(value));
    }
    if (const auto it = j.find("outputs"); it != j.end()) {
        for (const auto& [name, value] : it->items()) n.outputs.emplace(name, toRefs(value));
    }
    if (const auto it = j.find("src"); it != j.end()) {
        n.src = toSpan(*it);
        if (!n.src) throw conversionError("node " + n.id + ": malformed src");
    }
    return n;
}

graphSpec toGraph(const json& j) {
    graphSpec g;
    g.seed = j.value("seed", std::uint64_t{0});
    if (const auto it = j.find("tempo"); it != j.end()) g.tempo = it->get<double>();
    g.meter = j.value("meter", 4);
    if (const auto it = j.find("page"); it != j.end()) g.pagePort = it->at("port").get<int>();
    for (const auto& p : j.at("pipes")) {
        g.pipes.push_back({.pipe = toRef(p.at("pipe")),
                           .channels = p.value("channels", 1),
                           .feedback = p.value("feedback", false)});
    }
    if (const auto it = j.find("buffers"); it != j.end()) {
        for (const auto& b : *it) g.buffers.push_back(toBuffer(b));
    }
    for (const auto& n : j.at("nodes")) g.nodes.push_back(toNode(n));
    return g;
}

// Attributes a schema error at @p pointer to the node it falls under.
error schemaError(const json& doc, const std::string& pointer, const std::string& message) {
    error e{.node = {},
            .message = (pointer.empty() ? "document" : pointer) + ": " + message,
            .src = std::nullopt};
    // Nodes and buffers carry their definition sites; a node also its ID.
    for (const std::string_view member : {"nodes", "buffers"}) {
        const std::string prefix = "/" + std::string(member) + "/";
        if (!pointer.starts_with(prefix)) continue;
        const json& item = doc.at(member).at(std::stoul(pointer.substr(prefix.size())));
        const auto id = item.find("id");
        if (member == "nodes" && id != item.end() && id->is_string()) e.node = *id;
        // A malformed span is left out; the schema error itself reports it.
        if (const auto src = item.find("src"); src != item.end()) e.src = toSpan(*src);
    }
    return e;
}

} // namespace

std::string error::text() const {
    std::ostringstream out;
    if (src) out << src->file << ':' << src->line << ':' << src->column << ": ";
    if (!node.empty()) out << "node " << node << ": ";
    out << message;
    return out.str();
}

std::string readResult::text() const { return metadata::joinLines(errors); }

readResult read(std::string_view text) {
    readResult result;
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded()) {
        result.errors.push_back({.node = {}, .message = "IR is not valid JSON", .src = std::nullopt});
        return result;
    }
    collectErrors handler;
    try {
        validator().validate(doc, handler);
    } catch (const std::exception& e) {
        result.errors.push_back({.node = {},
                                 .message = std::string("schema validation failed: ") + e.what(),
                                 .src = std::nullopt});
        return result;
    }
    for (const auto& [pointer, message] : handler.found) {
        result.errors.push_back(schemaError(doc, pointer, message));
    }
    if (!result.errors.empty()) return result;

    graphSpec graph;
    try {
        graph = toGraph(doc);
    } catch (const std::exception& e) {
        // The schema admits only convertible documents; this reports a
        // disagreement between the two instead of throwing out of read().
        result.errors.push_back(
            {.node = {}, .message = std::string("IR conversion failed: ") + e.what(), .src = std::nullopt});
        return result;
    }
    result.errors = validate(graph);
    if (result.errors.empty()) result.graph = std::move(graph);
    return result;
}

std::string write(const graphSpec& g, int indent) {
    json doc{{"format", formatName}, {"version", formatVersion}, {"seed", g.seed}, {"meter", g.meter}};
    if (g.tempo) doc["tempo"] = *g.tempo;
    if (g.pagePort) doc["page"] = json{{"port", *g.pagePort}};
    json pipes = json::array();
    for (const auto& p : g.pipes) {
        json decl{{"pipe", fromRef(p.pipe)}, {"channels", p.channels}};
        if (p.feedback) decl["feedback"] = true;
        pipes.push_back(std::move(decl));
    }
    doc["pipes"] = std::move(pipes);
    if (!g.buffers.empty()) {
        json buffers = json::array();
        for (const auto& b : g.buffers) buffers.push_back(fromBuffer(b));
        doc["buffers"] = std::move(buffers);
    }
    json nodes = json::array();
    for (const auto& n : g.nodes) nodes.push_back(fromNode(n));
    doc["nodes"] = std::move(nodes);
    return doc.dump(indent);
}

} // namespace ir
