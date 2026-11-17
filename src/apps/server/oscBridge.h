// The bridge between the engine's OSC ports and UDP sockets on
// 127.0.0.1: listens on the ports that oscIn sources ask for, delivers
// received messages, and sends what oscOut sinks queue. Has no JUCE
// dependency.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <ostream>
#include <set>

#include <espresso/primitives/io.h>
#include <espresso/protocol/udp.h>

namespace server {

/// Moves OSC between UDP sockets and engine ports. Control thread only.
class oscBridge {
public:
    /// Datagrams read from each input socket per pump() call, so a flood
    /// on one port cannot hold the control thread.
    static constexpr std::size_t maxDatagramsPerPump = 256;

    /// Opens and closes input sockets to match the ports plans ask for,
    /// delivers waiting datagrams stamped with @p now (device clock,
    /// seconds), at most maxDatagramsPerPump per port, and sends queued
    /// output. Socket errors are reported once per port on @p errors;
    /// oversized datagrams and output that cannot be sent are counted as
    /// drops on @p io. Sent messages are logged on @p log if given.
    /// Returns the number of datagrams received.
    std::size_t pump(primitives::ioHub& io, double now, std::ostream& errors,
                     std::optional<std::reference_wrapper<std::ostream>> log = std::nullopt);

private:
    std::map<std::uint16_t, protocol::udpSocket> inputs_;
    std::set<std::uint16_t> failed_;
    std::optional<protocol::udpSocket> output_;
    bool outputFailed_ = false;  // the output socket error was reported
};

} // namespace server
