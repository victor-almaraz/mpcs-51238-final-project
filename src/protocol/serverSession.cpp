#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/protocol/messages.h>
#include <espresso/protocol/osc.h>
#include <espresso/protocol/serverSession.h>
#include <espresso/protocol/udp.h>

namespace protocol {

namespace {

constexpr std::size_t rememberedRequests = 64;

message reply(std::string_view addr, std::vector<argument> args) {
    return {.address = std::string(addr), .args = std::move(args)};
}

message done(std::int32_t id) { return reply(address::done, {id}); }

} // namespace

message failure(std::int32_t requestId, errorCode code, std::string text) {
    return reply(address::fail, {requestId, static_cast<std::int32_t>(code), std::move(text)});
}

namespace {

std::vector<message> malformed(const message& m, std::int32_t id) {
    return {failure(id, errorCode::malformedArguments,
                    m.address + ": malformed arguments (" + m.tags() + ")")};
}

} // namespace

// The message set (`protocol.md` §4): every address, whether it carries a
// request ID, and its handler.
std::optional<std::reference_wrapper<const serverSession::route>>
serverSession::findRoute(std::string_view address) {
    static const auto routes = std::to_array<route>({
        {.address = address::hello, .hasRequestId = true, .handle = &serverSession::onHello},
        {.address = address::notify, .hasRequestId = true, .handle = &serverSession::onNotify},
        {.address = address::status, .hasRequestId = true, .handle = &serverSession::onStatus},
        {.address = address::irBegin, .hasRequestId = true, .handle = &serverSession::onIrBegin},
        {.address = address::irChunk, .hasRequestId = false, .handle = &serverSession::onIrChunk},
        {.address = address::irEnd, .hasRequestId = true, .handle = &serverSession::onIrEnd},
        {.address = address::play, .hasRequestId = true, .handle = &serverSession::onPlay},
        {.address = address::stop, .hasRequestId = true, .handle = &serverSession::onStop},
        {.address = address::controlSet, .hasRequestId = false, .handle = &serverSession::onControlSet},
        {.address = address::controlGet, .hasRequestId = true, .handle = &serverSession::onControlGet},
        {.address = address::sync, .hasRequestId = true, .handle = &serverSession::onSync},
        {.address = address::quit, .hasRequestId = true, .handle = &serverSession::onQuit},
    });
    for (const auto& r : routes) {
        if (r.address == address) return std::cref(r);
    }
    return std::nullopt;
}

serverSession::serverSession(serverInfo info, engine& e) : info_(std::move(info)), engine_(e) {}

std::vector<outgoing> serverSession::handleDatagram(std::span<const std::uint8_t> bytes,
                                                    endpoint from) {
    const auto m = decode(bytes);
    if (!m) {
        countDropped();
        return {};
    }
    return handle(*m, from);
}

std::vector<outgoing> serverSession::handle(const message& m, endpoint from) {
    std::vector<outgoing> out;
    auto sendAll = [&](std::vector<message> messages) {
        for (auto& msg : messages) out.push_back({.to = from, .msg = std::move(msg)});
    };

    if (auto it = clients_.find(from); it != clients_.end()) it->second.lastHeard = ++heard_;

    const auto found = findRoute(m.address);
    const auto id = m.int32(0);
    if (!found) {
        sendAll({failure(id.value_or(0), errorCode::unknownAddress, "unknown address " + m.address)});
        return out;
    }
    const route& r = *found;
    if (!r.hasRequestId) {
        if (isConnected(from)) sendAll(std::invoke(r.handle, *this, m, from, 0));  // else nothing to reply to
        return out;
    }
    if (!id) {
        sendAll({failure(0, errorCode::malformedArguments, m.address + ": missing request ID")});
        return out;
    }
    if (m.address != address::hello && !isConnected(from)) {
        sendAll({failure(*id, errorCode::notConnected, "send /hello first")});
        return out;
    }

    // A repeated request ID gets the original reply without repeating
    // the action (§3.3).
    if (auto it = clients_.find(from); it != clients_.end()) {
        for (const auto& [rid, replies] : it->second.recent) {
            if (rid == *id) {
                sendAll(replies);
                return out;
            }
        }
    }

    std::vector<message> replies = std::invoke(r.handle, *this, m, from, *id);
    if (auto it = clients_.find(from); it != clients_.end()) {
        auto& recent = it->second.recent;
        recent.emplace_back(*id, replies);
        while (recent.size() > rememberedRequests) recent.pop_front();
    }
    sendAll(std::move(replies));
    if (m.address == address::quit) engine_.quit();  // after the reply is queued
    return out;
}

std::vector<message> serverSession::onHello(const message& m, endpoint from, std::int32_t id) {
    const auto clientVersion = m.string(1);
    const auto metadataVersion = m.string(2);
    if (!clientVersion || !metadataVersion) return malformed(m, id);
    if (*metadataVersion != info_.metadataVersion) {
        return {failure(id, errorCode::versionMismatch,
                        "metadata version mismatch: client " + std::string(*metadataVersion) +
                            ", server " + info_.metadataVersion)};
    }
    const auto [it, added] = clients_.try_emplace(from);
    it->second.lastHeard = ++heard_;
    if (added && clients_.size() > maxClients) {
        // Forget the client heard from least recently; never the new one.
        const auto oldest = std::ranges::min_element(
            clients_, [](const auto& a, const auto& b) { return a.second.lastHeard < b.second.lastHeard; });
        clients_.erase(oldest);
    }
    return {reply(address::helloOk, {id, info_.serverVersion, info_.sampleRate, info_.blockSize})};
}

std::vector<message> serverSession::onNotify(const message& m, endpoint from, std::int32_t id) {
    const auto on = m.int32(1);
    if (!on) return malformed(m, id);
    clients_[from].notify = *on != 0;
    return {done(id)};
}

serverCounters serverSession::counters() const {
    serverCounters c = engine_.counters();
    const auto total = static_cast<std::uint64_t>(std::max(c.drops, 0)) + droppedDatagrams();
    constexpr auto most = static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    c.drops = static_cast<std::int32_t>(std::min(total, most));
    return c;
}

std::vector<message> serverSession::onStatus(const message&, endpoint, std::int32_t id) {
    const serverCounters c = counters();
    return {reply(address::statusReply,
                  {id, c.load, c.missedDeadlines, c.xruns, c.drops, c.overflows, c.activePlan})};
}

std::vector<message> serverSession::onIrBegin(const message& m, endpoint, std::int32_t id) {
    const auto plan = m.int32(1);
    const auto total = m.int64(2);
    const auto count = m.int32(3);
    const auto crc = m.int32(4);
    if (!plan || !total || !count || !crc) return malformed(m, id);
    if (!ir_.begin(*plan, *total, *count, std::bit_cast<std::uint32_t>(*crc))) {
        return {failure(id, errorCode::irInvalid, "invalid /ir.begin announcement")};
    }
    return {done(id)};
}

std::vector<message> serverSession::onIrChunk(const message& m, endpoint, std::int32_t) {
    const auto plan = m.int32(0);
    const auto index = m.int32(1);
    const auto data = m.bytes(2);
    if (plan && index && data) ir_.add(*plan, *index, *data);
    return {};  // no reply (§4)
}

std::vector<message> serverSession::onIrEnd(const message& m, endpoint, std::int32_t id) {
    const auto planId = m.int32(1);
    if (!planId) return malformed(m, id);
    auto r = ir_.finish(*planId);
    using kind = irAssembler::result::kind;
    switch (r.what) {
    case kind::unknownPlan:
        return {failure(id, errorCode::unknownPlan, "no /ir.begin for plan " + std::to_string(*planId))};
    case kind::missing: {
        message missing = reply(address::irMissing, {id, *planId});
        for (auto index : r.missing) missing.args.emplace_back(index);
        return {missing};
    }
    case kind::incomplete:
        return {failure(id, errorCode::irIncomplete, "assembled IR size differs from /ir.begin")};
    case kind::checksumMismatch:
        return {failure(id, errorCode::irChecksumMismatch, "IR checksum mismatch")};
    case kind::complete:
        if (auto error = engine_.loadPlan(*planId, std::move(r.payload))) {
            return {failure(id, errorCode::irInvalid, *error)};
        }
        return {done(id)};
    }
    return {};
}

std::vector<message> serverSession::onPlay(const message& m, endpoint, std::int32_t id) {
    const auto plan = m.int32(1);
    const auto fade = m.float32(2);
    const auto quant = m.string(3);
    if (!plan || !fade || !quant || !validFade(*fade)) return malformed(m, id);
    if (*quant != "none" && *quant != "beat" && *quant != "bar") return malformed(m, id);
    if (!engine_.hasPlan(*plan)) {
        return {failure(id, errorCode::unknownPlan, "unknown plan " + std::to_string(*plan))};
    }
    engine_.play(*plan, *fade, *quant);
    return {done(id)};
}

std::vector<message> serverSession::onStop(const message& m, endpoint, std::int32_t id) {
    const auto fade = m.float32(1);
    if (!fade || !validFade(*fade)) return malformed(m, id);
    engine_.stop(*fade);
    return {done(id)};
}

std::vector<message> serverSession::onControlSet(const message& m, endpoint, std::int32_t) {
    // index, value pairs; no request ID and no reply
    for (std::size_t i = 0; i + 1 < m.args.size(); i += 2) {
        const auto index = m.int32(i);
        const auto value = m.float32(i + 1);
        if (index && value) engine_.setControl(*index, *value);
    }
    return {};
}

std::vector<message> serverSession::onControlGet(const message& m, endpoint, std::int32_t id) {
    const auto index = m.int32(1);
    if (!index) return malformed(m, id);
    return {reply(address::controlValue, {id, *index, engine_.control(*index)})};
}

std::vector<message> serverSession::onSync(const message&, endpoint, std::int32_t id) {
    return {reply(address::synced, {id})};
}

std::vector<message> serverSession::onQuit(const message&, endpoint, std::int32_t id) {
    return {done(id)};
}

std::vector<outgoing> serverSession::broadcast(const message& m) const {
    std::vector<outgoing> out;
    for (const auto& [client, state] : clients_) {
        if (state.notify) out.push_back({.to = client, .msg = m});
    }
    return out;
}

std::vector<outgoing> serverSession::countersMessages() const {
    const serverCounters c = counters();
    return broadcast(reply(address::counters, {c.missedDeadlines, c.xruns, c.drops, c.overflows, c.load,
                                               c.tempo, c.beat, c.meter}));
}

// --- Network side --------------------------------------------------------------

std::size_t sendAll(const udpSocket& socket, std::span<const outgoing> out, const trafficLog& log) {
    std::size_t failed = 0;
    for (const auto& o : out) {
        if (!socket.send(encode(o.msg), o.to)) ++failed;
        if (log.onMessage) log.onMessage("->", o.to, o.msg);
    }
    return failed;
}

serverLink::serverLink(const udpSocket& socket, serverSession& session, trafficLog log)
    : socket_(socket), session_(session), log_(std::move(log)) {}

void serverLink::send(const std::vector<outgoing>& out) {
    session_.countDropped(sendAll(socket_, out, log_));
}

bool serverLink::serveOnce(int timeoutMs) {
    const auto d = socket_.receive(timeoutMs);
    if (!d) return false;
    const auto m = decode(d->bytes);
    if (!m) {
        session_.countDropped();
        if (log_.onMalformed) log_.onMalformed(d->from);
        return true;
    }
    if (log_.onMessage) log_.onMessage("<-", d->from, *m);
    send(session_.handle(*m, d->from));
    return true;
}

void serverLink::broadcast(const message& m) { send(session_.broadcast(m)); }

void serverLink::sendCountersIfDue(std::chrono::steady_clock::time_point now) {
    if (now < nextCounters_) return;
    send(session_.countersMessages());
    nextCounters_ = now + std::chrono::milliseconds(countersIntervalMs);
}

} // namespace protocol
