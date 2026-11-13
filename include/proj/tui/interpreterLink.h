/// @file
/// Message queue between the editor and the interpreter (`tui.md` §9.2,
/// §9.5). Requests run on a worker thread, which also holds the
/// connection to the server (`interpreter.md` §13.4). The editor
/// collects replies on its own thread and never calls the interpreter or
/// the server directly.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <proj/protocol/client.h>
#include <proj/tui/consoleLog.h>
#include <proj/tui/settings.h>

namespace tui {

/// Server state shown in the status bar (`tui.md` §4).
struct serverStatus {
    bool connected = false;
    std::string server;  ///< Server name and version from `/hello.ok`
    double bpm = 120.0;
    std::optional<double> beat;
    long missed = 0;
    long xruns = 0;
    long drops = 0;
    long overflows = 0;
    float load = 0.0f;
    std::int32_t activePlan = 0;
};

/// How the link reaches the server.
struct linkOptions {
    protocol::clientOptions server;
    std::string clientVersion = "0";
    bool connectOnStart = true;
};

/// A run command sent to the interpreter.
struct runRequest {
    enum class kind { connect, play, stop, evalForm };

    kind what = kind::play;
    /// For play and evalForm: the project's sources by path, with unsaved
    /// buffer contents in place of the files on disk.
    std::map<std::filesystem::path, std::string> sources;
    /// For evalForm: the form's text, its file, and where it starts.
    std::string form;
    std::filesystem::path file;
    std::size_t line = 1;
    std::size_t column = 1;
    double fade = 0.05;
    quantization quant = quantization::none;
};

/// The interpreter's reply: console entries to show and, when it
/// changed, the server status.
struct runReply {
    std::vector<consoleEntry> entries;
    std::optional<serverStatus> status;
};

/// Worker thread with request and reply queues.
class interpreterLink {
public:
    /// @p wake is called from a worker thread after a reply is queued, so
    /// the editor can collect it on its own thread.
    explicit interpreterLink(linkOptions options = {}, std::function<void()> wake = {});
    ~interpreterLink();

    interpreterLink(const interpreterLink&) = delete;
    interpreterLink& operator=(const interpreterLink&) = delete;

    void setWake(std::function<void()> wake);
    void submit(runRequest request);

    /// Removes and returns the queued replies.
    std::vector<runReply> takeReplies();

    /// Blocks until every submitted request has a reply. For tests.
    void waitIdle();

    /// The serialized IR sent for a project until the compiler exists:
    /// a graph with no nodes.
    static std::string placeholderIr();

private:
    void work();
    runReply process(const runRequest& request);
    bool ensureConnected(runReply& reply);
    protocol::requestResult send(std::string_view addr, std::vector<protocol::argument> args,
                                 runReply& reply);
    void sendPlan(const runRequest& request, runReply& reply);
    void onUnsolicited(const protocol::message& m);
    void checkSilence();
    void push(runReply reply);
    serverStatus statusSnapshot();

    linkOptions options_;
    std::mutex mutex_;
    std::condition_variable wakeWorker_;
    std::condition_variable idle_;
    std::deque<runRequest> requests_;
    std::vector<runReply> replies_;
    std::function<void()> wake_;
    std::size_t inFlight_ = 0;
    bool stopping_ = false;
    serverStatus status_;  // guarded by mutex_

    // Worker thread only.
    std::unique_ptr<protocol::client> client_;
    std::int32_t nextPlan_ = 1;

    std::thread worker_;
};

} // namespace tui
