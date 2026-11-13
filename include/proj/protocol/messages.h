/// @file
/// The client-server message set, error codes, and defaults
/// (`protocol.md` §3–§8).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace protocol {

/// Default UDP port of the server on 127.0.0.1.
inline constexpr std::uint16_t defaultServerPort = 57190;

/// Reply timeout and retry count for requests (`protocol.md` §3.2).
inline constexpr int defaultTimeoutMs = 250;
inline constexpr int defaultRetries = 3;

/// Largest IR chunk payload (`protocol.md` §6.1).
inline constexpr std::size_t maxChunkBytes = 32 * 1024;

/// Largest assembled IR the server accepts.
inline constexpr std::size_t maxIrBytes = 64 * 1024 * 1024;

/// Interval between `/counters` messages (`protocol.md` §5).
inline constexpr int countersIntervalMs = 100;

/// Message addresses.
namespace address {
// Client to server (§4)
inline constexpr std::string_view hello = "/hello";
inline constexpr std::string_view notify = "/notify";
inline constexpr std::string_view status = "/status";
inline constexpr std::string_view irBegin = "/ir.begin";
inline constexpr std::string_view irChunk = "/ir.chunk";
inline constexpr std::string_view irEnd = "/ir.end";
inline constexpr std::string_view play = "/play";
inline constexpr std::string_view stop = "/stop";
inline constexpr std::string_view controlSet = "/c.set";
inline constexpr std::string_view controlGet = "/c.get";
inline constexpr std::string_view sync = "/sync";
inline constexpr std::string_view quit = "/quit";
// Replies (§3.4, §4)
inline constexpr std::string_view done = "/done";
inline constexpr std::string_view fail = "/fail";
inline constexpr std::string_view helloOk = "/hello.ok";
inline constexpr std::string_view statusReply = "/status.reply";
inline constexpr std::string_view irMissing = "/ir.missing";
inline constexpr std::string_view controlValue = "/c.value";
inline constexpr std::string_view synced = "/synced";
// Server to client (§5)
inline constexpr std::string_view planActive = "/plan.active";
inline constexpr std::string_view planRetired = "/plan.retired";
inline constexpr std::string_view counters = "/counters";
} // namespace address

/// Error codes carried by `/fail` (`protocol.md` §8).
enum class errorCode : std::int32_t {
    unknownAddress = 1,
    malformedArguments = 2,
    notConnected = 3,
    versionMismatch = 4,
    irIncomplete = 5,
    irChecksumMismatch = 6,
    irInvalid = 7,
    unknownPlan = 8,
    deviceError = 9,
};

/// True when replies to @p addr carry the request ID as their first
/// argument.
constexpr bool isReply(std::string_view addr) noexcept {
    return addr == address::done || addr == address::fail || addr == address::helloOk ||
           addr == address::statusReply || addr == address::irMissing ||
           addr == address::controlValue || addr == address::synced;
}

} // namespace protocol
