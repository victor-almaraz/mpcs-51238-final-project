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

namespace server {

std::size_t oscBridge::pump(primitives::ioHub& io, double now, std::ostream& errors,
                            std::optional<std::reference_wrapper<std::ostream>> log,
                            const std::function<bool(std::uint16_t, const primitives::oscPacket&)>& divert) {
    const auto wanted = io.oscInputPorts();
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
        if (divert && divert(port, p)) return;
        if (!output_) {
            std::string error;
            output_ = protocol::udpSocket::bind(0, error);
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
