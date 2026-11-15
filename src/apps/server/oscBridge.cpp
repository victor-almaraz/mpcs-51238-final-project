#include "oscBridge.h"

#include <algorithm>
#include <span>
#include <string>

namespace server {

std::size_t oscBridge::pump(primitives::ioHub& io, double now, std::ostream& errors,
                            std::optional<std::reference_wrapper<std::ostream>> log) {
    const auto wanted = io.oscInputPorts();
    for (auto it = inputs_.begin(); it != inputs_.end();) {
        it = std::find(wanted.begin(), wanted.end(), it->first) == wanted.end() ? inputs_.erase(it) : std::next(it);
    }
    for (auto port : wanted) {
        if (inputs_.count(port)) continue;
        std::string error;
        if (auto s = protocol::udpSocket::bind(port, error)) {
            inputs_.emplace(port, std::move(*s));
            failed_.erase(port);  // report again if it fails later
        } else if (failed_.insert(port).second) {
            errors << "proj-server: cannot listen for OSC on 127.0.0.1:" << port << ": " << error << "\n";
        }
    }
    std::size_t received = 0;
    for (auto& [port, socket] : inputs_) {
        for (std::size_t n = 0; n < maxDatagramsPerPump; ++n) {
            const auto d = socket.receive(0);
            if (!d) break;
            ++received;
            primitives::oscPacket p;
            if (d->bytes.size() > p.bytes.size()) {
                io.countDrop();
                continue;
            }
            p.time = now;
            p.size = static_cast<std::uint16_t>(d->bytes.size());
            std::transform(d->bytes.begin(), d->bytes.end(), p.bytes.begin(), [](std::uint8_t b) { return std::byte{b}; });
            io.deliverOsc(port, p);
        }
    }
    io.drainOscOut([&](std::uint16_t port, const primitives::oscPacket& p) {
        if (!output_) {
            std::string error;
            output_ = protocol::udpSocket::bind(0, error);
            if (!output_ && !outputFailed_) {
                errors << "proj-server: cannot open a socket for OSC output: " << error << "\n";
                outputFailed_ = true;
            }
        }
        if (!output_ || !output_->send(std::span<const std::byte>(p.bytes).first(p.size), {port})) {
            io.countDrop();
            return;
        }
        if (log) log->get() << "osc -> :" << port << " (" << p.size << " bytes)" << std::endl;
    });
    return received;
}

} // namespace server
