// The bridge between the engine's OSC ports and UDP sockets on
// 127.0.0.1: listens on the ports that oscIn sources ask for, delivers
// received messages, and sends what oscOut sinks queue. Has no JUCE
// dependency.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <ostream>
#include <set>

#include <proj/primitives/io.h>
#include <proj/protocol/udp.h>

namespace server {

/// Moves OSC between UDP sockets and engine ports. Control thread only.
class oscBridge {
public:
    /// Opens and closes input sockets to match the ports plans ask for,
    /// delivers waiting datagrams stamped with @p now (device clock,
    /// seconds), and sends queued output. Socket errors are reported once
    /// per port on @p errors; sent messages are logged on @p log if given.
    void pump(primitives::ioHub& io, double now, std::ostream& errors,
              std::optional<std::reference_wrapper<std::ostream>> log = std::nullopt);

private:
    std::map<std::uint16_t, protocol::udpSocket> inputs_;
    std::set<std::uint16_t> failed_;
    std::optional<protocol::udpSocket> output_;
};

} // namespace server
