#include "webBridge.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <espresso/primitives/io.h>

#include "serialLines.h"
#include "webData.h"
#include "webTransport.h"

namespace server {

namespace {

using steady = std::chrono::steady_clock;
using primitives::portKind;
using primitives::webPort;
using primitives::webRequest;

bool succeeded(long status) noexcept { return status >= 200 && status <= 299; }

std::chrono::milliseconds millisecondsOf(double seconds) {
    return std::chrono::milliseconds(std::llround(seconds * 1000.0));
}

std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& l : lines) {
        out += l;
        out += '\n';
    }
    return out;
}

// The schedule a webIn request shares (io.md §5.3): its URL, headers, and
// interval.
std::string pollKey(const webRequest& r) {
    return r.url + '\0' + joined(r.headers) + '\0' + std::to_string(millisecondsOf(r.every).count());
}

// The connection a webStream request shares: its URL and headers.
std::string streamKey(const webRequest& r) { return r.url + '\0' + joined(r.headers); }

// The connection a webSocket request shares: its URL, headers, and hello.
std::string socketKey(const webRequest& r) { return r.url + '\0' + joined(r.headers) + '\0' + r.hello; }

// @p s percent-encoded for a query string: every byte but the unreserved
// ones (RFC 3986).
std::string percentEncoded(std::string_view s) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string out;
    for (const char c : s) {
        const auto b = static_cast<unsigned char>(c);
        if (std::isalnum(b) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            out += '%';
            out += hex[b >> 4U];
            out += hex[b & 0xfU];
        }
    }
    return out;
}

// Why a transfer that ended with @p status and @p error failed; empty
// when it did not.
std::string failure(long status, const std::string& error) {
    if (!error.empty()) return error;
    if (!succeeded(status)) return "HTTP status " + std::to_string(status);
    return {};
}

}  // namespace

httpRequest outRequest(const webRequest& request, const primitives::dataFrame& frame) {
    httpRequest out;
    out.url = request.url;
    out.headers = request.headers;
    out.timeout = millisecondsOf(request.timeout);
    std::vector<std::pair<std::string, float>> pairs;
    const auto count = std::min<std::size_t>(frame.count, frame.values.size());
    if (request.fields.empty()) {
        if (count > 0) pairs.emplace_back("value", frame.values[0]);
    } else {
        for (std::size_t i = 0; i < std::min(count, request.fields.size()); ++i)
            pairs.emplace_back(request.fields[i], frame.values[i]);
    }
    if (request.method == "get") {
        std::string query;
        for (const auto& [key, value] : pairs) {
            query += query.empty() ? "" : "&";
            query += percentEncoded(key) + "=" + percentEncoded(formatNumber(value));
        }
        if (!query.empty()) out.url += (out.url.find('?') == std::string::npos ? "?" : "&") + query;
        return out;
    }
    out.method = request.method == "put" ? "PUT" : "POST";
    out.body = "{";
    for (const auto& [key, value] : pairs) {
        if (out.body.size() > 1) out.body += ',';
        out.body += nlohmann::json(key).dump() + ':' + formatNumber(value);
    }
    out.body += '}';
    out.headers.emplace_back("Content-Type: application/json");
    return out;
}

// A shared poll schedule (io.md §5.2, §5.3).
struct webBridge::poll {
    std::string url;
    std::vector<std::string> headers;
    std::chrono::milliseconds every{0};
    std::chrono::milliseconds timeout{0};
    steady::time_point due;
    std::optional<httpTransport::id> inFlight;
    std::string body;
    bool tooLarge = false;
};

// A held server-sent event stream (io.md §6.1–§6.3).
struct webBridge::stream {
    std::string url;
    std::vector<std::string> headers;
    std::optional<httpTransport::id> transfer;
    sseParser parser;
    std::uint64_t tooLargeBefore = 0;
    unsigned failures = 0;
    steady::time_point retryAt;
};

// A held WebSocket (io.md §6.4–§6.6).
struct webBridge::socket {
    std::string url;
    std::vector<std::string> headers;
    std::string hello;
    std::optional<httpTransport::id> connection;
    bool opened = false;
    unsigned failures = 0;
    steady::time_point retryAt;
};

webBridge::webBridge(primitives::ioHub& io, httpTransport& transport, webOptions options)
    : io_(io), transport_(transport), options_(std::move(options)) {}

// Open transfers stop, so none calls back into a bridge that is gone.
webBridge::~webBridge() {
    for (auto& [key, p] : polls_) {
        if (p->inFlight) transport_.stop(*p->inFlight);
    }
    for (auto& [key, s] : streams_) {
        if (s->transfer) transport_.stop(*s->transfer);
    }
    for (auto& [key, s] : sockets_) {
        if (s->connection) transport_.stop(*s->connection);
    }
}

webCounts webBridge::counts() const noexcept {
    return {.polls = counts_.polls.load(),
            .late = counts_.late.load(),
            .frames = counts_.frames.load(),
            .missing = counts_.missing.load(),
            .malformed = counts_.malformed.load(),
            .skipped = counts_.skipped.load(),
            .sent = counts_.sent.load(),
            .errors = counts_.errors.load()};
}

void webBridge::fail(const std::string& key, const std::string& message) {
    counts_.errors.fetch_add(1);
    if (reported_.insert(key).second && options_.report) options_.report(message);
}

void webBridge::succeed(const std::string& key) { reported_.erase(key); }

void webBridge::step(std::chrono::milliseconds wait) {
    const auto now = options_.now();
    schedulePolls(now);
    followStreams(now);
    followSockets(now);
    sendOutput();
    transport_.wait(wait);
}

bool webBridge::deliver(portKind kind, std::string_view text,
                        const std::function<bool(const webPort&)>& reads) {
    const document doc = parseDocument(text);
    if (doc.kind != documentKind::read) return false;
    for (const auto& port : io_.webInputs(kind)) {
        if (!port->active() || !reads(*port)) continue;
        auto e = fieldsOf(doc.json, port->request().fields);
        e.frame.time = options_.clock ? options_.clock() : 0.0;
        io_.deliverWeb(kind, port->owner(), e.frame);
        counts_.frames.fetch_add(1);
        counts_.missing.fetch_add(e.missing);
    }
    return true;
}

// Each poll starts when its plan becomes active, then follows its interval
// from the start of the previous request; one still running when the next
// is due skips that poll and counts it as late (io.md §5.2).
void webBridge::schedulePolls(steady::time_point now) {
    std::set<std::string> wanted;
    for (const auto& port : io_.webInputs(portKind::webIn)) {
        if (!port->active()) continue;
        const auto& r = port->request();
        const auto key = pollKey(r);
        if (!wanted.insert(key).second || polls_.contains(key)) continue;
        polls_.emplace(key, std::make_unique<poll>(poll{.url = r.url,
                                                        .headers = r.headers,
                                                        .every = millisecondsOf(r.every),
                                                        .timeout = millisecondsOf(r.timeout),
                                                        .due = now,
                                                        .inFlight = std::nullopt,
                                                        .body = {},
                                                        .tooLarge = false}));
    }
    for (auto it = polls_.begin(); it != polls_.end();) {
        if (wanted.contains(it->first)) {
            ++it;
            continue;
        }
        if (it->second->inFlight) transport_.stop(*it->second->inFlight);
        it = polls_.erase(it);
    }
    for (auto& [key, p] : polls_) {
        if (now < p->due) continue;
        p->due = now + p->every;
        if (p->inFlight) {
            counts_.late.fetch_add(1);
            continue;
        }
        p->body.clear();
        p->tooLarge = false;
        counts_.polls.fetch_add(1);
        const std::string k = key;
        p->inFlight = transport_.start(
            {.method = "GET", .url = p->url, .headers = p->headers, .body = {}, .timeout = p->timeout},
            {.data =
                 [this, k](long, std::string_view chunk) {
                     const auto it = polls_.find(k);
                     if (it == polls_.end()) return;
                     poll& q = *it->second;
                     if (q.body.size() + chunk.size() > maxDocumentBytes) {
                         q.tooLarge = true;
                         return;
                     }
                     q.body += chunk;
                 },
             .done =
                 [this, k](long status, const std::string& error) {
                     const auto it = polls_.find(k);
                     if (it == polls_.end()) return;
                     poll& q = *it->second;
                     q.inFlight.reset();
                     const std::string body = std::exchange(q.body, {});
                     std::string why = failure(status, error);
                     if (why.empty() && q.tooLarge) why = "the response is larger than 1 MiB";
                     if (why.empty() && !deliver(portKind::webIn, body, [&](const webPort& port) {
                             return pollKey(port.request()) == k;
                         })) {
                         why = "the response is not JSON";
                     }
                     if (why.empty()) {
                         succeed(k);
                     } else {
                         fail(k, "web-in " + q.url + ": " + why);
                     }
                 }});
    }
}

// Each stream stays open; when it ends or fails it reconnects after the
// server's retry time, or 3 s, doubling while failures repeat, and sends
// the last event ID it saw (io.md §6.3).
void webBridge::followStreams(steady::time_point now) {
    std::set<std::string> wanted;
    for (const auto& port : io_.webInputs(portKind::webStream)) {
        if (!port->active()) continue;
        const auto& r = port->request();
        const auto key = streamKey(r);
        if (!wanted.insert(key).second || streams_.contains(key)) continue;
        auto s = std::make_unique<stream>();
        s->url = r.url;
        s->headers = r.headers;
        s->retryAt = now;
        streams_.emplace(key, std::move(s));
    }
    for (auto it = streams_.begin(); it != streams_.end();) {
        if (wanted.contains(it->first)) {
            ++it;
            continue;
        }
        if (it->second->transfer) transport_.stop(*it->second->transfer);
        it = streams_.erase(it);
    }
    for (auto& [key, s] : streams_) {
        if (s->transfer || now < s->retryAt) continue;
        httpRequest request{.method = "GET", .url = s->url, .headers = s->headers, .body = {}, .timeout = {}};
        request.headers.emplace_back("Accept: text/event-stream");
        if (!s->parser.lastEventId().empty())
            request.headers.push_back("Last-Event-ID: " + s->parser.lastEventId());
        const std::string k = key;
        s->transfer = transport_.start(
            request,
            {.data =
                 [this, k](long status, std::string_view chunk) {
                     const auto it = streams_.find(k);
                     if (it == streams_.end() || !succeeded(status)) return;
                     stream& t = *it->second;
                     t.failures = 0;
                     succeed(k);
                     t.parser.feed(chunk, [&](const sseMessage& m) {
                         const bool json = deliver(portKind::webStream, m.data, [&](const webPort& port) {
                             const auto& event = port.request().event;
                             return streamKey(port.request()) == k && (event.empty() || event == m.event);
                         });
                         if (!json) counts_.malformed.fetch_add(1);
                     });
                     const auto dropped = t.parser.tooLarge();
                     counts_.malformed.fetch_add(dropped - std::exchange(t.tooLargeBefore, dropped));
                 },
             .done =
                 [this, k](long status, const std::string& error) {
                     const auto it = streams_.find(k);
                     if (it == streams_.end()) return;
                     stream& t = *it->second;
                     t.transfer.reset();
                     t.parser.reset();
                     ++t.failures;
                     t.retryAt = options_.now() + reconnectDelay(t.failures, t.parser.retry());
                     if (const auto why = failure(status, error); !why.empty()) {
                         fail(k, "web-stream " + t.url + ": " + why + "; reconnecting");
                     }
                 }});
    }
}

// Each socket stays open, sending its hello after each connection opens;
// when it closes or fails it reconnects after 3 s, doubling while failures
// repeat (io.md §6.5, §6.6).
void webBridge::followSockets(steady::time_point now) {
    std::set<std::string> wanted;
    for (const auto& port : io_.webInputs(portKind::webSocket)) {
        if (!port->active()) continue;
        const auto& r = port->request();
        const auto key = socketKey(r);
        if (!wanted.insert(key).second || sockets_.contains(key)) continue;
        auto s = std::make_unique<socket>();
        s->url = r.url;
        s->headers = r.headers;
        s->hello = r.hello;
        s->retryAt = now;
        sockets_.emplace(key, std::move(s));
    }
    for (auto it = sockets_.begin(); it != sockets_.end();) {
        if (wanted.contains(it->first)) {
            ++it;
            continue;
        }
        if (it->second->connection) transport_.stop(*it->second->connection);
        it = sockets_.erase(it);
    }
    for (auto& [key, s] : sockets_) {
        if (s->connection || now < s->retryAt) continue;
        const std::string k = key;
        s->opened = false;
        s->connection = transport_.openSocket(
            s->url, s->headers,
            {.opened =
                 [this, k] {
                     const auto it = sockets_.find(k);
                     if (it == sockets_.end()) return;
                     socket& t = *it->second;
                     t.opened = true;
                     t.failures = 0;
                     succeed(k);
                     if (!t.hello.empty() && t.connection) transport_.sendText(*t.connection, t.hello);
                 },
             .message =
                 [this, k](std::string_view message, bool binary) {
                     if (binary) {
                         counts_.skipped.fetch_add(1);
                         return;
                     }
                     const bool json = deliver(portKind::webSocket, message, [&](const webPort& port) {
                         return socketKey(port.request()) == k;
                     });
                     if (!json) counts_.malformed.fetch_add(1);
                 },
             .closed =
                 [this, k](const std::string& reason) {
                     const auto it = sockets_.find(k);
                     if (it == sockets_.end()) return;
                     socket& t = *it->second;
                     t.connection.reset();
                     ++t.failures;
                     t.retryAt = options_.now() + reconnectDelay(t.failures, {});
                     // A connection that never opened failed; one that did
                     // closed, and reconnects without an error.
                     if (!t.opened) fail(k, "web-socket " + t.url + ": " + reason + "; reconnecting");
                 }});
    }
}

// Each queued frame sends one request; responses are read and discarded,
// and a failed request is counted, never retried (io.md §7.4).
void webBridge::sendOutput() {
    io_.drainWebOut([&](const webPort& port, const primitives::dataFrame& frame) {
        counts_.sent.fetch_add(1);
        const std::string key = "web-out " + port.owner();
        const std::string url = port.request().url;
        transport_.start(outRequest(port.request(), frame),
                         {.data = [](long, std::string_view) {},
                          .done =
                              [this, key, url](long status, const std::string& error) {
                                  if (const auto why = failure(status, error); !why.empty()) {
                                      fail(key, "web-out " + url + ": " + why);
                                  } else {
                                      succeed(key);
                                  }
                              }});
    });
}

}  // namespace server
