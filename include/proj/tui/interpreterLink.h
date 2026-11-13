/// @file
/// Message queue between the editor and the interpreter (`tui.md` §9.2,
/// §9.5). Requests run on a worker thread; the editor collects replies
/// on its own thread and never calls the interpreter directly.

#pragma once

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <proj/tui/consoleLog.h>
#include <proj/tui/settings.h>

namespace tui {

/// A run command sent to the interpreter.
struct runRequest {
    enum class kind { play, stop, evalForm };

    kind what = kind::play;
    /// For play: the project's sources by path, with unsaved buffer
    /// contents in place of the files on disk.
    std::map<std::filesystem::path, std::string> sources;
    /// For evalForm: the form's text, its file, and where it starts.
    std::string form;
    std::filesystem::path file;
    std::size_t line = 1;
    std::size_t column = 1;
    double fade = 0.05;
    quantization quant = quantization::none;
};

/// The interpreter's reply: console entries to show.
struct runReply {
    std::vector<consoleEntry> entries;
};

/// Processes one request. Runs on the worker thread.
runReply processRequest(const runRequest& request);

/// Worker thread with request and reply queues.
class interpreterLink {
public:
    /// @p wake is called from the worker thread after a reply is queued,
    /// so the editor can collect it on its own thread.
    explicit interpreterLink(std::function<void()> wake = {});
    ~interpreterLink();

    interpreterLink(const interpreterLink&) = delete;
    interpreterLink& operator=(const interpreterLink&) = delete;

    void setWake(std::function<void()> wake);
    void submit(runRequest request);

    /// Removes and returns the queued replies.
    std::vector<runReply> takeReplies();

    /// Blocks until every submitted request has a reply. For tests.
    void waitIdle();

private:
    void work();

    std::mutex mutex_;
    std::condition_variable wakeWorker_;
    std::condition_variable idle_;
    std::deque<runRequest> requests_;
    std::vector<runReply> replies_;
    std::function<void()> wake_;
    std::size_t inFlight_ = 0;
    bool stopping_ = false;
    std::thread worker_;
};

} // namespace tui
