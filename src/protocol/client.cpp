#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/protocol/client.h>
#include <espresso/protocol/irTransfer.h>
#include <espresso/protocol/jthread.h>
#include <espresso/protocol/messages.h>
#include <espresso/protocol/osc.h>
#include <espresso/protocol/udp.h>

namespace protocol {

namespace {

// Rounds of /ir.end and chunk resends before giving up.
constexpr int maxIrRounds = 5;

} // namespace

std::optional<errorCode> requestResult::failCode() const {
    if (!reply || reply->address != address::fail) return std::nullopt;
    if (const auto code = reply->int32(1)) return static_cast<errorCode>(*code);
    return std::nullopt;
}

std::string requestResult::failText() const {
    if (!reply) return error;
    if (reply->address != address::fail) return {};
    return std::string(reply->string(2).value_or("request failed"));
}

planError requestResult::failNames() const {
    planError names{.message = failText(), .subject = {}, .id = {}};
    if (reply && reply->address == address::fail) {
        names.subject = std::string(reply->string(3).value_or(""));
        names.id = std::string(reply->string(4).value_or(""));
    }
    return names;
}

client::client(clientOptions options, listener unsolicited)
    : options_(options), unsolicited_(std::move(unsolicited)) {}

client::~client() {
    // Best effort: UDP gives the server no other notice that this port is
    // gone. The reply, if any, is not awaited. receiver_ then stops and
    // joins before the other members are destroyed.
    std::int32_t id = 0;
    {
        const std::scoped_lock lock(mutex_);
        if (!socket_ || !greeted_) return;
        id = nextId_;
    }
    send({.address = std::string(address::notify), .args = {id, std::int32_t{0}}});
}

bool client::open(std::string& error) {
    if (socket_) return true;
    socket_ = udpSocket::bind(0, error);
    if (!socket_) return false;
    // socket_ is never replaced once open, and receiver_ joins before it is
    // destroyed, so the thread can hold a reference to it.
    receiver_ = espresso::jthread(
        [this, &socket = *socket_](const espresso::stop_token& stop) { receiveLoop(socket, stop); });
    return true;
}

bool client::send(const message& m) {
    if (!socket_) return false;
    return socket_->send(encode(m), {options_.serverPort});
}

requestResult client::request(std::string_view addr, std::vector<argument> args) {
    return request(addr, std::move(args), options_.retries);
}

requestResult client::request(std::string_view addr, std::vector<argument> args, int retries) {
    if (!socket_) return {.reply = std::nullopt, .error = "client socket is not open"};
    std::int32_t id = 0;
    {
        std::scoped_lock lock(mutex_);
        id = nextId_++;
        if (nextId_ <= 0) nextId_ = 1;  // 0 marks unsolicited failures
        waiting_[id];
    }
    message m{.address = std::string(addr), .args = {}};
    m.args.reserve(args.size() + 1);
    m.args.emplace_back(id);
    for (auto& a : args) m.args.push_back(std::move(a));
    const auto packet = encode(m);

    const auto timeout = std::chrono::milliseconds(options_.timeoutMs);
    for (int attempt = 0; attempt <= retries; ++attempt) {
        socket_->send(packet, {options_.serverPort});
        std::unique_lock lock(mutex_);
        std::optional<message>& slot = waiting_[id];  // map references stay valid while the entry exists
        replied_.wait_for(lock, timeout, [&] { return slot.has_value(); });
        if (slot) {
            message reply = std::move(*slot);
            waiting_.erase(id);
            if (reply.address == address::helloOk) greeted_ = true;
            return {.reply = std::move(reply), .error = {}};
        }
    }
    std::scoped_lock lock(mutex_);
    waiting_.erase(id);
    return {.reply = std::nullopt,
            .error = std::string(addr) + ": no reply from 127.0.0.1:" + std::to_string(options_.serverPort) +
                     " after " + std::to_string(retries + 1) + " attempts"};
}

std::optional<planError> client::submitIr(std::int32_t planId, std::string_view payload) {
    const auto failed = [](std::string text) {
        return planError{.message = std::move(text), .subject = {}, .id = {}};
    };
    const auto chunks = splitChunks(payload);
    const auto count = static_cast<std::int32_t>(chunks.size());
    auto begin = request(address::irBegin, irBeginArgs(planId, payload));
    if (!begin.ok()) return failed("/ir.begin: " + begin.failText());

    // Sends the chunks in windows of chunksPerWindow. The server handles
    // datagrams in order, so /synced means the window left its receive
    // buffer; without the wait, a burst larger than the buffer is lost,
    // and so is every resend of it (§6.1, §6.3).
    auto sendChunks = [&](const std::vector<std::int32_t>& indices) -> std::optional<planError> {
        for (std::size_t n = 0; n < indices.size(); ++n) {
            const auto index = indices[n];
            send(chunkMessage(planId, index, chunks[static_cast<std::size_t>(index)]));
            if ((n + 1) % chunksPerWindow == 0 && n + 1 < indices.size()) {
                if (const auto synced = request(address::sync); !synced.ok())
                    return failed("/sync: " + synced.failText());
            }
        }
        return std::nullopt;
    };
    std::vector<std::int32_t> all(chunks.size());
    for (std::int32_t i = 0; i < count; ++i) all[static_cast<std::size_t>(i)] = i;
    if (auto error = sendChunks(all)) return error;

    for (int round = 0; round < maxIrRounds; ++round) {
        auto end = request(address::irEnd, {planId});
        if (!end.reply) return failed("/ir.end: " + end.error);
        if (end.reply->address == address::done) return std::nullopt;
        if (end.reply->address != address::irMissing) {
            auto error = end.failNames();
            error.message = "/ir.end: " + error.message;
            return error;
        }
        std::vector<std::int32_t> missing;
        for (std::size_t i = 2; i < end.reply->args.size(); ++i) {
            const auto index = end.reply->int32(i);
            if (index && *index >= 0 && *index < count) missing.push_back(*index);
        }
        if (auto error = sendChunks(missing)) return error;
    }
    return failed("/ir.end: chunks still missing after " + std::to_string(maxIrRounds) + " rounds");
}

std::chrono::steady_clock::time_point client::lastHeard() const {
    std::scoped_lock lock(mutex_);
    return lastHeard_;
}

void client::receiveLoop(const udpSocket& socket, const espresso::stop_token& stop) {
    while (!stop.stop_requested()) {
        auto d = socket.receive(50);
        if (!d || d->from.port != options_.serverPort) continue;
        auto m = decode(d->bytes);
        if (!m) continue;
        bool delivered = false;
        {
            std::scoped_lock lock(mutex_);
            lastHeard_ = std::chrono::steady_clock::now();
            if (isReply(m->address)) {
                if (const auto id = m->int32(0); id && *id != 0) {
                    if (auto it = waiting_.find(*id); it != waiting_.end() && !it->second) {
                        it->second = std::move(*m);
                        delivered = true;
                    } else {
                        delivered = true;  // late reply to a request that already returned
                    }
                }
            }
        }
        if (delivered) {
            replied_.notify_all();
        } else if (unsolicited_) {
            unsolicited_(*m);
        }
    }
}

} // namespace protocol
