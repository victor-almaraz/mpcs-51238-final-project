#include "pageBridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <espresso/metadata/pageSpec.h>
#include <espresso/page/render.h>
#include <espresso/protocol/jthread.h>
#include <espresso/protocol/osc.h>
#include <espresso/protocol/udp.h>

#include "clips.h"
#include "fileHandle.h"
#include "httpText.h"
#include "pageMessages.h"
#include "pagePairing.h"
#include "pagePlan.h"
#include "webSocket.h"

#include <poll.h>

namespace server {

namespace fs = std::filesystem;

namespace {

// How long the thread waits between rounds when nothing happens.
constexpr int pollMs = 100;

// The text form of the peer address of @p fd, or empty.
std::string peerOf(int fd) {
    sockaddr_in a{};
    socklen_t length = sizeof a;
    if (::getpeername(fd, protocol::asSockaddr(a), &length) != 0 || a.sin_family != AF_INET) return {};
    std::array<char, INET_ADDRSTRLEN> text{};
    if (::inet_ntop(AF_INET, &a.sin_addr, text.data(), text.size()) == nullptr) return {};
    return text.data();
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

// A listener on 127.0.0.1:@p port, or on every interface when @p lan, and
// the port it took; nothing, with the reason in @p error, when it cannot be
// had. Only this listener binds beyond loopback (io.md §12.8).
std::optional<std::pair<fileHandle, std::uint16_t>> listenOn(std::uint16_t port, bool lan,
                                                             std::string& error) {
    fileHandle fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!fd) {
        error = errnoMessage(errno);
        return std::nullopt;
    }
    int on = 1;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in a = protocol::loopback(port);
    if (lan) a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd.get(), protocol::asSockaddr(a), sizeof a) != 0 || ::listen(fd.get(), 16) != 0) {
        error = errnoMessage(errno);
        return std::nullopt;
    }
    socklen_t length = sizeof a;
    ::getsockname(fd.get(), protocol::asSockaddr(a), &length);
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

// A response's status line and headers, for a body of @p length bytes. No
// CORS headers: no other site may read what the listener answers (io.md
// §12.7).
std::string responseHead(int status, std::string_view type, std::size_t length, std::string_view extra = {}) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reasonOf(status)) + "\r\n";
    if (!type.empty()) out += "Content-Type: " + std::string(type) + "\r\n";
    out += "Content-Length: " + std::to_string(length) + "\r\n";
    out += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
    out += extra;
    out += "\r\n";
    return out;
}

// A whole response.
std::string response(int status, std::string_view type, std::string_view body, std::string_view extra = {}) {
    std::string out = responseHead(status, type, body.size(), extra);
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

// Whether a Connection header asks to close: its options are tokens in a
// list, compared without case (RFC 9110 §7.6.1).
bool asksToClose(std::string_view header) {
    const std::string list = lowered(header);
    std::string_view rest = list;
    while (true) {
        const auto comma = rest.find(',');
        std::string_view token = rest.substr(0, comma);
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.remove_prefix(1);
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.remove_suffix(1);
        if (token == "close") return true;
        if (comma == std::string_view::npos) return false;
        rest.remove_prefix(comma + 1);
    }
}

// A page of this listener's origin: only one may post or open a socket
// (io.md §12.7).
bool sameOrigin(const httpMessage& request) {
    return request.header("origin") == "http://" + std::string(request.header("host"));
}

}  // namespace

// One connection: what it has sent, what it will be sent, and whether it
// is an event stream.
struct pageBridge::connection {
    enum class mode : std::uint8_t { http, stream, socket };

    fileHandle fd;
    std::string peer;  // the peer's address
    requestReader reader;
    mode kind = mode::http;
    bool binary = false;                // a socket that takes binary frames (?format=binary)
    std::optional<frameReader> frames;  // a socket's frames from the page
    std::chrono::steady_clock::time_point nextPing;
    int unanswered = 0;                                  // pings without a pong
    std::string out;                                     // bytes to write
    bool closing = false;                                // close once out is written
    bool draining = false;                               // close once a refused body has arrived
    std::chrono::steady_clock::time_point drainUntil;    // or once it stops arriving
    bool dead = false;                                   // close now
    std::vector<std::shared_ptr<const outgoing>> queued;  // messages for the page, not yet in out

    bool listens() const noexcept { return kind != mode::http; }
};

pageBridge::pageBridge(pageOptions options)
    : options_(std::move(options)), lanNames_(options_.lan ? lanNames() : std::vector<std::string>()) {
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

void pageBridge::refuse(connection& c, int status, std::string_view why) {
    counts_.refused.fetch_add(1);
    c.out += refusal(status, why);
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
        if (auto l = listenOn(*port, options_.lan, error)) {
            fresh = std::move(l->first);
            bound = l->second;
        } else if (options_.report) {
            options_.report("cannot listen for pages on " +
                            std::string(options_.lan ? "0.0.0.0" : "127.0.0.1") + ":" +
                            std::to_string(*port) + ": " + error);
        }
    }
    {
        const std::scoped_lock lock(mutex_);
        pendingListener_ = std::move(fresh);
    }
    port_ = bound;
    if (bound != 0 && options_.lan && options_.announce) {
        std::vector<std::string> links;
        for (const auto& address : lanAddresses())
            links.push_back("http://" + address + ":" + std::to_string(bound) + "/?pair=" + options_.token);
        options_.announce(links);
    }
    wake();
}

std::vector<oscBytes> pageBridge::takeMessages() {
    const std::scoped_lock lock(mutex_);
    return std::exchange(inbox_, {});
}

void pageBridge::follow(std::shared_ptr<const pagePlan> names) {
    const std::scoped_lock lock(mutex_);
    if (names == plan_) return;
    plan_ = std::move(names);
    std::erase_if(retained_,
                  [&](const auto& entry) { return !plan_ || !plan_->addresses.contains(entry.first); });
}

void pageBridge::send(std::span<const std::uint8_t> osc) {
    // The last message to each of the active plan's addresses is kept for
    // the pages that connect later, so a page opened after a value was sent
    // shows it.
    std::string address;
    for (const auto b : osc) {
        if (b == 0) break;
        address += static_cast<char>(b);
    }
    bool retains = false;
    {
        const std::scoped_lock lock(mutex_);
        retains = plan_ && plan_->addresses.contains(address);
    }
    if (!retains && listening_.load(std::memory_order_relaxed) == 0) return;  // no page would read it
    auto json = messageToPage(osc);
    if (!json) {
        counts_.dropped.fetch_add(1);
        return;
    }
    auto message =
        std::make_shared<const outgoing>(outgoing{.json = std::move(*json), .osc = {osc.begin(), osc.end()}});
    {
        const std::scoped_lock lock(mutex_);
        // Retained messages are queued even with no page listening: one
        // whose stream opened since listening_ was counted has had its
        // replay, and reads this from the queue.
        if (retains) retained_.insert_or_assign(std::move(address), message);
        if (outbox_.size() >= outboxSize) {
            counts_.dropped.fetch_add(1);
            return;
        }
        outbox_.push_back(std::move(message));
    }
    wake();
}

// The last message to each address, for a page whose stream or socket has
// just opened: written at once, all of them, ahead of anything queued.
void pageBridge::replay(connection& c) {
    std::vector<std::shared_ptr<const outgoing>> messages;
    {
        const std::scoped_lock lock(mutex_);
        messages.reserve(retained_.size());
        for (const auto& [address, message] : retained_) messages.push_back(message);
    }
    write(c, messages);
}

void pageBridge::serve(std::shared_ptr<const std::string> page) {
    bool changed = false;
    {
        const std::scoped_lock lock(mutex_);
        if (page == generated_) return;
        // A page served before, now different: a new layout after an Update,
        // or after a stop and a run of another layout, which leaves the
        // last page served (shown_) in place.
        changed = shown_ && page && *shown_ != *page;
        if (page) shown_ = page;
        generated_ = std::move(page);
    }
    // A project's own ui/index.html does not change with the layout.
    if (changed && !ownPage())
        send(protocol::encode({.address = std::string(page::reloadAddress), .args = {}}));
}

// Whether the project has its own page: a ui/index.html that is a file and
// not empty, so the empty file a failed `--page FILE > ui/index.html`
// leaves does not hide the drawn page.
bool pageBridge::ownPage() const {
    std::error_code ec;
    const fs::path index = options_.root / "ui" / "index.html";
    return fs::is_regular_file(index, ec) && fs::file_size(index, ec) > 0 && !ec;
}

bool pageBridge::allowedHost(std::string_view host) const {
    return hostAllowed(host, port_.load(), lanNames_);
}

void pageBridge::distribute() {
    std::vector<std::shared_ptr<const outgoing>> messages;
    {
        const std::scoped_lock lock(mutex_);
        messages.swap(outbox_);
    }
    for (const auto& m : messages) {
        for (auto& [fd, c] : connections_) {
            if (!c->listens()) continue;
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
            if (pendingListener_) {
                listener_ = std::move(*pendingListener_);
                pendingListener_.reset();
                connections_.clear();  // pages of the previous port
            }
        }
        listening_.store(static_cast<std::size_t>(std::ranges::count_if(
                             connections_, [](const auto& entry) { return entry.second->listens(); })),
                         std::memory_order_relaxed);
        distribute();
        polled.clear();
        fds.clear();
        polled.push_back({.fd = wakeRead_.get(), .events = POLLIN, .revents = 0});
        if (listener_) polled.push_back({.fd = listener_.get(), .events = POLLIN, .revents = 0});
        const auto now = std::chrono::steady_clock::now();
        for (auto& [fd, c] : connections_) {
            if (c->kind == connection::mode::socket) ping(*c, now);
            if (c->draining && now >= c->drainUntil) {
                c->closing = true;
                if (c->out.empty()) c->dead = true;
            }
            if (c->out.empty()) render(*c);
            const short events = c->out.empty() ? short{POLLIN} : static_cast<short>(POLLIN | POLLOUT);
            polled.push_back({.fd = fd, .events = events, .revents = 0});
            fds.push_back(fd);
        }
        // poll() takes its descriptors as an array (modern-cpp skill §9).
        const int wait = static_cast<int>(std::min<std::int64_t>(pollMs, options_.pingInterval.count()));
        if (::poll(polled.data(), static_cast<nfds_t>(polled.size()), wait) <= 0) {
            std::erase_if(connections_, [](const auto& entry) { return entry.second->dead; });
            continue;
        }
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
        c->peer = peerOf(fd.get());
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
        const std::string_view bytes(buffer.data(), static_cast<std::size_t>(n));
        if (c.draining) c.drainUntil = std::chrono::steady_clock::now() + drainIdle;
        if (c.kind == connection::mode::socket) {
            frames(c, bytes);
        } else if (c.kind == connection::mode::http) {
            c.reader.feed(bytes);
        }  // an event stream sends nothing more that matters
    }
    while (!c.closing && c.kind == connection::mode::http) {
        auto request = c.reader.next();
        if (!request) break;
        respond(c, *request);
        // Bytes after an upgrade are the socket's first frames.
        if (c.kind == connection::mode::socket) frames(c, c.reader.takeRest());
    }
    if (c.kind != connection::mode::http) return;
    if (c.draining) {
        // Closed once its sender has sent it all, so it reads the refusal.
        if (c.reader.skipping() == 0) c.closing = true;
        return;
    }
    const auto error = c.reader.error();
    if (error == requestError::none || c.closing) return;
    c.closing = true;
    switch (error) {
    case requestError::lengthRequired: refuse(c, 411, "a body needs Content-Length"); break;
    case requestError::tooLarge:
        refuse(c, 413, "the body is larger than 64 KiB, or 1 MiB for a clip");
        if (c.reader.skipping() > 0 && c.reader.skipping() <= maxDrainedBody) {
            c.closing = false;
            c.draining = true;
            c.drainUntil = std::chrono::steady_clock::now() + drainIdle;
        }
        break;
    case requestError::headersTooLarge: refuse(c, 431, "the request head is larger than 16 KiB"); break;
    default: refuse(c, 400, "not an HTTP/1.1 request"); break;
    }
}

void pageBridge::respond(connection& c, const httpMessage& request) {
    if (asksToClose(request.header("connection"))) c.closing = true;
    if (!allowedHost(request.header("host"))) {
        refuse(c, 403,
               options_.lan ? "this page port answers only the machine's own names"
                            : "this page port answers only 127.0.0.1 and localhost");
        return;
    }
    // On the network, a peer other than loopback needs the pairing cookie;
    // the pairing link sets it and takes the token out of the address bar
    // (io.md §12.8).
    if (options_.lan) {
        switch (pairingFor(c.peer, request.header("cookie"), request.query("pair"), options_.token)) {
        case pairing::refused: refuse(c, 401, "open the pairing link the server printed first"); return;
        case pairing::pair:
            c.out += response(302, {}, {},
                              "Location: " + std::string(request.path()) +
                                  "\r\nSet-Cookie: " + std::string(pairCookie) + "=" + options_.token +
                                  "; HttpOnly; SameSite=Strict; Path=/\r\n");
            return;
        case pairing::allowed: break;
        }
    }
    const std::string path(request.path());
    if (path == "/osc") {
        if (request.method == "POST") {
            post(c, request);
        } else if (request.method != "GET") {
            refuse(c, 405, "/osc takes GET and POST");
        } else if (lowered(request.header("upgrade")) == "websocket") {
            upgrade(c, request);
        } else {
            // The messages sent to pages, as server-sent events (io.md §12.4).
            c.out += "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n\r\n";
            c.kind = connection::mode::stream;
            replay(c);
        }
        return;
    }
    if (path.starts_with("/clips/")) {
        clips(c, request);
        return;
    }
    if (request.method != "GET") {
        refuse(c, 405, "pages are read with GET");
        return;
    }
    // The helper and the control library, served from the server (io.md
    // §12.9, ui.md §4); a page's own files come from ui/.
    for (const auto& [served, body] : {std::pair{"/espresso.js", std::cref(options_.helper)},
                                       std::pair{"/espresso-ui.js", std::cref(options_.controls)}}) {
        if (path != served) continue;
        if (body.get().empty()) {
            refuse(c, 404, "no such file");
        } else {
            c.out += response(200, "text/javascript; charset=utf-8", body.get());
        }
        return;
    }
    serveFile(c, path);
}

// A regular file whose real path lies under ui/; anything else, symbolic
// links that leave ui/ included, is 404 (io.md §12.7).
void pageBridge::serveFile(connection& c, const std::string& path) {
    const auto missing = [&] { refuse(c, 404, "no such file"); };
    const auto decoded = percentDecoded(path);
    if (!decoded || decoded->find('\0') != std::string::npos || decoded->find('\\') != std::string::npos) {
        missing();
        return;
    }
    std::error_code ec;
    const fs::path ui = fs::canonical(options_.root / "ui", ec);
    // Whether ui/ holds @p name, a regular file.
    const auto holds = [&](std::string_view name) {
        std::error_code missingFile;
        return !ec && fs::is_regular_file(ui / name, missingFile);
    };
    const bool own = !ec && ownPage();
    std::shared_ptr<const std::string> page;
    if (!own) {
        const std::scoped_lock lock(mutex_);
        page = generated_;
    }
    // Without ui/index.html, / is the page drawn from the layout, if any.
    // The style and script a drawn page loads after its own, there or
    // written to ui/index.html, are empty until the project writes them
    // (dsl-ui.md §8).
    if ((*decoded == "/" || *decoded == "/index.html") && !own) {
        if (page) {
            c.out += response(200, "text/html; charset=utf-8", *page);
        } else {
            missing();
        }
        return;
    }
    for (const auto& [name, type] : {std::pair{"/style.css", "text/css; charset=utf-8"},
                                     std::pair{"/script.js", "text/javascript; charset=utf-8"}}) {
        if (*decoded == name && !holds(std::string_view(name).substr(1))) {
            c.out += response(200, type, "");
            return;
        }
    }
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
    const auto size = static_cast<std::size_t>(fs::file_size(file, ec));
    if (!in || ec) {
        missing();
        return;
    }
    // The file goes straight into the output, after its head.
    const std::size_t start = c.out.size();
    c.out += responseHead(200, typeOf(file), size);
    const std::size_t body = c.out.size();
    c.out.resize(body + size);
    // istream::read fills its bytes by pointer (modern-cpp skill §9).
    in.read(std::span(c.out).subspan(body).data(), static_cast<std::streamsize>(size));
    if (in.gcount() != static_cast<std::streamsize>(size)) {
        c.out.resize(start);
        missing();
    }
}

// Messages from a page, delivered in order; a batch with a message that
// breaks a rule is refused whole (io.md §12.5). Only a page of this
// listener's origin may post (§12.7).
void pageBridge::post(connection& c, const httpMessage& request) {
    if (!sameOrigin(request)) {
        refuse(c, 403, "messages are taken only from this page port's own pages");
        return;
    }
    std::string error;
    auto messages = messagesFromPage(request.body, error);
    if (!messages) {
        refuse(c, 400, error);
        return;
    }
    deliver(std::move(*messages));
    c.out += response(204, {}, {});
}

// A bank's clips (canvas.md §5.3): GET /clips/<bank> lists the titles
// of its slots, from 1, as a JSON object; POST /clips/<bank>/<slot>?name=<title>
// saves a MIDI file, the body, as the slot's, from this port's own pages.
void pageBridge::clips(connection& c, const httpMessage& request) {
    const std::string_view rest = request.path().substr(std::string_view("/clips/").size());
    const auto slash = rest.find('/');
    const std::string bank(rest.substr(0, slash));
    bool plays = false;
    {
        const std::scoped_lock lock(mutex_);
        plays = plan_ && plan_->banks.contains(bank);
    }
    // Only the banks the piece playing plays: an upload makes no folder
    // for a name nothing plays.
    if (!isClipBankName(bank) || !plays) {
        refuse(c, 404, "the piece playing has no clip bank " + bank);
        return;
    }
    if (slash == std::string_view::npos) {
        if (request.method != "GET") {
            refuse(c, 405, "a bank's clips are listed with GET");
            return;
        }
        nlohmann::json titles = nlohmann::json::object();
        for (const auto& [slot, file] : clipFilesOf(options_.root, bank))
            titles[std::to_string(slot + 1)] = clipTitle(file);
        c.out += response(200, "application/json", titles.dump());
        return;
    }
    if (request.method != "POST") {
        refuse(c, 405, "a clip is saved with POST");
        return;
    }
    if (!sameOrigin(request)) {
        refuse(c, 403, "clips are taken only from this page port's own pages");
        return;
    }
    const std::string_view number = rest.substr(slash + 1);
    const bool digits = !number.empty() && number.size() <= 2 &&
                        std::ranges::all_of(number, [](char ch) { return ch >= '0' && ch <= '9'; });
    const int slot = digits ? std::stoi(std::string(number)) - 1 : -1;
    const auto title = percentDecoded(request.query("name").value_or("")).value_or("");
    const std::vector<std::uint8_t> bytes(request.body.begin(), request.body.end());
    if (const auto error = saveClip(options_.root, bank, slot, title, bytes)) {
        refuse(c, 400, *error);
        return;
    }
    clipsChanged_ = true;
    c.out += response(204, {}, {});
    // The other open pages list the bank again.
    send(protocol::encode({.address = std::string(metadata::pageClipsAddress), .args = {bank}}));
}

// A WebSocket on /osc (io.md §12.6): only from this port's own pages, as
// posts are (§12.7), and without extensions.
void pageBridge::upgrade(connection& c, const httpMessage& request) {
    if (!sameOrigin(request)) {
        refuse(c, 403, "sockets are opened only from this page port's own pages");
        return;
    }
    const std::string_view key = request.header("sec-websocket-key");
    if (request.header("sec-websocket-version") != "13" || key.empty()) {
        refuse(c, 400, "a WebSocket needs Sec-WebSocket-Version 13 and a Sec-WebSocket-Key");
        c.closing = true;
        return;
    }
    c.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
             "Upgrade\r\nSec-WebSocket-Accept: " +
             acceptKey(key) + "\r\n\r\n";
    c.kind = connection::mode::socket;
    c.binary = request.query("format") == "binary";
    c.frames.emplace(true, maxPageBody);
    c.nextPing = std::chrono::steady_clock::now() + options_.pingInterval;
    replay(c);
}

// A socket's frames from its page: a text frame is a batch of messages and
// a binary frame one encoded message; one that breaks a rule is dropped
// and counted, and the socket stays open (io.md §12.6).
void pageBridge::frames(connection& c, std::string_view bytes) {
    if (bytes.empty() || c.closing || !c.frames) return;
    const auto error = c.frames->feed(bytes, [&](const frameMessage& m) {
        switch (m.op) {
        case opcode::text: {
            std::string why;
            if (auto batch = messagesFromPage(m.payload, why)) {
                deliver(std::move(*batch));
            } else {
                counts_.dropped.fetch_add(1);
            }
            break;
        }
        case opcode::binary: {
            const std::vector<std::uint8_t> raw(m.payload.begin(), m.payload.end());
            if (auto osc = binaryFromPage(raw)) {
                deliver({std::move(*osc)});
            } else {
                counts_.dropped.fetch_add(1);
            }
            break;
        }
        case opcode::ping: c.out += encodeFrame(opcode::pong, m.payload); break;
        case opcode::pong: c.unanswered = 0; break;
        case opcode::close:
            c.out += encodeFrame(opcode::close, m.payload.substr(0, 2));
            c.closing = true;
            break;
        default: break;
        }
    });
    if (error == frameError::none) return;
    // An unmasked frame or a protocol error is 1002; a message over 64 KiB
    // is 1009 (io.md §12.6).
    const std::uint16_t status = error == frameError::tooLarge ? 1009 : 1002;
    const std::string payload{static_cast<char>(status >> 8U), static_cast<char>(status & 0xFFU)};
    c.out += encodeFrame(opcode::close, payload);
    c.closing = true;
}

void pageBridge::deliver(std::vector<oscBytes> messages) {
    const std::scoped_lock lock(mutex_);
    for (auto& m : messages) {
        if (inbox_.size() >= inboxSize) {
            counts_.dropped.fetch_add(1);
            continue;
        }
        inbox_.push_back(std::move(m));
        counts_.received.fetch_add(1);
    }
}

// The server pings each socket every interval and closes one that leaves
// two pings unanswered (io.md §12.6).
void pageBridge::ping(connection& c, std::chrono::steady_clock::time_point now) {
    if (c.closing || now < c.nextPing) return;
    if (c.unanswered >= 2) {
        c.dead = true;
        return;
    }
    c.out += encodeFrame(opcode::ping, {});
    ++c.unanswered;
    c.nextPing = now + options_.pingInterval;
}

// Moves the messages queued for a page into its output: an event per
// message for a stream; for a socket, one text frame holding them as an
// array, or a binary frame per message.
void pageBridge::render(connection& c) {
    if (c.queued.empty() || c.closing) return;
    write(c, c.queued);
    c.queued.clear();
}

void pageBridge::write(connection& c, std::span<const std::shared_ptr<const outgoing>> messages) {
    if (messages.empty()) return;
    if (c.kind == connection::mode::stream) {
        for (const auto& m : messages) c.out += "data: " + m->json + "\n\n";
    } else if (c.binary) {
        for (const auto& m : messages) {
            c.out += encodeFrame(opcode::binary, std::string(m->osc.begin(), m->osc.end()));
        }
    } else {
        std::string batch = "[";
        for (const auto& m : messages) {
            if (batch.size() > 1) batch += ',';
            batch += m->json;
        }
        batch += ']';
        c.out += encodeFrame(opcode::text, batch);
    }
}

}  // namespace server
