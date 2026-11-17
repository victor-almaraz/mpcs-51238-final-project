/// @file
/// The client-server message set, error codes, and defaults
/// (`protocol.md` §3–§8).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

/// OSC over UDP between client and server: encoding and decoding, the
/// message set, IR chunking, and retries (the `protocol` module).
namespace protocol {

/// Default UDP port of the server on 127.0.0.1.
inline constexpr std::uint16_t defaultServerPort = 57190;

/// Reply timeout and retry count for requests (`protocol.md` §3.2).
inline constexpr int defaultTimeoutMs = 250;
/// Resends after the first attempt.
inline constexpr int defaultRetries = 3;

/// Largest IR chunk payload (`protocol.md` §6.1).
inline constexpr std::size_t maxChunkBytes = std::size_t{32} * 1024;

/// Largest assembled IR the server accepts.
inline constexpr std::size_t maxIrBytes = std::size_t{64} * 1024 * 1024;

/// Interval between `/counters` messages (`protocol.md` §5).
inline constexpr int countersIntervalMs = 100;

/// Longest fade that `/play` and `/stop` accept, in seconds. A fade that
/// is not finite, is negative, or exceeds this fails with
/// `malformedArguments` (`protocol.md` §4, §8).
inline constexpr float maxFadeSeconds = 3600.0f;

/// True when @p seconds is a fade that `/play` and `/stop` accept.
constexpr bool validFade(float seconds) noexcept {
    // Written so that NaN, which compares false with everything, fails.
    return seconds >= 0.0f && seconds <= maxFadeSeconds;
}

/// Clients the server remembers at once. A `/hello` from a new client
/// beyond this forgets the client heard from least recently
/// (`protocol.md` §3.3, §5).
inline constexpr std::size_t maxClients = 32;

/// IR transfers the server assembles at once (`protocol.md` §6). A new
/// `/ir.begin` beyond this, or beyond `maxIrBytes` announced in total,
/// abandons the oldest transfers.
inline constexpr std::size_t maxPendingTransfers = 8;

/// IR chunks the client sends before it waits for `/synced`, so a burst
/// never exceeds a small socket receive buffer (Linux defaults to about
/// 208 KiB) (`protocol.md` §6.1).
inline constexpr std::size_t chunksPerWindow = 4;

/// Message addresses.
namespace address {
// Client to server (§4)
/// Starts a session and checks versions.
inline constexpr std::string_view hello = "/hello";
/// Turns notifications on or off.
inline constexpr std::string_view notify = "/notify";
/// Asks for load, counters, and the active plan.
inline constexpr std::string_view status = "/status";
/// Announces an IR transfer.
inline constexpr std::string_view irBegin = "/ir.begin";
/// Carries one IR chunk; no reply.
inline constexpr std::string_view irChunk = "/ir.chunk";
/// Ends an IR transfer.
inline constexpr std::string_view irEnd = "/ir.end";
/// Schedules a swap to a plan.
inline constexpr std::string_view play = "/play";
/// Fades out the active plan.
inline constexpr std::string_view stop = "/stop";
/// Sets control values; no reply.
inline constexpr std::string_view controlSet = "/c.set";
/// Reads a control value.
inline constexpr std::string_view controlGet = "/c.get";
/// Replies after all earlier messages are processed.
inline constexpr std::string_view sync = "/sync";
/// Stops the server.
inline constexpr std::string_view quit = "/quit";
// Replies (§3.4, §4)
/// The request succeeded.
inline constexpr std::string_view done = "/done";
/// The request failed, or an unsolicited failure (request ID 0).
inline constexpr std::string_view fail = "/fail";
/// Reply to `/hello`: server version, sample rate, block size.
inline constexpr std::string_view helloOk = "/hello.ok";
/// Reply to `/status`.
inline constexpr std::string_view statusReply = "/status.reply";
/// Reply to `/ir.end` listing chunks to resend.
inline constexpr std::string_view irMissing = "/ir.missing";
/// Reply to `/c.get`.
inline constexpr std::string_view controlValue = "/c.value";
/// Reply to `/sync`.
inline constexpr std::string_view synced = "/synced";
// Server to client (§5)
/// A swap took effect.
inline constexpr std::string_view planActive = "/plan.active";
/// A plan and its fading nodes are gone.
inline constexpr std::string_view planRetired = "/plan.retired";
/// Periodic counters, ten times per second.
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
    signalFault = 10,  ///< A sink muted itself: non-finite samples
};

/// True when replies to @p addr carry the request ID as their first
/// argument.
constexpr bool isReply(std::string_view addr) noexcept {
    return addr == address::done || addr == address::fail || addr == address::helloOk ||
           addr == address::statusReply || addr == address::irMissing ||
           addr == address::controlValue || addr == address::synced;
}

} // namespace protocol
