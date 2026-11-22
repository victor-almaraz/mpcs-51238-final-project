#include "webBridge.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <espresso/primitives/io.h>

#include "httpText.h"
#include "ioStats.h"
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
    const auto values = frame.used();
    if (request.fields.empty()) {
        if (!values.empty()) pairs.emplace_back("value", values[0]);
    } else {
        for (std::size_t i = 0; i < std::min(values.size(), request.fields.size()); ++i)
            pairs.emplace_back(request.fields[i], values[i]);
    }
    if (request.method == primitives::httpMethod::get) {
        std::string query;
        for (const auto& [key, value] : pairs) {
            query += query.empty() ? "" : "&";
            query += percentEncoded(key) + "=" + percentEncoded(formatNumber(value));
        }
        if (!query.empty()) out.url += (out.url.find('?') == std::string::npos ? "?" : "&") + query;
        return out;
    }
    out.method = request.method;
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
struct webBridge::poll : shared {
    steady::time_point due;
    std::string body;
    bool tooLarge = false;
};

// A held server-sent event stream (io.md §6.1–§6.3).
struct webBridge::stream : shared {
    sseParser parser;
    unsigned failures = 0;
    steady::time_point retryAt;
};

// A held WebSocket (io.md §6.4–§6.6).
struct webBridge::socket : shared {
    bool opened = false;
    unsigned failures = 0;
    steady::time_point retryAt;
};

std::vector<std::string> webBridge::shared::nodes() const {
    std::vector<std::string> out;
    out.reserve(readers.size());
    for (const auto& r : readers) out.push_back(r.owner);
    return out;
}

webBridge::shareKey webBridge::keyOf(portKind kind, const webRequest& r) {
    return {.url = r.url,
            .headers = r.headers,
            .every = kind == portKind::webIn ? millisecondsOf(r.every) : std::chrono::milliseconds(0),
            .hello = kind == portKind::webSocket ? r.hello : std::string()};
}

webBridge::webBridge(primitives::ioHub& io, httpTransport& transport, ioStats& stats, webOptions options)
    : io_(io), transport_(transport), stats_(stats), options_(std::move(options)) {}

// Open transfers stop, so none calls back into a bridge that is gone.
webBridge::~webBridge() {
    const auto stopAll = [this](const auto& entries) {
        for (const auto& [key, e] : entries) {
            if (e->transfer) transport_.stop(*e->transfer);
        }
    };
    stopAll(polls_);
    stopAll(streams_);
    stopAll(sockets_);
}

template <typename Entry, typename Make>
void webBridge::follow(portKind kind, std::map<shareKey, std::unique_ptr<Entry>>& entries, Make&& make) {
    std::map<shareKey, std::vector<std::shared_ptr<const webPort>>> want;
    for (auto& port : io_.webInputs(kind)) {
        if (port->active()) want[keyOf(kind, port->request())].push_back(std::move(port));
    }
    for (auto it = entries.begin(); it != entries.end();) {
        if (want.contains(it->first)) {
            ++it;
            continue;
        }
        if (it->second->transfer) transport_.stop(*it->second->transfer);
        it = entries.erase(it);
    }
    for (auto& [key, ports] : want) {
        auto it = entries.find(key);
        if (it == entries.end()) {
            auto entry = make();
            entry->request = ports.front()->request();
            it = entries.emplace(key, std::move(entry)).first;
        }
        Entry& e = *it->second;
        if (e.ports == ports) continue;  // the same nodes read it: their readers stand
        e.ports = std::move(ports);
        e.readers.clear();
        for (const auto& port : e.ports) {
            const auto& r = port->request();
            reader next{.owner = port->owner(), .event = r.event, .fields = fieldPointers(r.fields)};
            // During a swap a node has two ports, oldest first: the newer
            // one's fields, which its node reads, replace the older's, so the
            // node gets one frame per message.
            const auto same = std::ranges::find(e.readers, port->owner(), &reader::owner);
            if (same != e.readers.end()) {
                *same = std::move(next);
            } else {
                e.readers.push_back(std::move(next));
            }
        }
    }
}

// Each error counts for its nodes; ioStats reports them at most once a
// second per node, so a dead endpoint polled every 0.1 s does not flood
// the console (io.md §10.2).
void webBridge::malformed(const shared& e, std::uint64_t n) {
    if (n > 0) stats_.add(e.nodes(), ioCount::malformed, n);
}

void webBridge::step(std::chrono::milliseconds wait) {
    const auto now = options_.now();
    // The active ports change only with the hub's generation, read before
    // the scan, so a bridge whose ports stay the same does not scan them.
    if (const auto generation = io_.generation(); generation != followed_) {
        followed_ = generation;
        followPorts(now);
    }
    schedulePolls(now);
    followStreams(now);
    followSockets(now);
    sendOutput();
    transport_.wait(wait);
}

bool webBridge::deliver(portKind kind, std::string_view text, const std::vector<reader>& readers,
                        std::optional<std::string_view> event) {
    const document doc = parseDocument(text);
    if (doc.kind != documentKind::read) return false;
    const double time = options_.clock ? options_.clock() : 0.0;
    for (const auto& r : readers) {
        if (event && !r.event.empty() && r.event != *event) continue;
        auto e = fieldsOf(doc.json, r.fields);
        e.frame.time = time;
        io_.deliverWeb(kind, r.owner, e.frame);
        stats_.add(r.owner, ioCount::frames);
        if (e.missing > 0) stats_.add(r.owner, ioCount::missing, e.missing);
    }
    return true;
}

// Each schedule, stream, and socket that a new set of ports asks for is due
// at once.
void webBridge::followPorts(steady::time_point now) {
    follow(portKind::webIn, polls_, [&] {
        auto p = std::make_unique<poll>();
        p->due = now;
        return p;
    });
    follow(portKind::webStream, streams_, [&] {
        auto s = std::make_unique<stream>();
        s->retryAt = now;
        return s;
    });
    follow(portKind::webSocket, sockets_, [&] {
        auto s = std::make_unique<socket>();
        s->retryAt = now;
        return s;
    });
}

// Each poll starts when its plan becomes active, then follows its interval
// from the start of the previous request; one still running when the next
// is due skips that poll and counts it as late (io.md §5.2).
void webBridge::schedulePolls(steady::time_point now) {
    for (auto& [key, p] : polls_) {
        if (now < p->due) continue;
        p->due = now + millisecondsOf(p->request.every);
        if (p->transfer) {
            stats_.add(p->nodes(), ioCount::late);
            continue;
        }
        p->body.clear();
        p->tooLarge = false;
        p->transfer =
            transport_.start({.method = primitives::httpMethod::get,
                              .url = p->request.url,
                              .headers = p->request.headers,
                              .body = {},
                              .timeout = millisecondsOf(p->request.timeout)},
                             {.data =
                                  [this, k = key](long, std::string_view chunk) {
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
                                  [this, k = key](long status, const std::string& error) {
                                      const auto it = polls_.find(k);
                                      if (it == polls_.end()) return;
                                      poll& q = *it->second;
                                      q.transfer.reset();
                                      const std::string body = std::exchange(q.body, {});
                                      std::string why = failure(status, error);
                                      if (why.empty() && q.tooLarge)
                                          why = "the response is larger than 1 MiB";
                                      if (why.empty() && !deliver(portKind::webIn, body, q.readers))
                                          why = "the response is not JSON";
                                      if (!why.empty())
                                          stats_.error(q.nodes(), "web-in " + q.request.url + ": " + why);
                                  }});
    }
}

// Each stream stays open; when it ends or fails it reconnects after the
// server's retry time, or 3 s, doubling while failures repeat, and sends
// the last event ID it saw (io.md §6.3).
void webBridge::followStreams(steady::time_point now) {
    for (auto& [key, s] : streams_) {
        if (s->transfer || now < s->retryAt) continue;
        httpRequest request{.method = primitives::httpMethod::get,
                            .url = s->request.url,
                            .headers = s->request.headers,
                            .body = {},
                            .timeout = {}};
        request.headers.emplace_back("Accept: text/event-stream");
        if (!s->parser.lastEventId().empty())
            request.headers.push_back("Last-Event-ID: " + s->parser.lastEventId());
        s->transfer = transport_.start(
            request, {.data =
                          [this, k = key](long status, std::string_view chunk) {
                              const auto it = streams_.find(k);
                              if (it == streams_.end() || !succeeded(status)) return;
                              stream& t = *it->second;
                              t.failures = 0;
                              t.parser.feed(chunk, [&](const sseMessage& m) {
                                  if (!deliver(portKind::webStream, m.data, t.readers, m.event))
                                      malformed(t, 1);
                              });
                              malformed(t, t.parser.takeTooLarge());
                          },
                      .done =
                          [this, k = key](long status, const std::string& error) {
                              const auto it = streams_.find(k);
                              if (it == streams_.end()) return;
                              stream& t = *it->second;
                              t.transfer.reset();
                              t.parser.reset();
                              ++t.failures;
                              t.retryAt = options_.now() + reconnectDelay(t.failures, t.parser.retry());
                              if (const auto why = failure(status, error); !why.empty())
                                  stats_.error(t.nodes(),
                                               "web-stream " + t.request.url + ": " + why + "; reconnecting");
                          }});
    }
}

// Each socket stays open, sending its hello after each connection opens;
// when it closes or fails it reconnects after 3 s, doubling while failures
// repeat (io.md §6.5, §6.6).
void webBridge::followSockets(steady::time_point now) {
    for (auto& [key, s] : sockets_) {
        if (s->transfer || now < s->retryAt) continue;
        s->opened = false;
        s->transfer = transport_.openSocket(
            s->request.url, s->request.headers,
            {.opened =
                 [this, k = key] {
                     const auto it = sockets_.find(k);
                     if (it == sockets_.end()) return;
                     socket& t = *it->second;
                     t.opened = true;
                     t.failures = 0;
                     if (!t.request.hello.empty() && t.transfer)
                         transport_.sendText(*t.transfer, t.request.hello);
                 },
             .message =
                 [this, k = key](std::string_view message, bool binary) {
                     const auto it = sockets_.find(k);
                     if (it == sockets_.end()) return;
                     // A binary message holds no JSON, so it does not read.
                     if (binary || !deliver(portKind::webSocket, message, it->second->readers))
                         malformed(*it->second, 1);
                 },
             .closed =
                 [this, k = key](const std::string& reason) {
                     const auto it = sockets_.find(k);
                     if (it == sockets_.end()) return;
                     socket& t = *it->second;
                     t.transfer.reset();
                     ++t.failures;
                     t.retryAt = options_.now() + reconnectDelay(t.failures, {});
                     // A connection that never opened failed; one that did
                     // closed, and reconnects without an error.
                     if (!t.opened)
                         stats_.error(t.nodes(),
                                      "web-socket " + t.request.url + ": " + reason + "; reconnecting");
                 }});
    }
}

// Each queued frame sends one request; responses are read and discarded,
// and a failed request is counted, never retried (io.md §7.4).
void webBridge::sendOutput() {
    io_.drainWebOut([&](const webPort& port, const primitives::dataFrame& frame) {
        stats_.add(port.owner(), ioCount::frames);
        transport_.start(outRequest(port.request(), frame),
                         {.data = [](long, std::string_view) {},
                          .done =
                              [this, owner = port.owner(),
                               url = port.request().url](long status, const std::string& error) {
                                  if (const auto why = failure(status, error); !why.empty())
                                      stats_.error(owner, "web-out " + url + ": " + why);
                              }});
    });
}

}  // namespace server
