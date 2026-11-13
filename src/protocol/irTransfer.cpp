#include <proj/protocol/irTransfer.h>

#include <proj/protocol/crc32.h>

namespace protocol {

std::vector<std::string_view> splitChunks(std::string_view payload, std::size_t maxBytes) {
    std::vector<std::string_view> chunks;
    if (maxBytes == 0) return chunks;
    for (std::size_t at = 0; at < payload.size(); at += maxBytes) {
        chunks.push_back(payload.substr(at, maxBytes));
    }
    return chunks;
}

bool irAssembler::begin(std::int32_t planId, std::int64_t totalBytes, std::int32_t chunkCount,
                        std::uint32_t crc) {
    if (totalBytes < 0 || static_cast<std::uint64_t>(totalBytes) > maxIrBytes || chunkCount < 0) {
        return false;
    }
    const auto expected = static_cast<std::int64_t>((static_cast<std::size_t>(totalBytes) +
                                                     maxChunkBytes - 1) / maxChunkBytes);
    if (chunkCount < expected || chunkCount > expected + 1) return false;
    transfer t;
    t.totalBytes = totalBytes;
    t.crc = crc;
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
    if (it == transfers_.end()) return {result::kind::unknownPlan, {}, {}};
    transfer& t = it->second;

    result r{result::kind::missing, {}, {}};
    for (std::size_t i = 0; i < t.chunks.size(); ++i) {
        if (!t.chunks[i]) r.missing.push_back(static_cast<std::int32_t>(i));
    }
    if (!r.missing.empty()) return r;

    std::string payload;
    payload.reserve(static_cast<std::size_t>(t.totalBytes));
    for (const auto& c : t.chunks) payload.append(c->begin(), c->end());
    const std::uint32_t expectedCrc = t.crc;
    const auto expectedSize = static_cast<std::size_t>(t.totalBytes);
    transfers_.erase(it);

    if (payload.size() != expectedSize) return {result::kind::incomplete, {}, {}};
    if (crc32(payload) != expectedCrc) return {result::kind::checksumMismatch, {}, {}};
    return {result::kind::complete, std::move(payload), {}};
}

} // namespace protocol
