#include "pageBridge.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <espresso/protocol/jthread.h>

#include "fileHandle.h"
#include "pageMessages.h"

#include <poll.h>

namespace server {

namespace fs = std::filesystem;

namespace {

// How long the thread waits between rounds when nothing happens.
constexpr int pollMs = 100;

// POSIX sockets take addresses by pointer to the generic type (modern-cpp
// skill §9); this is the only cast.
sockaddr* generic(sockaddr_in& a) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<sockaddr*>(&a);
}

void nonblocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, static_cast<unsigned>(flags) | static_cast<unsigned>(O_NONBLOCK));
}

// A write to a page that has gone must not raise SIGPIPE.
void quietPipe([[maybe_unused]] int fd) noexcept {
#ifdef SO_NOSIGPIPE
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
}

ssize_t sendQuietly(int fd, std::string_view bytes) noexcept {
#ifdef MSG_NOSIGNAL
    return ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
#else
    return ::send(fd, bytes.data(), bytes.size(), 0);
#endif
}

// A listener on 127.0.0.1:@p port, and the port it took; nothing, with
// the reason in @p error, when it cannot be had.
std::optional<std::pair<fileHandle, std::uint16_t>> listenOn(std::uint16_t port, std::string& error) {
    fileHandle fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!fd) {
        error = std::error_code(errno, std::generic_category()).message();
        return std::nullopt;
    }
    int on = 1;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::bind(fd.get(), generic(a), sizeof a) != 0 || ::listen(fd.get(), 16) != 0) {
        error = std::error_code(errno, std::generic_category()).message();
        return std::nullopt;
    }
    socklen_t length = sizeof a;
    ::getsockname(fd.get(), generic(a), &length);
    nonblocking(fd.get());
    return std::pair{std::move(fd), ntohs(a.sin_port)};
}

std::string_view reasonOf(int status) noexcept {
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Error";
    }
}

// A whole response. No CORS headers: no other site may read what the
// listener answers (io.md §12.7).
std::string response(int status, std::string_view type, std::string_view body, std::string_view extra = {}) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reasonOf(status)) + "\r\n";
    if (!type.empty()) out += "Content-Type: " + std::string(type) + "\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    out += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
    out += extra;
    out += "\r\n";
    out += body;
    return out;
}

std::string refusal(int status, std::string_view why) {
    return response(status, "text/plain; charset=utf-8", std::string(why) + "\n");
}

std::string_view typeOf(const fs::path& file) {
    const std::string ext = file.extension().string();
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "text/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".json") return "application/json";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".wasm") return "application/wasm";
    if (ext == ".txt") return "text/plain; charset=utf-8";
    if (ext == ".wav") return "audio/wav";
    return "application/octet-stream";
}

// @p s with each %XX decoded; nothing when an escape is malformed.
std::optional<std::string> percentDecoded(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out += s[i];
            continue;
        }
        if (i + 2 >= s.size()) return std::nullopt;
        const auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = hex(s[i + 1]);
        const int lo = hex(s[i + 2]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out += static_cast<char>((static_cast<unsigned>(hi) << 4U) | static_cast<unsigned>(lo));
        i += 2;
    }
    return out;
}

}  // namespace

// One connection: what it has sent, what it will be sent, and whether it
// is an event stream.
struct pageBridge::connection {
    fileHandle fd;
    requestReader reader;
    std::string out;                 // bytes to write
    bool stream = false;             // GET /osc as an event stream
    bool closing = false;            // close once out is written
    bool dead = false;               // close now
    std::deque<std::string> queued;  // messages for the stream, not yet in out
};

pageBridge::pageBridge(pageOptions options) : options_(std::move(options)) {
    std::array<int, 2> fds{-1, -1};
    if (::pipe(fds.data()) == 0) {
        wakeRead_ = fileHandle(fds[0]);
        wakeWrite_ = fileHandle(fds[1]);
        nonblocking(fds[0]);
        nonblocking(fds[1]);
    }
    thread_ = espresso::jthread([this](const espresso::stop_token& stop) { run(stop); });
}

pageBridge::~pageBridge() {
    thread_.request_stop();
    wake();
}

pageCounts pageBridge::counts() const noexcept {
    return {.received = counts_.received.load(),
            .sent = counts_.sent.load(),
            .dropped = counts_.dropped.load(),
            .refused = counts_.refused.load()};
}

void pageBridge::wake() const noexcept {
    const char byte = 1;
    if (wakeWrite_) [[maybe_unused]]
        const auto n = ::write(wakeWrite_.get(), &byte, 1);
}

void pageBridge::listen(std::optional<std::uint16_t> port) {
    if (port == wanted_ && (!port || *port != 0)) return;
    wanted_ = port;
    fileHandle fresh;
    std::uint16_t bound = 0;
    if (port) {
        std::string error;
        if (auto l = listenOn(*port, error)) {
            fresh = std::move(l->first);
            bound = l->second;
        } else if (options_.report) {
            options_.report("cannot listen for pages on 127.0.0.1:" + std::to_string(*port) + ": " + error);
        }
    }
    {
        const std::scoped_lock lock(mutex_);
        pendingListener_ = std::move(fresh);
        listenerChanged_ = true;
    }
    port_ = bound;
    wake();
}

std::vector<oscBytes> pageBridge::takeMessages() {
    const std::scoped_lock lock(mutex_);
    return std::exchange(inbox_, {});
}

void pageBridge::send(std::span<const std::uint8_t> osc) {
    auto json = messageToPage(osc);
    if (!json) {
        counts_.dropped.fetch_add(1);
        return;
    }
    {
        const std::scoped_lock lock(mutex_);
        if (outbox_.size() >= outboxSize) {
            counts_.dropped.fetch_add(1);
            return;
        }
        outbox_.push_back(std::move(*json));
    }
    wake();
}

bool pageBridge::allowedHost(std::string_view host) const {
    const std::string port = ":" + std::to_string(port_.load());
    return host == "127.0.0.1" + port || host == "localhost" + port;
}

void pageBridge::distribute() {
    std::deque<std::string> messages;
    {
        const std::scoped_lock lock(mutex_);
        messages.swap(outbox_);
    }
    for (const auto& m : messages) {
        for (auto& [fd, c] : connections_) {
            if (!c->stream) continue;
            if (c->queued.size() >= connectionQueue) {
                counts_.dropped.fetch_add(1);  // this page reads too slowly; others are unaffected
                continue;
            }
            c->queued.push_back(m);
            counts_.sent.fetch_add(1);
        }
    }
}

void pageBridge::run(const espresso::stop_token& stop) {
    std::vector<pollfd> polled;
    std::vector<int> fds;
    while (!stop.stop_requested()) {
        {
            const std::scoped_lock lock(mutex_);
            if (listenerChanged_) {
                listener_ = std::move(pendingListener_);
                listenerChanged_ = false;
                connections_.clear();  // pages of the previous port
            }
        }
        distribute();
        polled.clear();
        fds.clear();
        polled.push_back({.fd = wakeRead_.get(), .events = POLLIN, .revents = 0});
        if (listener_) polled.push_back({.fd = listener_.get(), .events = POLLIN, .revents = 0});
        for (auto& [fd, c] : connections_) {
            if (c->stream && c->out.empty()) {
                for (; !c->queued.empty(); c->queued.pop_front())
                    c->out += "data: " + c->queued.front() + "\n\n";
            }
            const short events = c->out.empty() ? short{POLLIN} : static_cast<short>(POLLIN | POLLOUT);
            polled.push_back({.fd = fd, .events = events, .revents = 0});
            fds.push_back(fd);
        }
        // poll() takes its descriptors as an array (modern-cpp skill §9).
        if (::poll(polled.data(), static_cast<nfds_t>(polled.size()), pollMs) <= 0) continue;
        if ((static_cast<unsigned>(polled[0].revents) & static_cast<unsigned>(POLLIN)) != 0U) {
            std::array<char, 64> drain{};
            while (::read(wakeRead_.get(), drain.data(), drain.size()) > 0) {
            }
        }
        std::size_t at = 1;
        if (listener_) {
            if ((static_cast<unsigned>(polled[at].revents) & static_cast<unsigned>(POLLIN)) != 0U) accept();
            ++at;
        }
        for (std::size_t i = 0; i < fds.size(); ++i, ++at) {
            const auto it = connections_.find(fds[i]);
            if (it == connections_.end()) continue;
            connection& c = *it->second;
            const auto revents = static_cast<unsigned>(polled[at].revents);
            if ((revents & static_cast<unsigned>(POLLIN | POLLHUP | POLLERR)) != 0U) readFrom(c);
            if (!c.dead && !c.out.empty() && (revents & static_cast<unsigned>(POLLOUT)) != 0U) {
                // send() takes the bytes by pointer (modern-cpp skill §9).
                const auto n = sendQuietly(c.fd.get(), c.out);
                if (n > 0) {
                    c.out.erase(0, static_cast<std::size_t>(n));
                } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    c.dead = true;
                }
            }
            if (c.closing && c.out.empty()) c.dead = true;
        }
        std::erase_if(connections_, [](const auto& entry) { return entry.second->dead; });
    }
}

void pageBridge::accept() {
    while (true) {
        fileHandle fd(::accept(listener_.get(), nullptr, nullptr));
        if (!fd) return;
        nonblocking(fd.get());
        quietPipe(fd.get());
        if (connections_.size() >= maxConnections) {
            counts_.refused.fetch_add(1);
            [[maybe_unused]] const auto n =
                sendQuietly(fd.get(), refusal(503, "too many connections to this page port"));
            continue;  // closed as fd goes
        }
        auto c = std::make_unique<connection>();
        const int key = fd.get();
        c->fd = std::move(fd);
        connections_.emplace(key, std::move(c));
    }
}

void pageBridge::readFrom(connection& c) {
    std::array<char, 16384> buffer{};
    for (int round = 0; round < 8; ++round) {
        // read() fills a buffer by pointer (modern-cpp skill §9).
        const auto n = ::read(c.fd.get(), buffer.data(), buffer.size());
        if (n == 0) {
            c.dead = true;
            return;
        }
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) c.dead = true;
            break;
        }
        if (c.stream) continue;  // an event stream sends nothing more that matters
        c.reader.feed(std::string_view(buffer.data(), static_cast<std::size_t>(n)));
    }
    while (!c.closing && !c.stream) {
        auto request = c.reader.next();
        if (!request) break;
        respond(c, *request);
    }
    const auto error = c.reader.error();
    if (error == requestError::none || c.closing) return;
    counts_.refused.fetch_add(1);
    c.closing = true;
    switch (error) {
    case requestError::lengthRequired: c.out += refusal(411, "a body needs Content-Length"); break;
    case requestError::tooLarge: c.out += refusal(413, "the body is larger than 64 KiB"); break;
    case requestError::headersTooLarge:
        c.out += refusal(431, "the request head is larger than 16 KiB");
        break;
    default: c.out += refusal(400, "not an HTTP/1.1 request"); break;
    }
}

void pageBridge::respond(connection& c, const httpMessage& request) {
    if (request.header("connection") == "close") c.closing = true;
    const std::string host = request.header("host");
    if (!allowedHost(host)) {
        counts_.refused.fetch_add(1);
        c.out += refusal(403, "this page port answers only 127.0.0.1 and localhost");
        return;
    }
    const std::string path = request.path();
    if (path == "/osc") {
        if (request.method == "POST") {
            post(c, request);
        } else if (request.method != "GET") {
            counts_.refused.fetch_add(1);
            c.out += refusal(405, "/osc takes GET and POST");
        } else if (request.header("upgrade") == "websocket") {
            counts_.refused.fetch_add(1);
            c.out += refusal(501, "WebSocket is not served yet");
        } else {
            // The messages sent to pages, as server-sent events (io.md §12.4).
            c.out += "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n\r\n";
            c.stream = true;
        }
        return;
    }
    if (request.method != "GET") {
        counts_.refused.fetch_add(1);
        c.out += refusal(405, "pages are read with GET");
        return;
    }
    if (path == "/espresso.js") {
        if (options_.helper.empty()) {
            counts_.refused.fetch_add(1);
            c.out += refusal(404, "no such file");
        } else {
            c.out += response(200, "text/javascript; charset=utf-8", options_.helper);
        }
        return;
    }
    serveFile(c, path);
}

// A regular file whose real path lies under ui/; anything else, symbolic
// links that leave ui/ included, is 404 (io.md §12.7).
void pageBridge::serveFile(connection& c, const std::string& path) {
    const auto missing = [&] {
        counts_.refused.fetch_add(1);
        c.out += refusal(404, "no such file");
    };
    const auto decoded = percentDecoded(path);
    if (!decoded || decoded->find('\0') != std::string::npos || decoded->find('\\') != std::string::npos) {
        missing();
        return;
    }
    std::error_code ec;
    const fs::path ui = fs::canonical(options_.root / "ui", ec);
    if (ec) {
        missing();
        return;
    }
    const fs::path relative = *decoded == "/" ? fs::path("index.html") : fs::path(decoded->substr(1));
    const fs::path file = fs::canonical(ui / relative, ec);
    if (ec || !fs::is_regular_file(file, ec)) {
        missing();
        return;
    }
    // Under ui/, component by component, after symbolic links resolve.
    if (std::ranges::mismatch(ui, file).in1 != ui.end()) {
        missing();
        return;
    }
    std::ifstream in(file, std::ios::binary);
    std::ostringstream body;
    body << in.rdbuf();
    if (in.bad()) {
        missing();
        return;
    }
    c.out += response(200, typeOf(file), body.str());
}

// Messages from a page, delivered in order; a batch with a message that
// breaks a rule is refused whole (io.md §12.5). Only a page of this
// listener's origin may post (§12.7).
void pageBridge::post(connection& c, const httpMessage& request) {
    if (request.header("origin") != "http://" + request.header("host")) {
        counts_.refused.fetch_add(1);
        c.out += refusal(403, "messages are taken only from this page port's own pages");
        return;
    }
    std::string error;
    auto messages = messagesFromPage(request.body, error);
    if (!messages) {
        counts_.refused.fetch_add(1);
        c.out += refusal(400, error);
        return;
    }
    {
        const std::scoped_lock lock(mutex_);
        for (auto& m : *messages) {
            if (inbox_.size() >= inboxSize) {
                counts_.dropped.fetch_add(1);
                continue;
            }
            inbox_.push_back(std::move(m));
            counts_.received.fetch_add(1);
        }
    }
    c.out += response(204, {}, {});
}

}  // namespace server
