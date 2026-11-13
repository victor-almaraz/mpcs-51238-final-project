#include <proj/tui/interpreterLink.h>

#include <chrono>
#include <cmath>
#include <sstream>
#include <utility>

#include <proj/interpreter/reader.h>
#include <proj/metadata/version.h>
#include <proj/protocol/messages.h>

namespace tui {

namespace {

namespace address = protocol::address;

// A connected server that sends nothing for this long is considered gone;
// it sends /counters ten times per second (`protocol.md` §5).
constexpr auto silenceLimit = std::chrono::milliseconds(1500);

// Line and column of a reader error inside a form that starts at
// (line, column) in its file.
sourceLocation locate(const std::filesystem::path& file, std::size_t line, std::size_t column,
                      const interpreter::sourceSpan& span) {
    sourceLocation loc{file, line + span.line - 1, span.column};
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

void add(runReply& reply, consoleKind kind, std::string text,
         std::optional<sourceLocation> loc = {}) {
    reply.entries.push_back({kind, std::move(text), std::move(loc)});
}

} // namespace

std::string interpreterLink::placeholderIr() {
    return R"({"format":"proj-ir","version":0,"nodes":[],"edges":[]})";
}

interpreterLink::interpreterLink(linkOptions options, std::function<void()> wake)
    : options_(std::move(options)), wake_(std::move(wake)), worker_([this] { work(); }) {
    if (options_.connectOnStart) {
        runRequest connect;
        connect.what = runRequest::kind::connect;
        submit(std::move(connect));
    }
}

interpreterLink::~interpreterLink() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wakeWorker_.notify_all();
    worker_.join();
    client_.reset();  // joins the receiver thread before members go away
}

void interpreterLink::setWake(std::function<void()> wake) {
    std::lock_guard lock(mutex_);
    wake_ = std::move(wake);
}

void interpreterLink::submit(runRequest request) {
    {
        std::lock_guard lock(mutex_);
        requests_.push_back(std::move(request));
        ++inFlight_;
    }
    wakeWorker_.notify_one();
}

std::vector<runReply> interpreterLink::takeReplies() {
    std::lock_guard lock(mutex_);
    return std::exchange(replies_, {});
}

void interpreterLink::waitIdle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return inFlight_ == 0; });
}

void interpreterLink::push(runReply reply) {
    std::function<void()> wake;
    {
        std::lock_guard lock(mutex_);
        replies_.push_back(std::move(reply));
        wake = wake_;
    }
    if (wake) wake();
}

serverStatus interpreterLink::statusSnapshot() {
    std::lock_guard lock(mutex_);
    return status_;
}

void interpreterLink::work() {
    std::unique_lock lock(mutex_);
    while (true) {
        wakeWorker_.wait_for(lock, std::chrono::milliseconds(250),
                             [this] { return stopping_ || !requests_.empty(); });
        if (stopping_) return;
        if (requests_.empty()) {
            lock.unlock();
            checkSilence();
            lock.lock();
            continue;
        }
        runRequest request = std::move(requests_.front());
        requests_.pop_front();
        lock.unlock();
        push(process(request));
        lock.lock();
        --inFlight_;
        idle_.notify_all();
    }
}

void interpreterLink::checkSilence() {
    if (!client_) return;
    {
        std::lock_guard lock(mutex_);
        if (!status_.connected) return;
    }
    if (std::chrono::steady_clock::now() - client_->lastHeard() < silenceLimit) return;
    runReply reply;
    {
        std::lock_guard lock(mutex_);
        status_.connected = false;
        status_.activePlan = 0;
        reply.status = status_;
    }
    add(reply, consoleKind::server,
        "server: connection to " + serverAddress(options_.server) + " lost");
    push(std::move(reply));
}

bool interpreterLink::ensureConnected(runReply& reply) {
    if (statusSnapshot().connected) return true;
    if (!client_) {
        client_ = std::make_unique<protocol::client>(
            options_.server, [this](const protocol::message& m) { onUnsolicited(m); });
        std::string error;
        if (!client_->open(&error)) {
            add(reply, consoleKind::error, "server: cannot open a client socket: " + error);
            client_.reset();
            return false;
        }
    }
    const auto hello = client_->request(
        address::hello, {options_.clientVersion, std::string(metadata::version)});
    if (!hello.reply) {
        add(reply, consoleKind::server,
            "server: not reachable at " + serverAddress(options_.server) +
                " (start proj-stub-server, then press Play)");
        return false;
    }
    if (!hello.ok()) {
        // A metadata mismatch means the client must not submit IR (`protocol.md` §4.2).
        add(reply, consoleKind::error, "server: " + hello.failText());
        return false;
    }
    const auto& ok = *hello.reply;
    const std::string name(ok.string(1).value_or("server"));
    if (!client_->request(address::notify, {std::int32_t{1}}).ok()) {
        add(reply, consoleKind::server, "server: /notify failed; counters will not update");
    }
    {
        std::lock_guard lock(mutex_);
        status_.connected = true;
        status_.server = name;
        reply.status = status_;
    }
    add(reply, consoleKind::server,
        "server: connected to " + name + " at " + serverAddress(options_.server) + " (" +
            std::to_string(ok.int32(2).value_or(0)) + " Hz, " +
            std::to_string(ok.int32(3).value_or(0)) + "-sample blocks)");
    return true;
}

// Sends a request; a server that forgot this client (restart) gets a new
// /hello and one more try.
protocol::requestResult interpreterLink::send(std::string_view addr,
                                              std::vector<protocol::argument> args,
                                              runReply& reply) {
    auto r = client_->request(addr, args);
    if (r.failCode() == protocol::errorCode::notConnected || !r.reply) {
        {
            std::lock_guard lock(mutex_);
            status_.connected = false;
        }
        if (!ensureConnected(reply)) return r;
        r = client_->request(addr, std::move(args));
    }
    return r;
}

void interpreterLink::sendPlan(const runRequest& request, runReply& reply) {
    add(reply, consoleKind::info,
        "play: the compiler is not implemented; sending an empty placeholder graph");
    if (!ensureConnected(reply)) return;
    const std::int32_t plan = nextPlan_++;
    const std::string ir = placeholderIr();
    if (auto error = client_->submitIr(plan, ir)) {
        add(reply, consoleKind::error, "server: plan " + std::to_string(plan) + ": " + *error);
        return;
    }
    add(reply, consoleKind::server,
        "server: plan " + std::to_string(plan) + " accepted (" + std::to_string(ir.size()) +
            " bytes)");
    const auto r = send(address::play,
                        {plan, static_cast<float>(request.fade), std::string(toString(request.quant))},
                        reply);
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
    case runRequest::kind::connect:
        ensureConnected(reply);
        break;
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
                "play: " + std::to_string(errors) +
                    " file(s) failed to read; the running plan is unchanged");
            break;
        }
        add(reply, consoleKind::info,
            "play: read " + std::to_string(request.sources.size()) + " file(s), " +
                std::to_string(forms) + " form(s)");
        sendPlan(request, reply);
        break;
    }
    case runRequest::kind::stop: {
        if (!ensureConnected(reply)) break;
        const auto r = send(address::stop, {static_cast<float>(request.fade)}, reply);
        if (r.ok()) {
            add(reply, consoleKind::server, "server: stopping (fade " + formatMilliseconds(request.fade) + ")");
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
        const sourceLocation loc{request.file, request.line, request.column};
        std::string preview = request.form.substr(0, request.form.find('\n'));
        if (preview.size() > 40) preview = preview.substr(0, 40) + "...";
        add(reply, consoleKind::info, "eval " + where(loc) + ": read " + preview, loc);
        // Evaluating a form runs the full pipeline and swaps the result in
        // (`interpreter.md` §13.6).
        sendPlan(request, reply);
        break;
    }
    }
    return reply;
}

void interpreterLink::onUnsolicited(const protocol::message& m) {
    runReply reply;
    {
        std::lock_guard lock(mutex_);
        if (m.address == address::counters) {
            const serverStatus before = status_;
            status_.missed = m.int32(0).value_or(0);
            status_.xruns = m.int32(1).value_or(0);
            status_.drops = m.int32(2).value_or(0);
            status_.overflows = m.int32(3).value_or(0);
            status_.load = m.float32(4).value_or(0.0f);
            // Counters arrive ten times per second; redraw only on change.
            if (status_.missed == before.missed && status_.xruns == before.xruns &&
                status_.drops == before.drops && status_.overflows == before.overflows &&
                status_.load == before.load) {
                return;
            }
        } else if (m.address == address::planActive) {
            status_.activePlan = m.int32(0).value_or(0);
            add(reply, consoleKind::server, "server: plan " + std::to_string(status_.activePlan) + " active");
        } else if (m.address == address::planRetired) {
            const auto plan = m.int32(0).value_or(0);
            if (status_.activePlan == plan) status_.activePlan = 0;
            add(reply, consoleKind::server, "server: plan " + std::to_string(plan) + " retired");
        } else if (m.address == address::fail) {
            add(reply, consoleKind::error, "server: " + std::string(m.string(2).value_or("failure")));
        } else {
            add(reply, consoleKind::server, "server: " + protocol::describe(m));
        }
        reply.status = status_;
    }
    push(std::move(reply));
}

} // namespace tui
