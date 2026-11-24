// Pages as an OSC transport (io.md §12.3, §12.11), behind an interface, so
// the OSC bridge's routing is tested against a scripted endpoint. The
// server's is the page bridge.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "pageMessages.h"

namespace server {

/// Where pages connect. The control thread opens their listener on the
/// active plan's page port, takes the messages they sent, and hands them
/// the messages they should receive.
class pageEndpoint {
public:
    virtual ~pageEndpoint() = default;

    /// Listens on @p port, the active plan's page port, or on none
    /// (io.md §12.2).
    virtual void listen(std::optional<std::uint16_t> port) = 0;
    /// The messages pages sent since the last call, in order.
    virtual std::vector<oscBytes> takeMessages() = 0;
    /// Queues @p osc, an encoded OSC message, for every connected page.
    virtual void send(std::span<const std::uint8_t> osc) = 0;

protected:
    pageEndpoint() = default;
    pageEndpoint(const pageEndpoint&) = default;
    pageEndpoint& operator=(const pageEndpoint&) = default;
    pageEndpoint(pageEndpoint&&) = default;
    pageEndpoint& operator=(pageEndpoint&&) = default;
};

}  // namespace server
