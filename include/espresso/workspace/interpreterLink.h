/// @file
/// Message queue between the editor and the interpreter (`tui.md` §9.2,
/// §9.5). Requests run on a worker thread, which also holds the
/// connection to the server (`interpreter.md` §13.4). The editor
/// collects replies on its own thread and never calls the interpreter or
/// the server directly.

#pragma once

#include <chrono>
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

#include <espresso/metadata/graph.h>
#include <espresso/protocol/client.h>
#include <espresso/protocol/jthread.h>
#include <espresso/workspace/consoleLog.h>
#include <espresso/workspace/settings.h>

namespace workspace {

/// Server state shown in the status bar (`tui.md` §3, §4).
struct serverStatus {
    /// True after a successful `/hello`.
    bool connected = false;
    std::string server;  ///< Server name and version from `/hello.ok`
    /// Tempo of the server's beat clock, in beats per minute, from
    /// `/counters`; empty until the server reports one.
    std::optional<double> bpm;
    /// Position of the server's beat clock, in beats, from `/counters`;
    /// empty until the server reports one.
    std::optional<double> beat;
    /// Beats per bar of the active plan, from `/counters`.
    std::int32_t meter = 4;
    /// Missed deadlines.
    std::int32_t missed = 0;
    /// Device underruns and overruns.
    std::int32_t xruns = 0;
    /// Dropped requests.
    std::int32_t drops = 0;
    /// Ring buffer overflows.
    std::int32_t overflows = 0;
    /// Audio thread load, from 0 to 1.
    float load = 0.0f;
    /// The threads the server runs a block on, from `/counters`
    /// (`threads.md` §8).
    std::int32_t threads = 1;
    /// Each thread's load, the audio thread first; empty from a server that
    /// does not report them.
    std::vector<float> threadLoads;
    /// The active plan, or 0.
    std::int32_t activePlan = 0;

    /// The busiest thread's load: the largest of `threadLoads`, or `load`
    /// when there are none.
    float busiest() const;
    /// The load as the status bar shows it: `Load 45%`, the busiest
    /// thread's, then ` · Threads 4` for more than one thread.
    std::string loadText() const;
    /// The tempo as the status bar shows it: `120 BPM`, to one decimal
    /// place, or `— BPM` when not connected or not reported.
    std::string tempoText() const;
    /// The beat position as the status bar shows it: `bar.beat`, both
    /// counted from 1, such as `9.3`; or `—` when not connected, when no
    /// plan is active, or when not reported.
    std::string beatText() const;
};

/// How the link reaches the server.
struct linkOptions {
    /// Server port, timeout, and retries.
    protocol::clientOptions server;
    /// Version sent in `/hello`.
    std::string clientVersion = "0";
    /// Connect when the link starts, rather than on the first Play.
    bool connectOnStart = true;
    /// Time between background attempts to reach a server that did not
    /// answer, until one answers.
    std::chrono::milliseconds retryInterval{1000};
};

/// A run command sent to the interpreter.
struct runRequest {
    /// Request kinds.
    enum class kind : std::uint8_t {
        connect,   ///< Connect to the server
        play,      ///< Read the project and play it
        stop,      ///< Stop with a fade
        evalForm,  ///< Read one form and play it (`interpreter.md` §13.6)
    };

    /// What to do.
    kind what = kind::play;
    /// The caller's tag, carried to the request's reply; 0 when the caller
    /// needs none.
    std::uint64_t id = 0;
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
    /// The `id` of the request this replies to, or 0 for a reply no tagged
    /// request asked for: the server's notifications, a lost connection,
    /// and background connection attempts.
    std::uint64_t id = 0;
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

private:
    void work(const espresso::stop_token& stop);
    runReply process(const runRequest& request);
    bool ensureConnected(runReply& reply, bool background = false);
    void retryIfDue();
    protocol::requestResult send(std::string_view addr, std::vector<protocol::argument> args,
                                 runReply& reply);
    std::optional<metadata::graphSpec> compileProject(const runRequest& request, runReply& reply);
    void sendPlan(const runRequest& request, const metadata::graphSpec& graph, runReply& reply);
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
    std::int32_t nextPlan_ = 1;
    // Plans accepted and not yet retired, newest last, for locating the
    // nodes that /io.error names. Guarded by mutex_.
    std::map<std::int32_t, metadata::graphSpec> plans_;
    static constexpr std::size_t maxKeptPlans = 8;
    double serverRate_ = 48000.0;  // for smp literals
    // When to send the next background /hello; empty while connected, and
    // after a server answered with a refusal.
    std::optional<std::chrono::steady_clock::time_point> retryAt_;

    // Used by the worker thread. Its receive thread calls onUnsolicited(),
    // which uses the members above, so it is declared after them: it is
    // destroyed, and its thread joined, before they are.
    std::unique_ptr<protocol::client> client_;
    // Last, so it stops and joins before the members it uses are destroyed.
    espresso::jthread worker_;
};

}  // namespace workspace
