// The sidecar mode (serve.h, tauri-editor.md §4).

#include "serve.h"

#include <array>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <map>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

#include <espresso/metadata/version.h>
#include <espresso/protocol/jthread.h>
#include <espresso/workspace/consoleLog.h>
#include <espresso/workspace/fileStore.h>
#include <espresso/workspace/highlighter.h>
#include <espresso/workspace/interpreterLink.h>
#include <espresso/workspace/settings.h>

namespace client {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// This protocol's version, answered to `hello` (§4.5).
constexpr int protocolVersion = 1;

// JSON-RPC 2.0's error codes, and the sidecar's own (§4.3.2).
enum class errorCode : std::int16_t {
    parse = -32700,
    invalidRequest = -32600,
    noMethod = -32601,
    invalidParams = -32602,
    noProject = -32001,
    file = -32002,
};

// Why a request failed.
struct failure {
    errorCode code = errorCode::invalidRequest;
    std::string message;
};

// A run request is answered later, once the link has run it.
struct deferred {};

// What a method gives: its result, its failure, or an answer to come.
using answer = std::variant<json, failure, deferred>;

// The names of the highlight categories (§4.5.2), in enum order.
constexpr std::array<std::string_view, workspace::highlightCategoryCount> categoryNames{
    "comment",     "string",     "number", "unitLiteral", "keyword",
    "specialForm", "objectForm", "symbol", "delimiter",   "error"};

constexpr std::string_view toString(workspace::highlightCategory c) {
    return categoryNames[static_cast<std::size_t>(c)];
}

constexpr std::string_view toString(workspace::consoleKind kind) {
    switch (kind) {
    case workspace::consoleKind::info: return "info";
    case workspace::consoleKind::error: return "error";
    case workspace::consoleKind::server: return "server";
    }
    return "info";
}

json response(const json& id, json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json errorResponse(const json& id, const failure& f) {
    return {{"jsonrpc", "2.0"},
            {"id", id},
            {"error", {{"code", static_cast<int>(f.code)}, {"message", f.message}}}};
}

json notification(std::string_view method, json params) {
    return {{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}};
}

json toJson(const workspace::consoleEntry& e) {
    json entry = {{"kind", toString(e.kind)}, {"text", e.text}};
    if (e.location) {
        entry["location"] = {{"path", e.location->path.generic_string()},
                             {"line", e.location->line},
                             {"column", e.location->column}};
    }
    return entry;
}

json toJson(const workspace::serverStatus& s) {
    const auto orNull = [](const std::optional<double>& v) { return v ? json(*v) : json(nullptr); };
    return {{"connected", s.connected}, {"server", s.server},     {"bpm", orNull(s.bpm)},
            {"beat", orNull(s.beat)},   {"beatsPerBar", s.meter}, {"missed", s.missed},
            {"xruns", s.xruns},         {"drops", s.drops},       {"overflows", s.overflows},
            {"load", s.load},           {"threads", s.threads},   {"threadLoads", s.threadLoads},
            {"plan", s.activePlan}};
}

// A path inside the project (§4.4), or nothing: empty, absolute, and
// `..` paths are refused.
std::optional<fs::path> projectPath(std::string_view text) {
    const fs::path path(text);
    if (text.empty() || path.is_absolute() || path.has_root_name()) return std::nullopt;
    for (const auto& part : path) {
        if (part == "..") return std::nullopt;
    }
    return path.lexically_normal();
}

// Reads a request's params by type, keeping the first problem.
class paramReader {
public:
    explicit paramReader(const json& params) : params_(params) {}

    // The first problem found, if any.
    const std::optional<failure>& problem() const { return problem_; }

    std::optional<std::string> text(std::string_view name, bool required = true) {
        if (!params_.contains(name)) return missing(name, required);
        const json& v = params_.at(name);
        if (!v.is_string()) return refuse(name, "a string");
        return v.get<std::string>();
    }

    std::optional<fs::path> path(std::string_view name, bool required = true) {
        const auto t = text(name, required);
        if (!t) return std::nullopt;
        auto p = projectPath(*t);
        if (!p) return refuse(name, "a path inside the project, relative to its root");
        return p;
    }

    // A whole number from 1.
    std::optional<std::size_t> position(std::string_view name) {
        if (!params_.contains(name)) return missing(name, true);
        const json& v = params_.at(name);
        if (!v.is_number_unsigned() || v.get<std::uint64_t>() < 1)
            return refuse(name, "a whole number from 1");
        return static_cast<std::size_t>(v.get<std::uint64_t>());
    }

    // Seconds from 0, or the link's default.
    double fade() {
        const double fallback = workspace::runRequest{}.fade;
        if (!params_.contains("fade")) return fallback;
        const json& v = params_.at("fade");
        if (!v.is_number() || !(v.get<double>() >= 0.0) || !std::isfinite(v.get<double>())) {
            refuse("fade", "a number of seconds from 0");
            return fallback;
        }
        return v.get<double>();
    }

    workspace::quantization quantize() {
        constexpr auto none = workspace::quantization::none;
        const auto name = text("quantize", false);
        if (!name) return none;
        for (const auto q : {none, workspace::quantization::beat, workspace::quantization::bar}) {
            if (workspace::toString(q) == *name) return q;
        }
        refuse("quantize", R"("none", "beat", or "bar")");
        return none;
    }

    // The editor's buffers by path: an object of strings; `.lisp` files only.
    std::map<fs::path, std::string> sources() {
        std::map<fs::path, std::string> result;
        if (!params_.contains("sources")) return result;
        const json& v = params_.at("sources");
        if (!v.is_object()) {
            refuse("sources", "an object from path to text");
            return result;
        }
        for (const auto& [key, value] : v.items()) {
            const auto p = projectPath(key);
            if (!p || !value.is_string()) {
                refuse("sources", "an object from paths inside the project to text");
                return {};
            }
            if (p->extension() == ".lisp") result.emplace(*p, value.get<std::string>());
        }
        return result;
    }

private:
    std::nullopt_t missing(std::string_view name, bool required) {
        if (required) note("params." + std::string(name) + " is missing");
        return std::nullopt;
    }

    std::nullopt_t refuse(std::string_view name, std::string_view what) {
        note("params." + std::string(name) + " must be " + std::string(what));
        return std::nullopt;
    }

    void note(std::string message) {
        if (!problem_) problem_ = failure{.code = errorCode::invalidParams, .message = std::move(message)};
    }

    const json& params_;
    std::optional<failure> problem_;
};

// Writes whole lines, one thread at a time (§4.2).
class lineWriter {
public:
    explicit lineWriter(std::ostream& out) : out_(out) {}

    void write(const json& message) {
        // Console text from the server may not be UTF-8; it is replaced,
        // never thrown on.
        const std::string line = message.dump(-1, ' ', false, json::error_handler_t::replace);
        const std::scoped_lock lock(mutex_);
        out_ << line << '\n' << std::flush;
    }

private:
    std::ostream& out_;
    std::mutex mutex_;
};

// One sidecar: the project it has open, its link, and the thread that
// writes what the link replies.
class session {
public:
    session(std::ostream& out, workspace::linkOptions link)
        : writer_(out),
          clientVersion_(link.clientVersion),
          serverAddress_("127.0.0.1:" + std::to_string(link.server.serverPort)),
          link_(std::move(link),
                [this] {
                    {
                        const std::scoped_lock lock(mutex_);
                        woken_ = true;
                    }
                    wake_.notify_one();
                }),
          drainer_([this](const espresso::stop_token& stop) { drainLoop(stop); }) {}

    ~session() { stopDrainer(); }

    session(const session&) = delete;
    session& operator=(const session&) = delete;

    // Answers one line; false once the line was `shutdown`.
    bool handle(std::string_view line) {
        json message = json::parse(line, nullptr, false);
        if (message.is_discarded()) {
            writer_.write(
                errorResponse(nullptr, {.code = errorCode::parse, .message = "the line is not JSON"}));
            return true;
        }
        const auto invalid = [this](std::string message) {
            writer_.write(
                errorResponse(nullptr, {.code = errorCode::invalidRequest, .message = std::move(message)}));
            return true;
        };
        if (!message.is_object()) return invalid("a request is a JSON object; batches are not accepted");
        const bool hasId = message.contains("id");
        const json id = hasId ? message.at("id") : json(nullptr);
        if (hasId && !id.is_number_integer() && !id.is_string())
            return invalid("id must be an integer or a string");
        if (!message.contains("jsonrpc") || message.at("jsonrpc") != "2.0")
            return invalid("jsonrpc must be \"2.0\"");
        if (!message.contains("method") || !message.at("method").is_string())
            return invalid("method must be a string");
        const auto method = message.at("method").get<std::string>();
        const json params = message.contains("params") ? message.at("params") : json::object();

        answer a = params.is_object()
                       ? call(method, params, id)
                       : failure{.code = errorCode::invalidParams, .message = "params must be an object"};
        if (hasId) {
            if (std::holds_alternative<json>(a)) writer_.write(response(id, std::move(std::get<json>(a))));
            if (std::holds_alternative<failure>(a)) writer_.write(errorResponse(id, std::get<failure>(a)));
        }
        return method != "shutdown";
    }

    // Waits for the run requests already read and writes what they produced.
    void finish() {
        link_.waitIdle();
        stopDrainer();
        drain();
    }

private:
    answer call(const std::string& method, const json& params, const json& id) {
        paramReader p(params);
        answer a = failure{.code = errorCode::noMethod, .message = "no method " + method};
        if (method == "hello") a = hello(p);
        if (method == "project.open") a = openProject(p);
        if (method == "file.read") a = readFile(p);
        if (method == "file.write") a = writeFile(p);
        if (method == "run.play" || method == "run.eval" || method == "run.stop" ||
            method == "server.connect") {
            a = run(method, p, id);
        }
        if (method == "highlight") a = highlight(p);
        if (method == "shutdown") a = json(nullptr);
        if (p.problem()) return *p.problem();
        return a;
    }

    answer hello(paramReader& p) const {
        (void)p.text("version", false);
        return json{{"client", clientVersion_},
                    {"protocol", protocolVersion},
                    {"metadata", metadata::version},
                    {"server", serverAddress_}};
    }

    answer openProject(paramReader& p) {
        const auto root = p.text("root");
        if (!root) return json(nullptr);
        std::error_code ec;
        const fs::path dir = fs::canonical(*root, ec);  // absolute, with links resolved
        if (ec || !fs::is_directory(dir, ec)) {
            return failure{.code = errorCode::invalidParams,
                           .message = "params.root is not a directory: " + *root};
        }
        root_ = dir;
        json files = json::array();
        for (const auto& f : workspace::fileStore::listProject(dir)) files.push_back(f.generic_string());
        return json{{"root", dir.generic_string()}, {"files", std::move(files)}};
    }

    answer readFile(paramReader& p) const {
        const auto path = p.path("path");
        if (!root_) return noProject();
        if (!path) return json(nullptr);
        auto read = workspace::fileStore::read(*root_ / *path);
        if (!read.text) return failure{.code = errorCode::file, .message = std::move(read.error)};
        return json{{"text", std::move(*read.text)}};
    }

    answer writeFile(paramReader& p) const {
        const auto path = p.path("path");
        const auto text = p.text("text");
        if (!root_) return noProject();
        if (!path || !text) return json(nullptr);
        if (auto error = workspace::fileStore::writeAtomic(*root_ / *path, *text)) {
            return failure{.code = errorCode::file, .message = std::move(*error)};
        }
        return json(nullptr);
    }

    answer run(const std::string& method, paramReader& p, const json& id) {
        workspace::runRequest request;
        if (method == "server.connect") {
            request.what = workspace::runRequest::kind::connect;
        } else if (method == "run.stop") {
            request.fade = p.fade();
            request.what = workspace::runRequest::kind::stop;
        } else {
            request.fade = p.fade();
            request.quant = p.quantize();
            auto overlay = p.sources();
            if (method == "run.eval") {
                request.what = workspace::runRequest::kind::evalForm;
                request.form = p.text("text").value_or("");
                request.file = p.path("file").value_or(fs::path());
                request.line = p.position("line").value_or(1);
                request.column = p.position("column").value_or(1);
            } else {
                request.file = p.path("file", false).value_or(fs::path());
            }
            if (p.problem()) return *p.problem();
            if (!root_) return noProject();
            request.sources = projectSources(*root_, std::move(overlay));
        }
        if (p.problem()) return *p.problem();
        {
            const std::scoped_lock lock(mutex_);
            request.id = nextId_++;
            pending_.emplace(request.id, id);  // null: a notification, answered by no response
        }
        link_.submit(std::move(request));
        return deferred{};
    }

    static answer highlight(paramReader& p) {
        const auto text = p.text("text");
        if (!text) return json(nullptr);
        json spans = json::array();
        for (const auto& s : workspace::highlight(*text).spans) {
            spans.push_back({{"offset", s.offset}, {"length", s.length}, {"category", toString(s.category)}});
        }
        return json{{"spans", std::move(spans)}};
    }

    static failure noProject() {
        return {.code = errorCode::noProject, .message = "no project is open; send project.open first"};
    }

    // The project's .lisp files from disk, with the editor's buffers in
    // place of the files they hold (§4.5.1).
    static std::map<fs::path, std::string> projectSources(const fs::path& root,
                                                          std::map<fs::path, std::string> overlay) {
        for (const auto& f : workspace::fileStore::listProject(root)) {
            if (f.extension() != ".lisp" || overlay.contains(f)) continue;
            if (auto read = workspace::fileStore::read(root / f); read.text) {
                overlay.emplace(f, std::move(*read.text));
            }
        }
        return overlay;
    }

    // Writes the link's replies: each one's console entries and status as
    // notifications, then, for a run request with an id, its response
    // (§4.6.1).
    void drain() {
        for (const auto& reply : link_.takeReplies()) {
            std::size_t errors = 0;
            for (const auto& e : reply.entries) {
                writer_.write(notification("console", toJson(e)));
                if (e.kind == workspace::consoleKind::error) ++errors;
            }
            if (const auto& status = reply.status) writer_.write(notification("status", toJson(*status)));
            if (reply.id == 0) continue;
            json id;
            {
                const std::scoped_lock lock(mutex_);
                auto node = pending_.extract(reply.id);
                if (node.empty()) continue;
                id = std::move(node.mapped());
            }
            if (!id.is_null()) writer_.write(response(id, {{"errors", errors}}));
        }
    }

    void drainLoop(const espresso::stop_token& stop) {
        while (true) {
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return woken_ || stop.stop_requested(); });
                if (stop.stop_requested()) return;
                woken_ = false;
            }
            drain();
        }
    }

    // Stops and joins the drainer; the stop is requested under the mutex
    // its wait uses, so the wake cannot be lost.
    void stopDrainer() {
        {
            const std::scoped_lock lock(mutex_);
            drainer_.request_stop();
        }
        wake_.notify_all();
        if (drainer_.joinable()) drainer_.join();
    }

    lineWriter writer_;
    std::string clientVersion_;
    std::string serverAddress_;     // where the link reaches the server, for hello
    std::optional<fs::path> root_;  // the open project, read and written by the reading thread only
    std::mutex mutex_;
    std::condition_variable wake_;
    bool woken_ = false;                     // guarded by mutex_
    std::uint64_t nextId_ = 1;               // guarded by mutex_
    std::map<std::uint64_t, json> pending_;  // run requests' ids by link id, guarded by mutex_
    // After what its wake uses, so its worker stops before they go away.
    workspace::interpreterLink link_;
    // Last, so it stops and joins before the members it uses go away.
    espresso::jthread drainer_;
};

}  // namespace

int serve(std::istream& in, std::ostream& out, serveOptions options) {
    session s(out, std::move(options.link));
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (!s.handle(line)) break;
    }
    s.finish();
    return 0;
}

}  // namespace client
