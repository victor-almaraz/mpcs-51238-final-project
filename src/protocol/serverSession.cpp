#include <proj/protocol/serverSession.h>

#include <bit>

#include <proj/protocol/messages.h>

namespace protocol {

namespace {

constexpr std::size_t rememberedRequests = 64;

message reply(std::string_view addr, std::vector<argument> args) {
    return {std::string(addr), std::move(args)};
}

message done(std::int32_t id) { return reply(address::done, {id}); }

bool knownAddress(std::string_view a) {
    for (auto known : {address::hello, address::notify, address::status, address::irBegin,
                       address::irChunk, address::irEnd, address::play, address::stop,
                       address::controlSet, address::controlGet, address::sync, address::quit}) {
        if (a == known) return true;
    }
    return false;
}

// Messages without a request ID (`protocol.md` §4).
bool hasNoRequestId(std::string_view a) {
    return a == address::irChunk || a == address::controlSet;
}

} // namespace

message failure(std::int32_t requestId, errorCode code, std::string text) {
    return reply(address::fail, {requestId, static_cast<std::int32_t>(code), std::move(text)});
}

serverSession::serverSession(serverInfo info, engine& e) : info_(std::move(info)), engine_(e) {}

std::vector<outgoing> serverSession::handleDatagram(std::span<const std::uint8_t> bytes,
                                                    endpoint from) {
    const auto m = decode(bytes);
    if (!m) {
        ++dropped_;
        return {};
    }
    return handle(*m, from);
}

std::vector<outgoing> serverSession::handle(const message& m, endpoint from) {
    std::vector<outgoing> out;
    auto sendAll = [&](std::vector<message> messages) {
        for (auto& msg : messages) out.push_back({from, std::move(msg)});
    };

    if (hasNoRequestId(m.address)) {
        if (!isConnected(from)) return out;  // nothing to reply to
        sendAll(dispatch(m, from, 0));
        return out;
    }
    const auto id = m.int32(0);
    if (!knownAddress(m.address)) {
        sendAll({failure(id.value_or(0), errorCode::unknownAddress, "unknown address " + m.address)});
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

    std::vector<message> replies = dispatch(m, from, *id);
    if (auto it = clients_.find(from); it != clients_.end()) {
        auto& recent = it->second.recent;
        recent.emplace_back(*id, replies);
        while (recent.size() > rememberedRequests) recent.pop_front();
    }
    sendAll(std::move(replies));
    if (m.address == address::quit) engine_.quit();
    return out;
}

std::vector<message> serverSession::dispatch(const message& m, endpoint from, std::int32_t id) {
    const std::string& a = m.address;
    auto malformed = [&] {
        return std::vector<message>{
            failure(id, errorCode::malformedArguments, a + ": malformed arguments (" + m.tags() + ")")};
    };

    if (a == address::hello) {
        const auto clientVersion = m.string(1);
        const auto metadataVersion = m.string(2);
        if (!clientVersion || !metadataVersion) return malformed();
        if (*metadataVersion != info_.metadataVersion) {
            return {failure(id, errorCode::versionMismatch,
                            "metadata version mismatch: client " + std::string(*metadataVersion) +
                                ", server " + info_.metadataVersion)};
        }
        clients_.try_emplace(from);
        return {reply(address::helloOk,
                      {id, info_.serverVersion, info_.sampleRate, info_.blockSize})};
    }
    if (a == address::notify) {
        const auto on = m.int32(1);
        if (!on) return malformed();
        clients_[from].notify = *on != 0;
        return {done(id)};
    }
    if (a == address::status) {
        const serverCounters c = engine_.counters();
        return {reply(address::statusReply, {id, c.load, c.missedDeadlines, c.xruns, c.drops,
                                             c.overflows, c.activePlan})};
    }
    if (a == address::irBegin) {
        const auto plan = m.int32(1);
        const auto total = m.int64(2);
        const auto count = m.int32(3);
        const auto crc = m.int32(4);
        if (!plan || !total || !count || !crc) return malformed();
        if (!ir_.begin(*plan, *total, *count, std::bit_cast<std::uint32_t>(*crc))) {
            return {failure(id, errorCode::irInvalid, "invalid /ir.begin announcement")};
        }
        return {done(id)};
    }
    if (a == address::irChunk) {
        const auto plan = m.int32(0);
        const auto index = m.int32(1);
        const blob* data = m.bytes(2);
        if (plan && index && data) ir_.add(*plan, *index, data->bytes);
        return {};  // no reply (§4)
    }
    if (a == address::irEnd) {
        const auto plan = m.int32(1);
        if (!plan) return malformed();
        return handleIrEnd(id, *plan);
    }
    if (a == address::play) {
        const auto plan = m.int32(1);
        const auto fade = m.float32(2);
        const auto quant = m.string(3);
        if (!plan || !fade || !quant || *fade < 0.0f) return malformed();
        if (*quant != "none" && *quant != "beat" && *quant != "bar") return malformed();
        if (!engine_.hasPlan(*plan)) {
            return {failure(id, errorCode::unknownPlan, "unknown plan " + std::to_string(*plan))};
        }
        engine_.play(*plan, *fade, *quant);
        return {done(id)};
    }
    if (a == address::stop) {
        const auto fade = m.float32(1);
        if (!fade || *fade < 0.0f) return malformed();
        engine_.stop(*fade);
        return {done(id)};
    }
    if (a == address::controlSet) {
        // index, value pairs; no request ID and no reply
        for (std::size_t i = 0; i + 1 < m.args.size(); i += 2) {
            const auto index = m.int32(i);
            const auto value = m.float32(i + 1);
            if (index && value) engine_.setControl(*index, *value);
        }
        return {};
    }
    if (a == address::controlGet) {
        const auto index = m.int32(1);
        if (!index) return malformed();
        return {reply(address::controlValue, {id, *index, engine_.control(*index)})};
    }
    if (a == address::sync) return {reply(address::synced, {id})};
    if (a == address::quit) return {done(id)};
    return {failure(id, errorCode::unknownAddress, "unknown address " + a)};
}

std::vector<message> serverSession::handleIrEnd(std::int32_t id, std::int32_t planId) {
    auto r = ir_.finish(planId);
    using kind = irAssembler::result::kind;
    switch (r.what) {
    case kind::unknownPlan:
        return {failure(id, errorCode::unknownPlan, "no /ir.begin for plan " + std::to_string(planId))};
    case kind::missing: {
        message m = reply(address::irMissing, {id, planId});
        for (auto index : r.missing) m.args.emplace_back(index);
        return {m};
    }
    case kind::incomplete:
        return {failure(id, errorCode::irIncomplete, "assembled IR size differs from /ir.begin")};
    case kind::checksumMismatch:
        return {failure(id, errorCode::irChecksumMismatch, "IR checksum mismatch")};
    case kind::complete:
        if (auto error = engine_.loadPlan(planId, std::move(r.payload))) {
            return {failure(id, errorCode::irInvalid, *error)};
        }
        return {done(id)};
    }
    return {};
}

std::vector<outgoing> serverSession::broadcast(const message& m) const {
    std::vector<outgoing> out;
    for (const auto& [client, state] : clients_) {
        if (state.notify) out.push_back({client, m});
    }
    return out;
}

std::vector<outgoing> serverSession::countersMessages() const {
    const serverCounters c = engine_.counters();
    return broadcast(reply(address::counters,
                           {c.missedDeadlines, c.xruns, c.drops, c.overflows, c.load}));
}

} // namespace protocol
