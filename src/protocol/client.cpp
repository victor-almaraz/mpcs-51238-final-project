#include <proj/protocol/client.h>

#include <bit>

#include <proj/protocol/crc32.h>
#include <proj/protocol/irTransfer.h>

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

client::client(clientOptions options, listener unsolicited)
    : options_(options), unsolicited_(std::move(unsolicited)) {}

client::~client() {
    stopping_ = true;
    if (receiver_.joinable()) receiver_.join();
}

bool client::open(std::string* error) {
    if (socket_) return true;
    socket_ = udpSocket::bind(0, error);
    if (!socket_) return false;
    receiver_ = std::thread([this] { receiveLoop(); });
    return true;
}

bool client::send(const message& m) {
    if (!socket_) return false;
    return socket_->send(encode(m), {options_.serverPort});
}

requestResult client::request(std::string_view addr, std::vector<argument> args) {
    if (!socket_) return {std::nullopt, "client socket is not open"};
    std::int32_t id = 0;
    {
        std::lock_guard lock(mutex_);
        id = nextId_++;
        if (nextId_ <= 0) nextId_ = 1;  // 0 marks unsolicited failures
        waiting_[id];
    }
    message m{std::string(addr), {}};
    m.args.reserve(args.size() + 1);
    m.args.emplace_back(id);
    for (auto& a : args) m.args.push_back(std::move(a));
    const auto packet = encode(m);

    const auto timeout = std::chrono::milliseconds(options_.timeoutMs);
    for (int attempt = 0; attempt <= options_.retries; ++attempt) {
        socket_->send(packet, {options_.serverPort});
        std::unique_lock lock(mutex_);
        if (replied_.wait_for(lock, timeout, [&] { return waiting_[id].has_value(); })) {
            message reply = std::move(*waiting_[id]);
            waiting_.erase(id);
            return {std::move(reply), {}};
        }
    }
    std::lock_guard lock(mutex_);
    waiting_.erase(id);
    return {std::nullopt, std::string(addr) + ": no reply from 127.0.0.1:" +
                              std::to_string(options_.serverPort) + " after " +
                              std::to_string(options_.retries + 1) + " attempts"};
}

std::optional<std::string> client::submitIr(std::int32_t planId, std::string_view payload) {
    const auto chunks = splitChunks(payload);
    const auto count = static_cast<std::int32_t>(chunks.size());
    const auto crc = std::bit_cast<std::int32_t>(crc32(payload));
    auto begin = request(address::irBegin,
                         {planId, static_cast<std::int64_t>(payload.size()), count, crc});
    if (!begin.ok()) return "/ir.begin: " + begin.failText();

    auto sendChunk = [&](std::int32_t index) {
        const std::string_view c = chunks[static_cast<std::size_t>(index)];
        blob b{std::vector<std::uint8_t>(c.begin(), c.end())};
        send({std::string(address::irChunk), {planId, index, std::move(b)}});
    };
    for (std::int32_t i = 0; i < count; ++i) sendChunk(i);

    for (int round = 0; round < maxIrRounds; ++round) {
        auto end = request(address::irEnd, {planId});
        if (!end.reply) return "/ir.end: " + end.error;
        if (end.reply->address == address::done) return std::nullopt;
        if (end.reply->address != address::irMissing) return "/ir.end: " + end.failText();
        for (std::size_t i = 2; i < end.reply->args.size(); ++i) {
            const auto index = end.reply->int32(i);
            if (index && *index >= 0 && *index < count) sendChunk(*index);
        }
    }
    return "/ir.end: chunks still missing after " + std::to_string(maxIrRounds) + " rounds";
}

std::chrono::steady_clock::time_point client::lastHeard() const {
    std::lock_guard lock(mutex_);
    return lastHeard_;
}

void client::receiveLoop() {
    while (!stopping_) {
        auto d = socket_->receive(50);
        if (!d || d->from.port != options_.serverPort) continue;
        auto m = decode(d->bytes);
        if (!m) continue;
        bool delivered = false;
        {
            std::lock_guard lock(mutex_);
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
