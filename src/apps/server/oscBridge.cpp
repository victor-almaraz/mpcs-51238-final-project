#include "oscBridge.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <utility>

#include <espresso/primitives/io.h>
#include <espresso/protocol/udp.h>

#include "pageEndpoint.h"
#include "pageMessages.h"

namespace server {

void oscBridge::receivePages(primitives::ioHub& io, double now, std::optional<std::uint16_t> pagePort) {
    if (!pages_) return;
    auto& pages = pages_->get();
    // The listener follows the active plan (io.md §12.2).
    pages.listen(pagePort);
    for (const auto& bytes : pages.takeMessages()) {
        const auto p = primitives::oscPacket::of(bytes, now);
        if (!pagePort || !p || io.deliverOsc(*pagePort, *p) == 0) io.countDrop();
    }
}

void oscBridge::sendPages(primitives::ioHub& io, const primitives::oscPacket& p) {
    if (!pages_) {
        io.countDrop();
        return;
    }
    pages_->get().send(bytesOf(p.used()));
}

std::size_t oscBridge::pump(primitives::ioHub& io, double now, std::ostream& errors,
                            std::optional<std::reference_wrapper<std::ostream>> log,
                            std::optional<std::uint16_t> pagePort) {
    receivePages(io, now, pagePort);
    // The ports asked for change only with the hub's generation; a socket
    // that failed to open is still retried every pump.
    if (scanned_.changed(io.generation())) wanted_ = io.oscInputPorts();
    const auto& wanted = wanted_;
    for (auto it = inputs_.begin(); it != inputs_.end();) {
        it = std::ranges::find(wanted, it->first) == wanted.end() ? inputs_.erase(it) : std::next(it);
    }
    for (auto port : wanted) {
        if (inputs_.contains(port)) continue;
        std::string error;
        if (auto s = protocol::udpSocket::bind(port, error)) {
            inputs_.emplace(port, std::move(*s));
            failed_.erase(port);  // report again if it fails later
        } else if (failed_.insert(port).second) {
            errors << "espresso-server: cannot listen for OSC on 127.0.0.1:" << port << ": " << error << "\n";
        }
    }
    std::size_t received = 0;
    for (auto& [port, socket] : inputs_) {
        for (std::size_t n = 0; n < maxDatagramsPerPump; ++n) {
            const auto d = socket.receive(0);
            if (!d) break;
            ++received;
            if (const auto p = primitives::oscPacket::of(d->bytes, now)) {
                io.deliverOsc(port, *p);
            } else {
                io.countDrop();
            }
        }
    }
    io.drainOscOut([&](std::uint16_t port, const primitives::oscPacket& p) {
        // Packets for the page port go to pages, so they never loop back
        // into the server's own UDP socket on it (io.md §12.3).
        if (pagePort && port == *pagePort) {
            sendPages(io, p);
            return;
        }
        if (!output_) {
            std::string error;
            output_ = openOutput_(error);
            if (!output_ && !outputFailed_) {
                errors << "espresso-server: cannot open a socket for OSC output: " << error << "\n";
                outputFailed_ = true;
            }
        }
        if (!output_ || !output_->send(std::span<const std::byte>(p.bytes).first(p.size), {port})) {
            io.countDrop();
            return;
        }
        if (log) log->get() << "osc -> :" << port << " (" << p.size << " bytes)\n" << std::flush;
    });
    return received;
}

} // namespace server
