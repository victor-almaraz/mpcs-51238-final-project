#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/interpreter/project.h>
#include <espresso/interpreter/reader.h>
#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>
#include <espresso/metadata/version.h>
#include <espresso/protocol/client.h>
#include <espresso/protocol/jthread.h>
#include <espresso/protocol/messages.h>
#include <espresso/protocol/osc.h>
#include <espresso/workspace/consoleLog.h>
#include <espresso/workspace/interpreterLink.h>
#include <espresso/workspace/settings.h>

namespace workspace {

namespace {

namespace address = protocol::address;

// A connected server that sends nothing for this long is considered gone;
// it sends /counters ten times per second (`protocol.md` §5).
constexpr auto silenceLimit = std::chrono::milliseconds(1500);

// Line and column of a reader error inside a form that starts at
// (line, column) in its file.
sourceLocation locate(const std::filesystem::path& file, std::size_t line, std::size_t column,
                      const interpreter::sourceSpan& span) {
    sourceLocation loc{.path = file, .line = line + span.line - 1, .column = span.column};
    if (span.line == 1) loc.column = column + span.column - 1;
    return loc;
}

std::string where(const sourceLocation& loc) {
    std::ostringstream out;
    out << loc.path.string() << ':' << loc.line << ':' << loc.column;
    return out.str();
}

std::string serverAddress(const protocol::clientOptions& o) {
    return "127.0.0.1:" + std::to_string(o.serverPort);
}

void add(runReply& reply, consoleKind kind, std::string text, std::optional<sourceLocation> loc = {}) {
    reply.entries.push_back({.kind = kind, .text = std::move(text), .location = std::move(loc)});
}

}  // namespace

float serverStatus::busiest() const { return threadLoads.empty() ? load : std::ranges::max(threadLoads); }

std::string serverStatus::loadText() const {
    if (!connected) return "Load —";
    const float b = busiest();
    std::ostringstream out;
    out << "Load " << (std::isfinite(b) ? std::lround(std::clamp(b, 0.0f, 99.99f) * 100.0f) : 0) << '%';
    if (threads > 1) out << " · Threads " << threads;
    return out.str();
}

std::string serverStatus::tempoText() const {
    if (!connected || !bpm || !std::isfinite(*bpm)) return "— BPM";
    std::ostringstream out;
    out << std::round(*bpm * 10.0) / 10.0 << " BPM";
    return out.str();
}

std::string serverStatus::beatText() const {
    // The upper bound keeps the conversion to an integer defined.
    if (!connected || activePlan == 0 || !beat || !std::isfinite(*beat) || *beat < 0.0 || *beat >= 1e15)
        return "—";
    const auto beats = static_cast<std::int64_t>(std::floor(*beat));
    const std::int64_t perBar = std::max<std::int64_t>(1, meter);
    return std::to_string(beats / perBar + 1) + "." + std::to_string(beats % perBar + 1);
}

interpreterLink::interpreterLink(linkOptions options, std::function<void()> wake)
    : options_(std::move(options)),
      wake_(std::move(wake)),
      worker_([this](const espresso::stop_token& stop) { work(stop); }) {
    if (options_.connectOnStart) {
        runRequest connect;
        connect.what = runRequest::kind::connect;
        submit(std::move(connect));
    }
}

// The worker is the last member, so it stops and joins first; the client,
// whose receiver thread calls back into this object, is destroyed next,
// while the state it touches still exists.
interpreterLink::~interpreterLink() {
    {
        const std::scoped_lock lock(mutex_);  // no lost wake-up between the check and the wait
        worker_.request_stop();
    }
    wakeWorker_.notify_all();
}

void interpreterLink::setWake(std::function<void()> wake) {
    const std::scoped_lock lock(wakeMutex_);  // waits out a wake in progress
    wake_ = std::move(wake);
}

void interpreterLink::submit(runRequest request) {
    {
        std::scoped_lock lock(mutex_);
        requests_.push_back(std::move(request));
        ++inFlight_;
    }
    wakeWorker_.notify_one();
}

std::vector<runReply> interpreterLink::takeReplies() {
    std::scoped_lock lock(mutex_);
    return std::exchange(replies_, {});
}

void interpreterLink::waitIdle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return inFlight_ == 0; });
}

void interpreterLink::push(runReply reply) {
    {
        const std::scoped_lock lock(mutex_);
        replies_.push_back(std::move(reply));
    }
    // The wake runs under its own lock, so setWake() cannot return, and its
    // owner cannot destroy what the wake uses, while a wake is running. The
    // wake only posts an event and takes no lock of this class.
    const std::scoped_lock lock(wakeMutex_);
    if (wake_) wake_();
}

serverStatus interpreterLink::statusSnapshot() {
    std::scoped_lock lock(mutex_);
    return status_;
}

void interpreterLink::work(const espresso::stop_token& stop) {
    while (true) {
        std::optional<runRequest> request;
        {
            std::unique_lock lock(mutex_);
            auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            if (retryAt_ && *retryAt_ < until) until = *retryAt_;
            wakeWorker_.wait_until(lock, until, [&] { return stop.stop_requested() || !requests_.empty(); });
            if (stop.stop_requested()) return;
            if (!requests_.empty()) {
                request = std::move(requests_.front());
                requests_.pop_front();
            }
        }
        if (!request) {
            checkSilence();
            retryIfDue();
            continue;
        }
        runReply reply = process(*request);
        reply.id = request->id;
        push(std::move(reply));
        {
            const std::scoped_lock lock(mutex_);
            --inFlight_;
        }
        idle_.notify_all();
    }
}

void interpreterLink::checkSilence() {
    if (!client_) return;
    {
        std::scoped_lock lock(mutex_);
        if (!status_.connected) return;
    }
    if (std::chrono::steady_clock::now() - client_->lastHeard() < silenceLimit) return;
    runReply reply;
    {
        std::scoped_lock lock(mutex_);
        status_.connected = false;
        status_.activePlan = 0;
        status_.bpm.reset();
        status_.beat.reset();
        reply.status = status_;
    }
    add(reply, consoleKind::server, "server: connection to " + serverAddress(options_.server) + " lost");
    push(std::move(reply));
    retryAt_ = std::chrono::steady_clock::now();  // a restarted server answers at once
}

// Sends /hello in the background while the server does not answer. Only
// a change is reported: the connection, or a refusal such as a metadata
// mismatch, which ends the retries.
void interpreterLink::retryIfDue() {
    if (!retryAt_ || std::chrono::steady_clock::now() < *retryAt_) return;
    runReply reply;
    if (!ensureConnected(reply, true) && reply.entries.empty()) return;  // still not answering
    push(std::move(reply));
}

// A background attempt sends /hello once, so it holds the worker, and its
// shutdown, for one reply timeout at most, and reports nothing while the
// server does not answer.
bool interpreterLink::ensureConnected(runReply& reply, bool background) {
    if (statusSnapshot().connected) return true;
    if (!client_) {
        client_ = std::make_unique<protocol::client>(
            options_.server, [this](const protocol::message& m) { onUnsolicited(m); });
        std::string error;
        if (!client_->open(error)) {
            add(reply, consoleKind::error, "server: cannot open a client socket: " + error);
            client_.reset();
            return false;
        }
    }
    std::vector<protocol::argument> args{options_.clientVersion, std::string(metadata::version)};
    const auto hello = background ? client_->request(address::hello, std::move(args), 0)
                                  : client_->request(address::hello, std::move(args));
    if (!hello.reply) {
        retryAt_ = std::chrono::steady_clock::now() + options_.retryInterval;
        if (!background) {
            add(reply, consoleKind::server,
                "server: not reachable at " + serverAddress(options_.server) +
                    "; the editor connects when espresso-server starts");
        }
        return false;
    }
    retryAt_.reset();  // the server answered
    if (!hello.ok()) {
        // A metadata mismatch means the client must not submit IR (`protocol.md` §4.2).
        add(reply, consoleKind::error, "server: " + hello.failText());
        return false;
    }
    const auto& ok = *hello.reply;
    const std::string name(ok.string(1).value_or("server"));
    serverRate_ = ok.int32(2).value_or(48000);
    if (!client_->request(address::notify, {std::int32_t{1}}).ok()) {
        add(reply, consoleKind::server, "server: /notify failed; counters will not update");
    }
    {
        std::scoped_lock lock(mutex_);
        status_.connected = true;
        status_.server = name;
        reply.status = status_;
    }
    add(reply, consoleKind::server,
        "server: connected to " + name + " at " + serverAddress(options_.server) + " (" +
            std::to_string(ok.int32(2).value_or(0)) + " Hz, " + std::to_string(ok.int32(3).value_or(0)) +
            "-sample blocks)");
    return true;
}

// Sends a request; a server that forgot this client (restart) gets a new
// /hello and one more try.
protocol::requestResult interpreterLink::send(std::string_view addr, std::vector<protocol::argument> args,
                                              runReply& reply) {
    auto r = client_->request(addr, args);
    if (r.failCode() == protocol::errorCode::notConnected || !r.reply) {
        {
            std::scoped_lock lock(mutex_);
            status_.connected = false;
        }
        if (!ensureConnected(reply)) return r;
        r = client_->request(addr, std::move(args));
    }
    return r;
}

// Compiles the project's entry file (interpreter.md §2). Errors are
// reported with their locations; the running plan is unchanged (§13.3).
std::optional<metadata::graphSpec> interpreterLink::compileProject(const runRequest& request,
                                                                   runReply& reply) {
    const auto entry = interpreter::entryFile(request.sources, request.file);
    if (entry.empty()) {
        add(reply, consoleKind::error,
            "play: add a main.lisp, or open the file to play; the running plan is unchanged");
        return std::nullopt;
    }
    interpreter::compileOptions base;
    base.sampleRate = serverRate_;
    // The project file's entry, seed, and page port (interpreter.md §11.1),
    // and imports from the editor's buffers, saved or not (§11.2).
    const auto file = request.sources.find(interpreter::projectFile);
    const auto compiled = interpreter::compileProject(
        request.sources.at(entry), entry.generic_string(), std::move(base),
        file == request.sources.end() ? std::nullopt : std::optional<std::string_view>(file->second),
        [&sources = request.sources](const std::string& path) {
            const auto it = sources.find(path);
            return interpreter::loadedFile{
                .text = it == sources.end() ? std::nullopt : std::optional(it->second), .error = {}};
        });
    if (!compiled.projectErrors.empty()) {
        for (const auto& e : compiled.projectErrors) {
            const sourceLocation loc{.path = e.file, .line = e.span.line, .column = e.span.column};
            add(reply, consoleKind::error, where(loc) + ": " + e.message, loc);
        }
        add(reply, consoleKind::error, "play: project.lisp is not valid; the running plan is unchanged");
        return std::nullopt;
    }
    const auto& result = compiled.piece;
    // A console line jumps to a project file; a standard library file is
    // compiled in, so its lines name it without a location to open.
    const auto report = [&reply](consoleKind kind, const std::string& prefix, const std::string& file,
                                 const interpreter::sourceSpan& span, const std::string& suffix) {
        const sourceLocation loc{.path = file, .line = span.line, .column = span.column};
        const bool opens = !interpreter::isLibraryPath(file);
        add(reply, kind, prefix + where(loc) + suffix, opens ? std::optional(loc) : std::nullopt);
    };
    if (!result.graph) {
        for (const auto& e : result.errors) {
            report(consoleKind::error, "", e.file, e.span, ": " + e.message);
            for (const auto& frame : std::views::reverse(e.stack))
                report(consoleKind::info, "  called from ", frame.file, frame.span, "");
        }
        add(reply, consoleKind::error,
            "play: " + entry.generic_string() + " did not compile; the running plan is unchanged");
        return std::nullopt;
    }
    for (const auto& w : result.warnings)
        report(consoleKind::info, "", w.file, w.span, ": warning: " + w.message);
    const metadata::graphSpec& graph = *result.graph;
    add(reply, consoleKind::info,
        "play: compiled " + entry.generic_string() + ": " + std::to_string(graph.nodes.size()) +
            " node(s), " + std::to_string(graph.pipes.size()) + " pipe(s)" +
            (graph.buffers.empty() ? "" : ", " + std::to_string(graph.buffers.size()) + " buffer(s)"));
    return result.graph;
}

namespace {

// Where the node or buffer @p id of @p graph was written, as a server
// error's subject names it (protocol.md §5, samples.md §4.6). Nothing when
// the subject is none, @p graph has no such node or buffer, or it was
// written in a library file.
std::optional<sourceLocation> sourceOf(const metadata::graphSpec& graph, protocol::failSubject subject,
                                       std::string_view id) {
    const auto at = [](const std::optional<metadata::sourceSpan>& src) -> std::optional<sourceLocation> {
        if (!src || interpreter::isLibraryPath(src->file)) return std::nullopt;
        return sourceLocation{.path = src->file,
                              .line = static_cast<std::size_t>(src->line),
                              .column = static_cast<std::size_t>(src->column)};
    };
    if (subject == protocol::failSubject::buffer) {
        const auto b = std::ranges::find_if(graph.buffers, [&](const auto& d) { return d.id == id; });
        if (b != graph.buffers.end()) return at(b->src);
    } else if (subject == protocol::failSubject::node) {
        const auto n = std::ranges::find_if(graph.nodes, [&](const auto& d) { return d.id == id; });
        if (n != graph.nodes.end()) return at(n->src);
    }
    return std::nullopt;
}

// Where node @p id was written, in the newest of @p plans that has it.
std::optional<sourceLocation> locateNode(std::string_view id,
                                         const std::map<std::int32_t, metadata::graphSpec>& plans) {
    for (const auto& [plan, graph] : std::views::reverse(plans)) {
        if (auto loc = sourceOf(graph, protocol::failSubject::node, id)) return loc;
    }
    return std::nullopt;
}

}  // namespace

void interpreterLink::sendPlan(const runRequest& request, const metadata::graphSpec& graph, runReply& reply) {
    if (!ensureConnected(reply)) return;
    const std::int32_t plan = nextPlan_++;
    const std::string ir = ir::write(graph);
    if (auto error = client_->submitIr(plan, ir)) {
        add(reply, consoleKind::error, "server: plan " + std::to_string(plan) + ": " + error->message,
            sourceOf(graph, error->subject, error->id));
        return;
    }
    add(reply, consoleKind::server,
        "server: plan " + std::to_string(plan) + " accepted (" + std::to_string(ir.size()) + " bytes)");
    {
        // Kept while it may play, so I/O errors can name their nodes'
        // sources (io.md §10.2); the newest few at most, in case a
        // retirement is never heard.
        const std::scoped_lock lock(mutex_);
        plans_[plan] = graph;
        while (plans_.size() > maxKeptPlans) plans_.erase(plans_.begin());
    }
    const auto r = send(
        address::play, {plan, static_cast<float>(request.fade), std::string(toString(request.quant))}, reply);
    if (!r.ok()) {
        add(reply, consoleKind::error, "server: /play: " + r.failText());
        return;
    }
    add(reply, consoleKind::server,
        "server: plan " + std::to_string(plan) + " scheduled (fade " + formatMilliseconds(request.fade) +
            ", quant " + std::string(toString(request.quant)) + ")");
}

runReply interpreterLink::process(const runRequest& request) {
    runReply reply;
    switch (request.what) {
    case runRequest::kind::connect: ensureConnected(reply); break;
    case runRequest::kind::play: {
        std::size_t errors = 0;
        std::size_t forms = 0;
        for (const auto& [path, text] : request.sources) {
            const auto result = interpreter::read(text);
            if (result.error) {
                const auto loc = locate(path, 1, 1, result.error->span);
                add(reply, consoleKind::error, where(loc) + ": " + result.error->message, loc);
                ++errors;
            }
            forms += result.data.size();
        }
        if (errors > 0) {
            // A failed compilation leaves the running plan unchanged (`interpreter.md` §13.3).
            add(reply, consoleKind::error,
                "play: " + std::to_string(errors) + " file(s) failed to read; the running plan is unchanged");
            break;
        }
        add(reply, consoleKind::info,
            "play: read " + std::to_string(request.sources.size()) + " file(s), " + std::to_string(forms) +
                " form(s)");
        // Connect first: the server's sample rate converts smp literals.
        if (const bool connected = ensureConnected(reply); const auto ir = compileProject(request, reply)) {
            if (connected) sendPlan(request, *ir, reply);
        }
        break;
    }
    case runRequest::kind::stop: {
        if (!ensureConnected(reply)) break;
        const auto r = send(address::stop, {static_cast<float>(request.fade)}, reply);
        if (r.ok()) {
            add(reply, consoleKind::server,
                "server: stopping (fade " + formatMilliseconds(request.fade) + ")");
        } else {
            add(reply, consoleKind::error, "server: /stop: " + r.failText());
        }
        break;
    }
    case runRequest::kind::evalForm: {
        const auto result = interpreter::read(request.form);
        if (result.error) {
            const auto loc = locate(request.file, request.line, request.column, result.error->span);
            add(reply, consoleKind::error, where(loc) + ": " + result.error->message, loc);
            break;
        }
        const sourceLocation loc{.path = request.file, .line = request.line, .column = request.column};
        std::string preview = request.form.substr(0, request.form.find('\n'));
        if (preview.size() > 40) preview = preview.substr(0, 40) + "...";
        add(reply, consoleKind::info, "eval " + where(loc) + ": read " + preview, loc);
        // Evaluating a form runs the full pipeline on the project, with the
        // form's buffer as it is, and swaps the result in (`interpreter.md` §13.6).
        // Connect first: the server's sample rate converts smp literals.
        if (const bool connected = ensureConnected(reply); const auto ir = compileProject(request, reply)) {
            if (connected) sendPlan(request, *ir, reply);
        }
        break;
    }
    }
    return reply;
}

void interpreterLink::onUnsolicited(const protocol::message& m) {
    runReply reply;
    {
        std::scoped_lock lock(mutex_);
        if (m.address == address::counters) {
            const serverStatus before = status_;
            status_.missed = m.int32(0).value_or(0);
            status_.xruns = m.int32(1).value_or(0);
            status_.drops = m.int32(2).value_or(0);
            status_.overflows = m.int32(3).value_or(0);
            status_.load = m.float32(4).value_or(0.0f);
            // Tempo, beat, and meter follow the counters (`protocol.md` §5).
            status_.bpm = m.float64(5);
            status_.beat = m.float64(6);
            status_.meter = m.int32(7).value_or(4);
            // The threads and their loads follow the I/O errors (`threads.md` §8).
            status_.threads = m.int32(9).value_or(1);
            status_.threadLoads.clear();
            for (std::size_t i = 0; std::cmp_less(i, status_.threads); ++i) {
                const auto l = m.float32(11 + i);
                if (!l) break;
                status_.threadLoads.push_back(*l);
            }
            // Counters arrive ten times per second; redraw only when what
            // the status bar shows changed.
            if (status_.missed == before.missed && status_.xruns == before.xruns &&
                status_.drops == before.drops && status_.overflows == before.overflows &&
                status_.loadText() == before.loadText() && status_.tempoText() == before.tempoText() &&
                status_.beatText() == before.beatText()) {
                return;
            }
        } else if (m.address == address::planActive) {
            status_.activePlan = m.int32(0).value_or(0);
            add(reply, consoleKind::server, "server: plan " + std::to_string(status_.activePlan) + " active");
        } else if (m.address == address::planRetired) {
            const auto plan = m.int32(0).value_or(0);
            if (status_.activePlan == plan) status_.activePlan = 0;
            plans_.erase(plan);
            add(reply, consoleKind::server, "server: plan " + std::to_string(plan) + " retired");
        } else if (m.address == address::bufferSaved) {
            add(reply, consoleKind::server,
                "server: saved buffer " + std::string(m.string(0).value_or("?")) + " to " +
                    std::string(m.string(1).value_or("?")));
        } else if (m.address == address::ioError) {
            // A node's I/O errors since the last report (io.md §10.2).
            const auto count = m.int32(2).value_or(1);
            add(reply, consoleKind::error,
                "server: " + std::string(m.string(1).value_or("I/O error")) +
                    (count > 1 ? " (" + std::to_string(count) + " errors)" : ""),
                locateNode(m.string(0).value_or(""), plans_));
        } else if (m.address == address::fail) {
            add(reply, consoleKind::error, "server: " + std::string(m.string(2).value_or("failure")));
        } else {
            add(reply, consoleKind::server, "server: " + protocol::describe(m));
        }
        reply.status = status_;
    }
    push(std::move(reply));
}

}  // namespace workspace
