// Port registry between primitives and the I/O layer (primitives.md §7.2, §7.3).

#include <proj/primitives/io.h>

#include <algorithm>
#include <cctype>

namespace primitives {

template <typename Port, typename... Args>
std::shared_ptr<Port> ioHub::open(Args&&... args) {
    auto port = std::make_shared<Port>(portKey{}, std::forward<Args>(args)...);
    const std::lock_guard lock(mutex_);
    std::erase_if(ports_, [](const std::weak_ptr<ioPort>& p) { return p.expired(); });
    ports_.push_back(port);
    return port;
}

std::shared_ptr<midiPort> ioHub::openMidiIn(std::string device) { return open<midiPort>(portKind::midiIn, std::move(device)); }
std::shared_ptr<midiPort> ioHub::openMidiOut(std::string device) { return open<midiPort>(portKind::midiOut, std::move(device)); }
std::shared_ptr<oscPort> ioHub::openOscIn(std::uint16_t port) { return open<oscPort>(portKind::oscIn, port); }
std::shared_ptr<oscPort> ioHub::openOscOut(std::uint16_t port) { return open<oscPort>(portKind::oscOut, port); }

template <typename Port, typename Visit>
void ioHub::forEach(portKind kind, Visit&& visit) const {
    const std::lock_guard lock(mutex_);
    std::erase_if(ports_, [](const std::weak_ptr<ioPort>& p) { return p.expired(); });
    for (const auto& weak : ports_) {
        // A port released meanwhile by its plan is destroyed here, on this
        // thread, when `port` goes out of scope; its destructor does not
        // touch the hub.
        if (const auto port = weak.lock(); port && port->kind() == kind) visit(static_cast<Port&>(*port));
    }
}

bool ioHub::matches(std::string_view device, std::string_view deviceName) noexcept {
    if (device.empty()) return true;
    const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    const auto it = std::search(deviceName.begin(), deviceName.end(), device.begin(), device.end(),
                                [&](char a, char b) { return lower(a) == lower(b); });
    return it != deviceName.end();
}

void ioHub::deliverMidi(std::string_view deviceName, const midiPacket& packet) {
    forEach<midiPort>(portKind::midiIn, [&](midiPort& p) {
        if (p.active() && matches(p.device(), deviceName) && !p.queue.push(packet)) countDrop();
    });
}

void ioHub::deliverOsc(std::uint16_t port, const oscPacket& packet) {
    forEach<oscPort>(portKind::oscIn, [&](oscPort& p) {
        if (p.active() && p.udpPort() == port && !p.queue.push(packet)) countDrop();
    });
}

void ioHub::drainMidiOut(const std::function<void(std::string_view, const midiPacket&)>& send) {
    forEach<midiPort>(portKind::midiOut, [&](midiPort& p) {
        midiPacket packet;
        while (p.queue.pop(packet)) send(p.device(), packet);
    });
}

void ioHub::drainOscOut(const std::function<void(std::uint16_t, const oscPacket&)>& send) {
    forEach<oscPort>(portKind::oscOut, [&](oscPort& p) {
        oscPacket packet;
        while (p.queue.pop(packet)) send(p.udpPort(), packet);
    });
}

void ioHub::reportFault(std::string_view node, std::string_view what) noexcept {
    fault f;
    std::copy_n(node.begin(), std::min(node.size(), f.node.size() - 1), f.node.begin());
    std::copy_n(what.begin(), std::min(what.size(), f.what.size() - 1), f.what.begin());
    faultCount_.fetch_add(1, std::memory_order_relaxed);
    faults_.push(f);
}

std::vector<std::string> ioHub::takeFaults() {
    // The fixed fields are terminated by their first zero.
    auto text = [](const auto& field) { return std::string(field.begin(), std::find(field.begin(), field.end(), '\0')); };
    std::vector<std::string> out;
    fault f;
    while (faults_.pop(f)) out.push_back(text(f.node) + ": " + text(f.what));
    return out;
}

std::vector<std::uint16_t> ioHub::oscInputPorts() const {
    std::vector<std::uint16_t> out;
    forEach<oscPort>(portKind::oscIn, [&](const oscPort& p) { out.push_back(p.udpPort()); });
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace primitives
