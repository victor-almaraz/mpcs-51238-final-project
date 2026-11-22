// The bridge between the engine's OSC ports and their two transports: UDP
// sockets on 127.0.0.1, and pages (io.md §12.3). Listens on the ports
// that oscIn sources ask for, delivers received messages, and sends what
// oscOut sinks queue. Has no JUCE dependency.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <vector>

#include <espresso/primitives/io.h>
#include <espresso/protocol/udp.h>

#include "pageEndpoint.h"

namespace server {

/// Moves OSC between engine ports, UDP sockets, and pages. Control thread
/// only.
class oscBridge {
public:
    /// Datagrams read from each input socket per pump() call, so a flood
    /// on one port cannot hold the control thread.
    static constexpr std::size_t maxDatagramsPerPump = 256;

    /// A bridge for UDP alone.
    oscBridge() = default;
    /// A bridge that also carries OSC to and from @p pages, which must
    /// outlive it.
    explicit oscBridge(pageEndpoint& pages) : pages_(pages) {}

    /// Opens and closes input sockets to match the ports plans ask for,
    /// delivers waiting datagrams stamped with @p now (device clock,
    /// seconds), at most maxDatagramsPerPump per port, and sends queued
    /// output. @p pagePort is the active plan's page port P (io.md §12.2,
    /// §12.3): the page endpoint listens on it; pages' messages are
    /// delivered to OSC input port P, stamped with @p now, before the
    /// datagrams; and output to P goes to the page endpoint and never to
    /// UDP, even while its listener cannot open. Socket errors are reported
    /// once per port on @p errors. Counted as drops on @p io: oversized
    /// datagrams and page messages, page messages that no node reads or
    /// that arrive while there is no page port, output that cannot be
    /// sent, and output to P from a bridge without pages. Messages sent
    /// over UDP are logged on @p log if given. Returns the number of
    /// datagrams received.
    std::size_t pump(primitives::ioHub& io, double now, std::ostream& errors,
                     std::optional<std::reference_wrapper<std::ostream>> log = std::nullopt,
                     std::optional<std::uint16_t> pagePort = std::nullopt);

private:
    // Delivers what pages sent to OSC input port @p pagePort.
    void receivePages(primitives::ioHub& io, double now, std::optional<std::uint16_t> pagePort);
    // Sends @p p, output to the page port, to the pages.
    void sendPages(primitives::ioHub& io, const primitives::oscPacket& p);

    std::optional<std::reference_wrapper<pageEndpoint>> pages_;
    // The UDP ports OSC inputs asked for at the hub's generation scanned_.
    std::vector<std::uint16_t> wanted_;
    std::optional<std::uint64_t> scanned_;
    std::map<std::uint16_t, protocol::udpSocket> inputs_;
    std::set<std::uint16_t> failed_;
    std::optional<protocol::udpSocket> output_;
    bool outputFailed_ = false;  // the output socket error was reported
};

}  // namespace server
