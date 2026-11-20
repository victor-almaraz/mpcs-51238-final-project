// The web bridge's transport on libcurl's multi interface (io.md §3.3,
// §3.4), and the thread that runs the bridge on it.

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/primitives/io.h>
#include <espresso/protocol/jthread.h>

#include "webBridge.h"
#include "webTransport.h"

namespace server {

/// HTTP and WebSocket transfers through one libcurl multi handle, driven
/// from the thread that calls wait(). WebSockets connect through the multi
/// handle and are then read and written directly, so a slow endpoint
/// delays only its own transfers.
class curlTransport final : public httpTransport {
public:
    curlTransport();
    ~curlTransport() override;

    curlTransport(const curlTransport&) = delete;
    curlTransport& operator=(const curlTransport&) = delete;
    curlTransport(curlTransport&&) = delete;
    curlTransport& operator=(curlTransport&&) = delete;

    id start(const httpRequest& request, handlers events) override;
    id openSocket(const std::string& url, const std::vector<std::string>& headers,
                  socketHandlers events) override;
    void sendText(id socket, std::string_view text) override;
    void stop(id transfer) override;
    void wait(std::chrono::milliseconds most) override;

    /// One transfer or socket; public so libcurl's C callbacks can reach it.
    struct transfer;

private:
    // A transfer for @p url with @p headers and the options every transfer
    // takes, not yet added.
    std::unique_ptr<transfer> newTransfer(const std::string& url, const std::vector<std::string>& headers);
    // Adds @p t to the multi handle and keeps it; its id.
    id add(std::unique_ptr<transfer> t);
    // Takes @p t out of the multi handle, when it is in it.
    void detach(transfer& t);
    void finish(id which, int result);
    void readSockets();

    struct multiHandle;
    std::unique_ptr<multiHandle> multi_;
    std::map<id, std::unique_ptr<transfer>> transfers_;
    id next_ = 1;
};

/// Runs a web bridge on a curlTransport on its own thread (io.md §3.4).
class webService {
public:
    /// Starts the bridge between @p io's web ports and the network,
    /// counting in @p stats.
    webService(primitives::ioHub& io, ioStats& stats, webOptions options);
    ~webService();

    webService(const webService&) = delete;
    webService& operator=(const webService&) = delete;
    webService(webService&&) = delete;
    webService& operator=(webService&&) = delete;

private:
    curlTransport transport_;
    webBridge bridge_;
    espresso::jthread thread_;  // last: stops and joins before the bridge and transport go
};

}  // namespace server
