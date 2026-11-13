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

namespace protocol {

/// Splits @p payload into chunks of at most @p maxBytes bytes. An empty
/// payload has no chunks.
std::vector<std::string_view> splitChunks(std::string_view payload,
                                          std::size_t maxBytes = maxChunkBytes);

/// Reassembles IR announced by `/ir.begin`, one plan ID at a time per
/// plan. Used by the server's network thread.
class irAssembler {
public:
    /// Outcome of finish().
    struct result {
        /// Outcome kinds.
        enum class kind {
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
    /// plan. Returns false when the announcement is invalid.
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

private:
    struct transfer {
        std::int64_t totalBytes = 0;
        std::uint32_t crc = 0;
        std::vector<std::optional<std::vector<std::uint8_t>>> chunks;
    };
    std::map<std::int32_t, transfer> transfers_;
};

} // namespace protocol
