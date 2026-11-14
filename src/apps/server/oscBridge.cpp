#include "oscBridge.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace server {

void oscBridge::pump(primitives::ioHub& io, double now, std::ostream& errors, std::ostream* log) {
    const auto wanted = io.oscInputPorts();
    for (auto it = inputs_.begin(); it != inputs_.end();) {
        it = std::find(wanted.begin(), wanted.end(), it->first) == wanted.end() ? inputs_.erase(it) : std::next(it);
    }
    for (auto port : wanted) {
        if (inputs_.count(port)) continue;
        std::string error;
        if (auto s = protocol::udpSocket::bind(port, &error)) {
            inputs_.emplace(port, std::move(*s));
        } else if (failed_.insert(port).second) {
            errors << "proj-server: cannot listen for OSC on 127.0.0.1:" << port << ": " << error << "\n";
        }
    }
    for (auto& [port, socket] : inputs_) {
        while (auto d = socket.receive(0)) {
            primitives::oscPacket p;
            if (d->bytes.size() > p.bytes.size()) {
                io.countDrop();
                continue;
            }
            p.time = now;
            p.size = static_cast<std::uint16_t>(d->bytes.size());
            std::memcpy(p.bytes.data(), d->bytes.data(), d->bytes.size());
            io.deliverOsc(port, p);
        }
    }
    io.drainOscOut([&](std::uint16_t port, const primitives::oscPacket& p) {
        if (!output_) output_ = protocol::udpSocket::bind(0);
        if (output_) output_->send({reinterpret_cast<const std::uint8_t*>(p.bytes.data()), p.size}, {port});
        if (log) *log << "osc -> :" << port << " (" << p.size << " bytes)" << std::endl;
    });
}

} // namespace server
