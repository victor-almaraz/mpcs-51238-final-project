#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/protocol/crc32.h>
#include <espresso/protocol/irTransfer.h>
#include <espresso/protocol/messages.h>
#include <espresso/protocol/osc.h>

namespace protocol {

std::vector<std::string_view> splitChunks(std::string_view payload, std::size_t maxBytes) {
    std::vector<std::string_view> chunks;
    if (maxBytes == 0) return chunks;
    for (std::size_t at = 0; at < payload.size(); at += maxBytes) {
        chunks.push_back(payload.substr(at, maxBytes));
    }
    return chunks;
}

std::vector<argument> irBeginArgs(std::int32_t planId, std::string_view payload) {
    const auto count = (payload.size() + maxChunkBytes - 1) / maxChunkBytes;  // splitChunks(payload).size()
    return {planId, static_cast<std::int64_t>(payload.size()), static_cast<std::int32_t>(count),
            std::bit_cast<std::int32_t>(crc32(payload))};
}

message chunkMessage(std::int32_t planId, std::int32_t index, std::string_view chunk) {
    return {.address = std::string(address::irChunk),
            .args = {planId, index, blob{{chunk.begin(), chunk.end()}}}};
}

bool irAssembler::begin(std::int32_t planId, std::int64_t totalBytes, std::int32_t chunkCount,
                        std::uint32_t crc) {
    if (totalBytes < 0 || std::cmp_greater(totalBytes, maxIrBytes) || chunkCount < 0) {
        return false;
    }
    const auto expected = static_cast<std::int64_t>((static_cast<std::size_t>(totalBytes) +
                                                     maxChunkBytes - 1) / maxChunkBytes);
    if (chunkCount < expected || chunkCount > expected + 1) return false;

    // Make room: abandon the oldest transfers until the new one fits the
    // count and byte limits. totalBytes <= maxIrBytes, so this ends.
    transfers_.erase(planId);  // replaced, not abandoned
    const auto announced = [this] {
        std::uint64_t sum = 0;
        for (const auto& [id, t] : transfers_) sum += static_cast<std::uint64_t>(t.totalBytes);
        return sum;
    };
    while (!transfers_.empty() && (transfers_.size() + 1 > maxPendingTransfers ||
                                   announced() + static_cast<std::uint64_t>(totalBytes) > maxIrBytes)) {
        const auto oldest = std::ranges::min_element(
            transfers_, [](const auto& a, const auto& b) { return a.second.started < b.second.started; });
        transfers_.erase(oldest);
        ++abandoned_;
    }

    transfer t;
    t.totalBytes = totalBytes;
    t.crc = crc;
    t.started = nextStart_++;
    t.chunks.resize(static_cast<std::size_t>(chunkCount));
    transfers_[planId] = std::move(t);
    return true;
}

bool irAssembler::add(std::int32_t planId, std::int32_t index, std::span<const std::uint8_t> data) {
    const auto it = transfers_.find(planId);
    if (it == transfers_.end() || index < 0 ||
        static_cast<std::size_t>(index) >= it->second.chunks.size() || data.size() > maxChunkBytes) {
        return false;
    }
    it->second.chunks[static_cast<std::size_t>(index)].emplace(data.begin(), data.end());
    return true;
}

irAssembler::result irAssembler::finish(std::int32_t planId) {
    const auto it = transfers_.find(planId);
    if (it == transfers_.end()) return {.what = result::kind::unknownPlan, .payload = {}, .missing = {}};
    transfer& t = it->second;

    result r{.what = result::kind::missing, .payload = {}, .missing = {}};
    for (std::size_t i = 0; i < t.chunks.size(); ++i) {
        if (!t.chunks[i]) r.missing.push_back(static_cast<std::int32_t>(i));
    }
    if (!r.missing.empty()) return r;

    std::string payload;
    payload.reserve(static_cast<std::size_t>(t.totalBytes));
    for (const auto& c : t.chunks) {
        if (c) payload.append(c->begin(), c->end());  // every chunk is present: missing is empty
    }
    const std::uint32_t expectedCrc = t.crc;
    const auto expectedSize = static_cast<std::size_t>(t.totalBytes);
    transfers_.erase(it);

    if (payload.size() != expectedSize)
        return {.what = result::kind::incomplete, .payload = {}, .missing = {}};
    if (crc32(payload) != expectedCrc)
        return {.what = result::kind::checksumMismatch, .payload = {}, .missing = {}};
    return {.what = result::kind::complete, .payload = std::move(payload), .missing = {}};
}

} // namespace protocol
