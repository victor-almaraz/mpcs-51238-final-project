#include "curlTransport.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <espresso/primitives/io.h>
#include <espresso/protocol/jthread.h>

#include "ioStats.h"
#include "webBridge.h"
#include "webData.h"
#include "webTransport.h"

#include <curl/curl.h>

namespace server {

namespace {

// libcurl is a C library: its handles are pointers, owned here by
// unique_ptrs with its free functions, and its options and callbacks pass
// through varargs and void* (modern-cpp skill §9).
struct easyFree {
    void operator()(CURL* e) const noexcept { curl_easy_cleanup(e); }
};
struct listFree {
    void operator()(curl_slist* l) const noexcept { curl_slist_free_all(l); }
};
struct multiFree {
    void operator()(CURLM* m) const noexcept { curl_multi_cleanup(m); }
};

// Sets option @p option of @p easy; the one varargs call.
template <typename T>
void set(CURL* easy, CURLoption option, T value) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): curl_easy_setopt takes its value as varargs
    curl_easy_setopt(easy, option, value);
}

// Reads @p info of @p easy into @p out; the other varargs call.
template <typename T>
void get(CURL* easy, CURLINFO info, T& out) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): curl_easy_getinfo returns its value through varargs
    curl_easy_getinfo(easy, info, &out);
}

// Reads at most this many socket frames per wait(), so one busy socket
// cannot hold the thread.
constexpr int framesPerWait = 256;

}  // namespace

struct curlTransport::multiHandle {
    std::unique_ptr<CURLM, multiFree> handle{curl_multi_init()};
};

struct curlTransport::transfer {
    curlTransport::id id = 0;
    std::unique_ptr<CURL, easyFree> easy;
    std::unique_ptr<curl_slist, listFree> headers;
    std::array<char, CURL_ERROR_SIZE> error{};
    bool inMulti = false;
    bool open = false;  // a socket whose upgrade is done
    bool stopped = false;
    // What it reports: a request's handlers, or a socket's.
    std::variant<handlers, socketHandlers> events;
    std::string message;  // a WebSocket message being joined from its frames

    bool socket() const noexcept { return std::holds_alternative<socketHandlers>(events); }
};

namespace {

// libcurl's write callback: a piece of a response body for @p user's
// transfer, which is a request: only requests set it.
std::size_t onData(char* data, std::size_t size, std::size_t count, void* user) {
    auto& t = *static_cast<curlTransport::transfer*>(user);
    const std::size_t n = size * count;
    if (t.stopped || t.socket()) return n;
    const auto& events = std::get<curlTransport::handlers>(t.events);
    if (!events.data) return n;
    long status = 0;
    get(t.easy.get(), CURLINFO_RESPONSE_CODE, status);
    events.data(status, std::string_view(data, n));
    return n;
}

void initialize() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::unique_ptr<curl_slist, listFree> headerList(const std::vector<std::string>& headers) {
    curl_slist* list = nullptr;
    for (const auto& h : headers) list = curl_slist_append(list, h.c_str());
    return std::unique_ptr<curl_slist, listFree>(list);
}

}  // namespace

curlTransport::curlTransport() {
    initialize();
    multi_ = std::make_unique<multiHandle>();
}

// Each handle leaves the multi handle before it is freed.
curlTransport::~curlTransport() {
    for (auto& [id, t] : transfers_) detach(*t);
    transfers_.clear();
}

std::unique_ptr<curlTransport::transfer> curlTransport::newTransfer(const std::string& url,
                                                                    const std::vector<std::string>& headers) {
    auto t = std::make_unique<transfer>();
    t->id = next_++;
    t->easy.reset(curl_easy_init());
    t->headers = headerList(headers);
    CURL* e = t->easy.get();
    set(e, CURLOPT_URL, url.c_str());
    set(e, CURLOPT_NOSIGNAL, 1L);
    set(e, CURLOPT_HTTPHEADER, t->headers.get());
    set(e, CURLOPT_ERRORBUFFER, t->error.data());
    set(e, CURLOPT_PRIVATE, t.get());
    return t;
}

curlTransport::id curlTransport::add(std::unique_ptr<transfer> t) {
    curl_multi_add_handle(multi_->handle.get(), t->easy.get());
    t->inMulti = true;
    const id which = t->id;
    transfers_.emplace(which, std::move(t));
    return which;
}

void curlTransport::detach(transfer& t) {
    if (t.inMulti) curl_multi_remove_handle(multi_->handle.get(), t.easy.get());
    t.inMulti = false;
}

curlTransport::id curlTransport::start(const httpRequest& request, handlers events) {
    auto t = newTransfer(request.url, request.headers);
    t->events = std::move(events);
    CURL* e = t->easy.get();
    set(e, CURLOPT_PROTOCOLS_STR, "http,https");
    set(e, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    set(e, CURLOPT_FOLLOWLOCATION, 1L);
    set(e, CURLOPT_MAXREDIRS, 5L);
    set(e, CURLOPT_WRITEFUNCTION, onData);
    set(e, CURLOPT_WRITEDATA, t.get());
    if (request.method != primitives::httpMethod::get) {
        if (request.method == primitives::httpMethod::put) set(e, CURLOPT_CUSTOMREQUEST, "PUT");
        set(e, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
        set(e, CURLOPT_COPYPOSTFIELDS, request.body.c_str());
    }
    if (request.timeout.count() > 0) set(e, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
    return add(std::move(t));
}

curlTransport::id curlTransport::openSocket(const std::string& url, const std::vector<std::string>& headers,
                                            socketHandlers events) {
    auto t = newTransfer(url, headers);
    t->events = std::move(events);
    CURL* e = t->easy.get();
    set(e, CURLOPT_PROTOCOLS_STR, "ws,wss");
    // The transfer ends once the upgrade is done; the socket is then read
    // and written with curl_ws_recv and curl_ws_send.
    set(e, CURLOPT_CONNECT_ONLY, 2L);
    return add(std::move(t));
}

void curlTransport::sendText(id socket, std::string_view text) {
    const auto it = transfers_.find(socket);
    if (it == transfers_.end() || !it->second->open) return;
    std::size_t sent = 0;
    curl_ws_send(it->second->easy.get(), text.data(), text.size(), &sent, 0, CURLWS_TEXT);
}

void curlTransport::stop(id which) {
    const auto it = transfers_.find(which);
    if (it == transfers_.end()) return;
    it->second->stopped = true;
    detach(*it->second);
    // Freed after the round that may be calling back into it.
}

void curlTransport::finish(id which, int result) {
    const auto it = transfers_.find(which);
    if (it == transfers_.end()) return;
    transfer& t = *it->second;
    const auto code = static_cast<CURLcode>(result);
    // An opened socket's handle stays in the multi handle: libcurl reads and
    // writes a connect-only connection only while it is added
    // (CURLOPT_CONNECT_ONLY). Everything else leaves it now.
    if (!t.socket() || code != CURLE_OK) detach(t);
    if (t.stopped) return;
    std::string error;
    if (code != CURLE_OK) error = t.error[0] != '\0' ? std::string(t.error.data()) : curl_easy_strerror(code);
    if (t.socket()) {
        const auto& events = std::get<socketHandlers>(t.events);
        if (code == CURLE_OK) {
            t.open = true;
            if (events.opened) events.opened();
        } else {
            t.stopped = true;
            if (events.closed) events.closed(error);
        }
        return;
    }
    long status = 0;
    get(t.easy.get(), CURLINFO_RESPONSE_CODE, status);
    t.stopped = true;
    if (const auto& events = std::get<handlers>(t.events); events.done)
        events.done(code == CURLE_OK ? status : 0, error);
}

void curlTransport::readSockets() {
    std::vector<id> open;
    for (const auto& [which, t] : transfers_) {
        if (t->open && !t->stopped) open.push_back(which);
    }
    std::array<char, 16384> buffer{};  // one buffer for every frame of the round
    for (const id which : open) {
        for (int n = 0; n < framesPerWait; ++n) {
            const auto it = transfers_.find(which);
            if (it == transfers_.end() || it->second->stopped) break;
            transfer& t = *it->second;
            const auto& events = std::get<socketHandlers>(t.events);  // only sockets open
            std::size_t got = 0;
            const curl_ws_frame* meta = nullptr;
            const CURLcode rc = curl_ws_recv(t.easy.get(), buffer.data(), buffer.size(), &got, &meta);
            if (rc == CURLE_AGAIN) break;
            const bool closing =
                rc == CURLE_OK && meta != nullptr &&
                (static_cast<unsigned>(meta->flags) & static_cast<unsigned>(CURLWS_CLOSE)) != 0;
            if (rc != CURLE_OK || meta == nullptr || closing) {
                t.stopped = true;
                detach(t);
                if (events.closed) events.closed(closing ? "closed by the server" : curl_easy_strerror(rc));
                break;
            }
            const auto flags = static_cast<unsigned>(meta->flags);
            if ((flags & (static_cast<unsigned>(CURLWS_PING) | static_cast<unsigned>(CURLWS_PONG))) != 0)
                continue;  // libcurl answers pings
            // Held up to one byte past the limit, which the bridge refuses.
            if (t.message.size() <= maxDocumentBytes) t.message.append(buffer.data(), got);
            if (meta->bytesleft == 0 && (flags & static_cast<unsigned>(CURLWS_CONT)) == 0) {
                if (events.message)
                    events.message(t.message, (flags & static_cast<unsigned>(CURLWS_BINARY)) != 0);
                t.message.clear();  // keeps its capacity for the next message
            }
        }
    }
}

void curlTransport::wait(std::chrono::milliseconds most) {
    std::vector<curl_waitfd> sockets;
    for (const auto& [which, t] : transfers_) {
        if (!t->open || t->stopped) continue;
        curl_socket_t fd = CURL_SOCKET_BAD;
        get(t->easy.get(), CURLINFO_ACTIVESOCKET, fd);
        if (fd != CURL_SOCKET_BAD) sockets.push_back({.fd = fd, .events = CURL_WAIT_POLLIN, .revents = 0});
    }
    // libcurl takes the extra descriptors as an array (modern-cpp skill §9).
    curl_multi_poll(multi_->handle.get(), sockets.data(), static_cast<unsigned>(sockets.size()),
                    static_cast<int>(most.count()), nullptr);
    int running = 0;
    curl_multi_perform(multi_->handle.get(), &running);
    std::vector<std::pair<id, int>> done;
    int left = 0;
    while (const CURLMsg* m = curl_multi_info_read(multi_->handle.get(), &left)) {
        if (m->msg != CURLMSG_DONE) continue;
        void* user = nullptr;
        get(m->easy_handle, CURLINFO_PRIVATE, user);
        done.emplace_back(static_cast<transfer*>(user)->id, static_cast<int>(m->data.result));
    }
    for (const auto& [which, result] : done) finish(which, result);
    readSockets();
    // Transfers that ended or were stopped are freed now that no callback
    // can be running in them.
    std::erase_if(transfers_,
                  [](const auto& entry) { return entry.second->stopped && !entry.second->inMulti; });
}

webService::webService(primitives::ioHub& io, ioStats& stats, webOptions options)
    : bridge_(io, transport_, stats, std::move(options)), thread_([this](const espresso::stop_token& stop) {
          while (!stop.stop_requested()) bridge_.step(std::chrono::milliseconds(20));
      }) {}

webService::~webService() = default;

}  // namespace server
