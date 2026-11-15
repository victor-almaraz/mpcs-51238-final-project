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
#include <vector>

#include <proj/protocol/client.h>
#include <proj/protocol/jthread.h>
#include <proj/tui/consoleLog.h>
#include <proj/tui/settings.h>

namespace tui {

/// Server state shown in the status bar (`tui.md` §4).
struct serverStatus {
    /// True after a successful `/hello`.
    bool connected = false;
    std::string server;  ///< Server name and version from `/hello.ok`
    /// Tempo, in beats per minute.
    double bpm = 120.0;
    /// Beat position, when the server reports one.
    std::optional<double> beat;
    /// Missed deadlines.
    long missed = 0;
    /// Device underruns and overruns.
    long xruns = 0;
    /// Dropped requests.
    long drops = 0;
    /// Ring buffer overflows.
    long overflows = 0;
    /// Audio thread load, from 0 to 1.
    float load = 0.0f;
    /// The active plan, or 0.
    std::int32_t activePlan = 0;
};

/// How the link reaches the server.
struct linkOptions {
    /// Server port, timeout, and retries.
    protocol::clientOptions server;
    /// Version sent in `/hello`.
    std::string clientVersion = "0";
    /// Connect when the link starts, rather than on the first Play.
    bool connectOnStart = true;
};

/// A run command sent to the interpreter.
struct runRequest {
    /// Request kinds.
    enum class kind {
        connect,   ///< Connect to the server
        play,      ///< Read the project and play it
        stop,      ///< Stop with a fade
        evalForm,  ///< Read one form and play it (`interpreter.md` §13.6)
    };

    /// What to do.
    kind what = kind::play;
    /// For play and evalForm: the project's sources by path relative to
    /// the project root, with unsaved buffer contents in place of the
    /// files on disk.
    std::map<std::filesystem::path, std::string> sources;
    /// For evalForm: the form's text.
    std::string form;
    /// For evalForm: the form's file. For play: the file being edited.
    std::filesystem::path file;
    /// For evalForm: the line the form starts on, from 1.
    std::size_t line = 1;
    /// For evalForm: the column the form starts at, from 1.
    std::size_t column = 1;
    /// Swap or stop fade, in seconds.
    double fade = 0.05;
    /// Swap quantization grid.
    quantization quant = quantization::none;
};

/// The interpreter's reply: console entries to show and, when it
/// changed, the server status.
struct runReply {
    /// Console entries to show.
    std::vector<consoleEntry> entries;
    /// The server status, when it changed.
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

    /// Replaces the wake callback.
    void setWake(std::function<void()> wake);
    /// Queues @p request for the worker thread.
    void submit(runRequest request);

    /// Removes and returns the queued replies.
    std::vector<runReply> takeReplies();

    /// Blocks until every submitted request has a reply. For tests.
    void waitIdle();

    /// The file a project plays: `main.lisp`; otherwise @p current, the
    /// file being edited, when it is among the sources; otherwise the only
    /// file. Empty when none applies.
    static std::filesystem::path entryFile(const std::map<std::filesystem::path, std::string>& sources,
                                           const std::filesystem::path& current = {});

private:
    void work(const proj::stop_token& stop);
    runReply process(const runRequest& request);
    bool ensureConnected(runReply& reply);
    protocol::requestResult send(std::string_view addr, std::vector<protocol::argument> args,
                                 runReply& reply);
    std::optional<std::string> compileProject(const runRequest& request, runReply& reply);
    void sendPlan(const runRequest& request, const std::string& ir, runReply& reply);
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
    std::mutex wakeMutex_;
    std::function<void()> wake_;  // guarded by wakeMutex_
    std::size_t inFlight_ = 0;
    serverStatus status_;  // guarded by mutex_

    // Worker thread only.
    std::unique_ptr<protocol::client> client_;
    std::int32_t nextPlan_ = 1;
    double serverRate_ = 48000.0;  // for smp literals

    // Last, so it stops and joins before the members it uses are destroyed.
    proj::jthread worker_;
};

} // namespace tui
