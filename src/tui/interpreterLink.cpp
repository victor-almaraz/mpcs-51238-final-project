#include <proj/tui/interpreterLink.h>

#include <cmath>
#include <sstream>

#include <proj/interpreter/reader.h>

namespace tui {

namespace {

std::string formatFade(double seconds) {
    std::ostringstream out;
    out << std::round(seconds * 1000.0 * 1000.0) / 1000.0 << "ms";
    return out.str();
}

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

} // namespace

runReply processRequest(const runRequest& request) {
    runReply reply;
    auto add = [&](consoleKind kind, std::string text, std::optional<sourceLocation> loc = {}) {
        reply.entries.push_back({kind, std::move(text), std::move(loc)});
    };
    const std::string options =
        "fade " + formatFade(request.fade) + ", quant " + std::string(toString(request.quant));

    switch (request.what) {
    case runRequest::kind::play: {
        std::size_t errors = 0;
        std::size_t forms = 0;
        for (const auto& [path, text] : request.sources) {
            const auto result = interpreter::read(text);
            if (result.error) {
                const auto loc = locate(path, 1, 1, result.error->span);
                add(consoleKind::error, where(loc) + ": " + result.error->message, loc);
                ++errors;
            }
            forms += result.data.size();
        }
        if (errors > 0) {
            add(consoleKind::error, "play: " + std::to_string(errors) +
                                        " file(s) failed to read; the running plan is unchanged");
            break;
        }
        add(consoleKind::info, "play: read " + std::to_string(request.sources.size()) +
                                   " file(s), " + std::to_string(forms) + " form(s) (" + options +
                                   ")");
        add(consoleKind::server,
            "play: evaluation and IR emission are not implemented; nothing was sent to the server");
        break;
    }
    case runRequest::kind::stop:
        add(consoleKind::server, "stop (" + formatFade(request.fade) +
                                     " fade): not connected to a server");
        break;
    case runRequest::kind::evalForm: {
        const auto result = interpreter::read(request.form);
        if (result.error) {
            const auto loc = locate(request.file, request.line, request.column, result.error->span);
            add(consoleKind::error, where(loc) + ": " + result.error->message, loc);
            break;
        }
        const sourceLocation loc{request.file, request.line, request.column};
        std::string preview = request.form.substr(0, request.form.find('\n'));
        if (preview.size() > 40) preview = preview.substr(0, 40) + "...";
        add(consoleKind::info, "eval " + where(loc) + ": read " + preview, loc);
        add(consoleKind::server, "eval: evaluation is not implemented; nothing was sent to the server");
        break;
    }
    }
    return reply;
}

interpreterLink::interpreterLink(std::function<void()> wake)
    : wake_(std::move(wake)), worker_([this] { work(); }) {}

interpreterLink::~interpreterLink() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wakeWorker_.notify_all();
    worker_.join();
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

void interpreterLink::work() {
    std::unique_lock lock(mutex_);
    while (true) {
        wakeWorker_.wait(lock, [this] { return stopping_ || !requests_.empty(); });
        if (stopping_) return;
        runRequest request = std::move(requests_.front());
        requests_.pop_front();
        lock.unlock();
        runReply reply = processRequest(request);
        lock.lock();
        replies_.push_back(std::move(reply));
        --inFlight_;
        const auto wake = wake_;
        lock.unlock();
        if (wake) wake();
        idle_.notify_all();
        lock.lock();
    }
}

} // namespace tui
