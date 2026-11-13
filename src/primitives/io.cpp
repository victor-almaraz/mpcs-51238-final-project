// Port registry between primitives and the I/O layer (primitives.md §7.2, §7.3).

#include <proj/primitives/io.h>

#include <algorithm>
#include <cctype>

namespace primitives {

ioPort::ioPort(ioHub& hub, portKind kind, std::string device, std::uint16_t udpPort)
    : hub_(hub), kind_(kind), device_(std::move(device)), udpPort_(udpPort) {}

ioPort::~ioPort() { hub_.remove(this); }

void ioHub::add(ioPort* port) {
    const std::lock_guard lock(mutex_);
    ports_.push_back(port);
}

void ioHub::remove(ioPort* port) {
    const std::lock_guard lock(mutex_);
    ports_.erase(std::remove(ports_.begin(), ports_.end(), port), ports_.end());
}

std::unique_ptr<midiPort> ioHub::openMidiIn(std::string device) {
    std::unique_ptr<midiPort> p(new midiPort(*this, portKind::midiIn, std::move(device), 0));
    add(p.get());
    return p;
}

std::unique_ptr<midiPort> ioHub::openMidiOut(std::string device) {
    std::unique_ptr<midiPort> p(new midiPort(*this, portKind::midiOut, std::move(device), 0));
    add(p.get());
    return p;
}

std::unique_ptr<oscPort> ioHub::openOscIn(std::uint16_t port) {
    std::unique_ptr<oscPort> p(new oscPort(*this, portKind::oscIn, {}, port));
    add(p.get());
    return p;
}

std::unique_ptr<oscPort> ioHub::openOscOut(std::uint16_t port) {
    std::unique_ptr<oscPort> p(new oscPort(*this, portKind::oscOut, {}, port));
    add(p.get());
    return p;
}

bool ioHub::matches(std::string_view device, std::string_view deviceName) noexcept {
    if (device.empty()) return true;
    const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    const auto it = std::search(deviceName.begin(), deviceName.end(), device.begin(), device.end(),
                                [&](char a, char b) { return lower(a) == lower(b); });
    return it != deviceName.end();
}

void ioHub::deliverMidi(std::string_view deviceName, const midiPacket& packet) {
    const std::lock_guard lock(mutex_);
    for (ioPort* p : ports_) {
        if (p->kind() != portKind::midiIn || !p->active() || !matches(p->device(), deviceName)) continue;
        if (!static_cast<midiPort*>(p)->queue.push(packet)) countDrop();
    }
}

void ioHub::deliverOsc(std::uint16_t port, const oscPacket& packet) {
    const std::lock_guard lock(mutex_);
    for (ioPort* p : ports_) {
        if (p->kind() != portKind::oscIn || !p->active() || p->udpPort() != port) continue;
        if (!static_cast<oscPort*>(p)->queue.push(packet)) countDrop();
    }
}

void ioHub::drainMidiOut(const std::function<void(std::string_view, const midiPacket&)>& send) {
    const std::lock_guard lock(mutex_);
    midiPacket packet;
    for (ioPort* p : ports_) {
        if (p->kind() != portKind::midiOut) continue;
        while (static_cast<midiPort*>(p)->queue.pop(packet)) send(p->device(), packet);
    }
}

void ioHub::drainOscOut(const std::function<void(std::uint16_t, const oscPacket&)>& send) {
    const std::lock_guard lock(mutex_);
    oscPacket packet;
    for (ioPort* p : ports_) {
        if (p->kind() != portKind::oscOut) continue;
        while (static_cast<oscPort*>(p)->queue.pop(packet)) send(p->udpPort(), packet);
    }
}

std::vector<std::uint16_t> ioHub::oscInputPorts() const {
    const std::lock_guard lock(mutex_);
    std::vector<std::uint16_t> out;
    for (const ioPort* p : ports_) {
        if (p->kind() == portKind::oscIn) out.push_back(p->udpPort());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace primitives
