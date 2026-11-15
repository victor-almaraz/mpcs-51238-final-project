/// @file
/// Splitting serialized IR into chunks and reassembling it
/// (`protocol.md` §6).

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <proj/protocol/messages.h>
#include <proj/protocol/osc.h>

namespace protocol {

/// Splits @p payload into chunks of at most @p maxBytes bytes. An empty
/// payload has no chunks.
std::vector<std::string_view> splitChunks(std::string_view payload,
                                          std::size_t maxBytes = maxChunkBytes);

/// The arguments of `/ir.begin` after the request ID, announcing
/// @p payload as plan @p planId: plan ID, total bytes, the chunk count of
/// splitChunks(), and the CRC-32 (`protocol.md` §6.2).
std::vector<argument> irBeginArgs(std::int32_t planId, std::string_view payload);

/// The `/ir.chunk` message carrying @p chunk as chunk @p index of plan
/// @p planId (`protocol.md` §4).
message chunkMessage(std::int32_t planId, std::int32_t index, std::string_view chunk);

/// Reassembles IR announced by `/ir.begin`, one transfer per plan ID.
/// Used by the server's network thread. At most `maxPendingTransfers`
/// transfers, announcing at most `maxIrBytes` in total, are kept: a new
/// announcement abandons the oldest transfers until it fits, so transfers
/// that a client never finishes cannot accumulate.
class irAssembler {
public:
    /// Outcome of finish().
    struct result {
        /// Outcome kinds.
        enum class kind : std::uint8_t {
            complete,          ///< Every chunk arrived and the CRC-32 matches
            missing,           ///< Chunks are missing; see `missing`
            checksumMismatch,  ///< The CRC-32 differs from `/ir.begin`
            incomplete,        ///< The assembled size differs from `/ir.begin`
            unknownPlan,       ///< No `/ir.begin` for the plan
        };
        kind what;  ///< The outcome
        std::string payload;                  ///< For complete
        std::vector<std::int32_t> missing;    ///< For missing: chunk indices
    };

    /// Starts plan @p planId. Replaces an earlier transfer of the same
    /// plan and abandons the oldest transfers beyond the limits. Returns
    /// false when the announcement is invalid.
    bool begin(std::int32_t planId, std::int64_t totalBytes, std::int32_t chunkCount,
               std::uint32_t crc);

    /// Stores chunk @p index of @p planId. Returns false for an unknown
    /// plan, an index out of range, or an oversized chunk.
    bool add(std::int32_t planId, std::int32_t index, std::span<const std::uint8_t> data);

    /// Checks that every chunk arrived and the CRC-32 matches. A complete
    /// or failed transfer is forgotten; a transfer with missing chunks is
    /// kept so the client can resend them (`protocol.md` §6.3).
    result finish(std::int32_t planId);

    /// Number of transfers in progress.
    std::size_t pending() const noexcept { return transfers_.size(); }

    /// Transfers abandoned to stay within the limits.
    std::size_t abandoned() const noexcept { return abandoned_; }

private:
    struct transfer {
        std::int64_t totalBytes = 0;
        std::uint32_t crc = 0;
        std::uint64_t started = 0;  // order of /ir.begin, oldest first
        std::vector<std::optional<std::vector<std::uint8_t>>> chunks;
    };
    std::map<std::int32_t, transfer> transfers_;
    std::uint64_t nextStart_ = 0;
    std::size_t abandoned_ = 0;
};

} // namespace protocol
