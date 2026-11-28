/// @file
/// The client-server message set, error codes, and defaults
/// (`protocol.md` §3–§8).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

/// OSC over UDP between client and server: encoding and decoding, the
/// message set, IR chunking, and retries (the `protocol` module).
namespace protocol {

/// Default UDP port of the server on 127.0.0.1.
inline constexpr std::uint16_t defaultServerPort = 57190;

/// Time to wait for each reply to a request, in milliseconds
/// (`protocol.md` §3.2).
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
/// Periodic counters, tempo, and beat position, ten times per second.
inline constexpr std::string_view counters = "/counters";
/// A `bufferSave` node wrote its buffer to a file.
inline constexpr std::string_view bufferSaved = "/buffer.saved";
/// A serial or web node's I/O errors since its last report: node,
/// message, count; at most once a second per node (`io.md` §10.2).
inline constexpr std::string_view ioError = "/io.error";
}  // namespace address

/// Error codes carried by `/fail` (`protocol.md` §8).
enum class errorCode : std::int32_t {
    unknownAddress = 1,      ///< No handler for the address
    malformedArguments = 2,  ///< Arguments missing, of the wrong type, or out of range
    notConnected = 3,        ///< The client has not completed `/hello`
    versionMismatch = 4,     ///< The client's metadata version differs from the server's
    irIncomplete = 5,        ///< The assembled IR size differs from `/ir.begin`
    irChecksumMismatch = 6,  ///< The assembled IR's CRC-32 differs from `/ir.begin`
    irInvalid = 7,           ///< The announcement or the IR is invalid, or the plan cannot be built
    unknownPlan = 8,         ///< No such plan, or it was already played
    deviceError = 9,         ///< A device failed
    signalFault = 10,        ///< A sink muted itself: non-finite samples
    fileError = 11,          ///< A buffer could not be saved to its file
};

/// True when replies to @p addr carry the request ID as their first
/// argument.
constexpr bool isReply(std::string_view addr) noexcept {
    return addr == address::done || addr == address::fail || addr == address::helloOk ||
           addr == address::statusReply || addr == address::irMissing || addr == address::controlValue ||
           addr == address::synced;
}

/// What a `/fail` names in its trailing arguments (`protocol.md` §5).
enum class failSubject : std::uint8_t {
    none,    ///< The plan or request as a whole
    node,    ///< One node
    buffer,  ///< One buffer
};

/// The name `/fail` carries for @p s: `node` or `buffer`; empty for none.
constexpr std::string_view subjectName(failSubject s) noexcept {
    switch (s) {
    case failSubject::node: return "node";
    case failSubject::buffer: return "buffer";
    case failSubject::none: break;
    }
    return {};
}

/// The subject that @p name names; none for any other text.
constexpr failSubject subjectNamed(std::string_view name) noexcept {
    if (name == subjectName(failSubject::node)) return failSubject::node;
    if (name == subjectName(failSubject::buffer)) return failSubject::buffer;
    return failSubject::none;
}

/// Why a plan was refused (`protocol.md` §5): the message, and, when one
/// node or buffer of the plan failed, which, so a client can find its
/// source.
struct planError {
    /// What is wrong, as `/fail` reports it.
    std::string message;
    /// What failed; none for the plan as a whole.
    failSubject subject = failSubject::none;
    /// The node's or buffer's ID; empty for the plan as a whole.
    std::string id;

    /// An error about the plan as a whole.
    static planError plain(std::string message) {
        return {.message = std::move(message), .subject = failSubject::none, .id = {}};
    }
};

}  // namespace protocol
