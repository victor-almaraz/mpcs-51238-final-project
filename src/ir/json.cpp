// IR JSON reading and writing (interpreter.md §12.1).

#include <proj/ir/ir.h>

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <sstream>

namespace ir {

namespace {

using nlohmann::json;
using metadata::graphSpec;
using metadata::nodeSpec;
using metadata::paramValue;
using metadata::pipeRef;

// --- Schema validation --------------------------------------------------

class collectErrors : public nlohmann::json_schema::basic_error_handler {
public:
    void error(const json::json_pointer& ptr, const json&, const std::string& message) override {
        basic_error_handler::error(ptr, json(), message);
        found.push_back({ptr.to_string(), message});
    }
    std::vector<std::pair<std::string, std::string>> found;
};

const nlohmann::json_schema::json_validator& validator() {
    static const nlohmann::json_schema::json_validator v(json::parse(schema()));
    return v;
}

// --- JSON to graph ------------------------------------------------------

pipeRef toRef(const json& j) {
    // A reference is an object with one member, {"<kind>": index}.
    const auto member = j.begin();
    return {*metadata::parsePipeKind(member.key()), member.value().get<std::int32_t>()};
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
    return toRef(j);
}

json fromParam(const paramValue& v) {
    return std::visit(
        [](const auto& x) -> json {
            if constexpr (std::is_same_v<std::decay_t<decltype(x)>, pipeRef>) {
                return fromRef(x);
            } else {
                return x;
            }
        },
        v);
}

metadata::sourceSpan toSpan(const json& j) {
    return {j.at("file").get<std::string>(), j.at("line").get<int>(), j.at("column").get<int>(),
            j.value("offset", std::int64_t{0}), j.value("length", std::int64_t{0})};
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
    if (const auto it = j.find("src"); it != j.end()) n.src = toSpan(*it);
    return n;
}

graphSpec toGraph(const json& j) {
    graphSpec g;
    g.seed = j.value("seed", std::uint64_t{0});
    if (const auto it = j.find("tempo"); it != j.end()) g.tempo = it->get<double>();
    g.meter = j.value("meter", 4);
    for (const auto& p : j.at("pipes")) {
        g.pipes.push_back({toRef(p.at("pipe")), p.value("channels", 1), p.value("feedback", false)});
    }
    for (const auto& n : j.at("nodes")) g.nodes.push_back(toNode(n));
    return g;
}

// Attributes a schema error at @p pointer to the node it falls under.
error schemaError(const json& doc, const std::string& pointer, const std::string& message) {
    error e{{}, (pointer.empty() ? "document" : pointer) + ": " + message, std::nullopt};
    constexpr std::string_view prefix = "/nodes/";
    if (pointer.starts_with(prefix)) {
        const std::size_t index = std::stoul(pointer.substr(prefix.size()));
        const json& node = doc.at("nodes").at(index);
        if (const auto id = node.find("id"); id != node.end() && id->is_string()) e.node = *id;
        if (const auto src = node.find("src"); src != node.end()) {
            try {
                e.src = toSpan(*src);
            } catch (const json::exception&) {
                // malformed span: reported by the schema error itself
            }
        }
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

std::string readResult::text() const {
    std::string out;
    for (const auto& e : errors) {
        if (!out.empty()) out += '\n';
        out += e.text();
    }
    return out;
}

readResult read(std::string_view text) {
    readResult result;
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded()) {
        result.errors.push_back({{}, "IR is not valid JSON", std::nullopt});
        return result;
    }
    collectErrors handler;
    try {
        validator().validate(doc, handler);
    } catch (const std::exception& e) {
        result.errors.push_back({{}, std::string("schema validation failed: ") + e.what(), std::nullopt});
        return result;
    }
    for (const auto& [pointer, message] : handler.found) {
        result.errors.push_back(schemaError(doc, pointer, message));
    }
    if (!result.errors.empty()) return result;

    graphSpec graph = toGraph(doc);
    result.errors = validate(graph);
    if (result.errors.empty()) result.graph = std::move(graph);
    return result;
}

std::string write(const graphSpec& g, int indent) {
    json doc{{"format", formatName}, {"version", formatVersion}, {"seed", g.seed}, {"meter", g.meter}};
    if (g.tempo) doc["tempo"] = *g.tempo;
    json pipes = json::array();
    for (const auto& p : g.pipes) {
        json decl{{"pipe", fromRef(p.pipe)}, {"channels", p.channels}};
        if (p.feedback) decl["feedback"] = true;
        pipes.push_back(std::move(decl));
    }
    doc["pipes"] = std::move(pipes);
    json nodes = json::array();
    for (const auto& n : g.nodes) {
        json node{{"id", n.id}, {"type", n.type}};
        if (!n.params.empty()) {
            json params = json::object();
            for (const auto& [name, value] : n.params) params[name] = fromParam(value);
            node["params"] = std::move(params);
        }
        for (const auto& [key, ports] : {std::pair{"inputs", &n.inputs}, std::pair{"outputs", &n.outputs}}) {
            if (ports->empty()) continue;
            json obj = json::object();
            for (const auto& [name, refs] : *ports) {
                json list = json::array();
                for (const auto& r : refs) list.push_back(fromRef(r));
                obj[name] = std::move(list);
            }
            node[key] = std::move(obj);
        }
        if (n.src) {
            node["src"] = {{"file", n.src->file},     {"line", n.src->line},
                           {"column", n.src->column}, {"offset", n.src->offset},
                           {"length", n.src->length}};
        }
        nodes.push_back(std::move(node));
    }
    doc["nodes"] = std::move(nodes);
    return doc.dump(indent);
}

} // namespace ir
