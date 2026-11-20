// Port registry between primitives and the I/O layer (primitives.md §7.2, §7.3).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/primitives/io.h>
#include <espresso/primitives/realtime.h>

namespace primitives {

template <typename Port, typename... Args>
std::shared_ptr<Port> ioHub::open(Args&&... args) {
    auto port = std::make_shared<Port>(portKey{}, std::forward<Args>(args)...);
    const std::scoped_lock lock(mutex_);
    std::erase_if(ports_, [](const std::weak_ptr<ioPort>& p) { return p.expired(); });
    ports_.push_back(port);
    return port;
}

std::shared_ptr<midiPort> ioHub::openMidiIn(std::string owner, std::string device) {
    return open<midiPort>(portKind::midiIn, std::move(owner), std::move(device));
}
std::shared_ptr<midiPort> ioHub::openMidiOut(std::string device) {
    return open<midiPort>(portKind::midiOut, std::string{}, std::move(device));
}
std::shared_ptr<oscPort> ioHub::openOscIn(std::string owner, std::uint16_t port) {
    return open<oscPort>(portKind::oscIn, std::move(owner), port);
}
std::shared_ptr<oscPort> ioHub::openOscOut(std::uint16_t port) {
    return open<oscPort>(portKind::oscOut, std::string{}, port);
}

std::shared_ptr<serialPort> ioHub::openSerialIn(std::string owner, std::string device, std::uint32_t baud) {
    return open<serialPort>(portKind::serialIn, std::move(owner), std::move(device), baud);
}
std::shared_ptr<serialPort> ioHub::openSerialOut(std::string device, std::uint32_t baud) {
    return open<serialPort>(portKind::serialOut, std::string{}, std::move(device), baud);
}

std::shared_ptr<webPort> ioHub::openWebIn(portKind kind, std::string owner, webRequest request) {
    return open<webPort>(kind, std::move(owner), std::move(request));
}

std::shared_ptr<savePort> ioHub::openSave(std::string owner, std::string bufferId, std::string file,
                                          int channels, std::size_t frames, double sampleRate) {
    return open<savePort>(std::move(owner), std::move(bufferId), std::move(file), channels, frames,
                          sampleRate);
}

savePort::savePort(portKey, std::string owner, std::string bufferId, std::string file, int channels,
                   std::size_t frames, double sampleRate)
    : ioPort(portKind::save, std::move(owner), {}, 0),
      bufferId_(std::move(bufferId)),
      file_(std::move(file)),
      snapshot_(channels, frames, sampleRate) {}

// Only the audio thread leaves idle and copying; only the control thread
// leaves ready and writing. Each hand-over releases what its side wrote.
bool savePort::begin() noexcept ESPRESSO_NONBLOCKING {
    if (state_.load(std::memory_order_acquire) != state::idle) return false;
    state_.store(state::copying, std::memory_order_relaxed);
    return true;
}

void savePort::finish() noexcept ESPRESSO_NONBLOCKING {
    state_.store(state::ready, std::memory_order_release);
}

void savePort::writeReady(const writer& write) {
    state expected = state::ready;
    if (!state_.compare_exchange_strong(expected, state::writing, std::memory_order_acquire)) return;
    try {
        write(*this, snapshot_);
    } catch (...) {
        state_.store(state::idle, std::memory_order_release);
        throw;
    }
    state_.store(state::idle, std::memory_order_release);
}

template <typename Port, typename Visit>
void ioHub::forEachLocked(portKind kind, Visit&& visit) const {
    std::erase_if(ports_, [](const std::weak_ptr<ioPort>& p) { return p.expired(); });
    // Ports are appended when opened, so this visits the oldest first.
    for (const auto& weak : ports_) {
        // A port released meanwhile by its plan is destroyed here, on this
        // thread, when `port` goes out of scope; its destructor does not
        // touch the hub.
        if (const auto port = weak.lock(); port && port->kind() == kind)
            visit(std::static_pointer_cast<Port>(port));
    }
}

template <typename Port>
std::vector<std::shared_ptr<Port>> ioHub::live(portKind kind) const {
    std::vector<std::shared_ptr<Port>> out;
    const std::scoped_lock lock(mutex_);
    forEachLocked<Port>(kind, [&](std::shared_ptr<Port> p) { out.push_back(std::move(p)); });
    return out;
}

template <typename Port, typename Packet, typename Accepts>
void ioHub::deliver(portKind kind, const Packet& packet, Accepts&& accepts) {
    // During a swap an adopting node activates its port before the engine
    // deactivates the port of the node it replaces, so a node may have two
    // active ports. The older one gets the packet; the adopting node reads
    // both, so a packet is neither lost nor doubled.
    std::vector<std::string_view> reached;
    const std::scoped_lock lock(mutex_);
    forEachLocked<Port>(kind, [&](const std::shared_ptr<Port>& p) {
        // Started before the active check: the audio thread keeps draining
        // a port that a delivery may still push into (ioPort::closed).
        const ioPort::delivery d(*p);
        if (!d.admitted() || !accepts(*p)) return;
        if (std::find(reached.begin(), reached.end(), p->owner()) != reached.end()) return;
        reached.push_back(p->owner());
        if (!p->queue.push(packet)) countDrop();
    });
}

template <typename Port, typename Packet, typename Send>
void ioHub::drain(portKind kind, Send&& send) {
    // The lock covers finding the ports only: @p send may call the hub.
    for (const auto& p : live<Port>(kind)) {
        Packet packet;
        while (p->queue.pop(packet)) send(*p, packet);
    }
}

bool ioHub::matches(std::string_view device, std::string_view deviceName) noexcept {
    if (device.empty()) return true;
    const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    return !std::ranges::search(deviceName, device, [&](char a, char b) {
                return lower(a) == lower(b);
            }).empty();
}

void ioHub::deliverMidi(std::string_view deviceName, const midiPacket& packet) {
    deliver<midiPort>(portKind::midiIn, packet,
                      [&](const midiPort& p) { return matches(p.device(), deviceName); });
}

void ioHub::deliverOsc(std::uint16_t port, const oscPacket& packet) {
    deliver<oscPort>(portKind::oscIn, packet, [&](const oscPort& p) { return p.udpPort() == port; });
}

void ioHub::deliverSerial(std::string_view device, const dataFrame& frame) {
    deliver<serialPort>(portKind::serialIn, frame, [&](const serialPort& p) { return p.device() == device; });
}

void ioHub::deliverWeb(portKind kind, std::string_view owner, const dataFrame& frame) {
    deliver<webPort>(kind, frame, [&](const webPort& p) { return p.owner() == owner; });
}

void ioHub::drainMidiOut(const std::function<void(std::string_view, const midiPacket&)>& send) {
    drain<midiPort, midiPacket>(
        portKind::midiOut, [&](const midiPort& p, const midiPacket& packet) { send(p.device(), packet); });
}

void ioHub::drainOscOut(const std::function<void(std::uint16_t, const oscPacket&)>& send) {
    drain<oscPort, oscPacket>(portKind::oscOut,
                              [&](const oscPort& p, const oscPacket& packet) { send(p.udpPort(), packet); });
}

void ioHub::drainSerialOut(const std::function<void(std::string_view, const dataFrame&)>& send) {
    drain<serialPort, dataFrame>(
        portKind::serialOut, [&](const serialPort& p, const dataFrame& frame) { send(p.device(), frame); });
}

void ioHub::drainSaves(const savePort::writer& write) {
    for (const auto& p : live<savePort>(portKind::save)) p->writeReady(write);
}

void ioHub::reportFault(std::string_view node, std::string_view what) noexcept ESPRESSO_NONBLOCKING {
    fault f;
    std::copy_n(node.begin(), std::min(node.size(), f.node.size() - 1), f.node.begin());
    std::copy_n(what.begin(), std::min(what.size(), f.what.size() - 1), f.what.begin());
    // Counted whether or not the report fits.
    faultCount_.fetch_add(1, std::memory_order_relaxed);
    if (!faults_.push(f)) {
        // The queue is full: the count above records the fault; its text is lost.
    }
}

std::vector<std::string> ioHub::takeFaults() {
    // The fixed fields are terminated by their first zero.
    auto text = [](const auto& field) { return std::string(field.begin(), std::find(field.begin(), field.end(), '\0')); };
    std::vector<std::string> out;
    fault f;
    while (faults_.pop(f)) out.push_back(text(f.node) + ": " + text(f.what));
    return out;
}

std::vector<std::shared_ptr<const webPort>> ioHub::webInputs(portKind kind) const {
    const auto ports = live<webPort>(kind);
    return {ports.begin(), ports.end()};
}

std::vector<std::uint16_t> ioHub::oscInputPorts() const {
    std::vector<std::uint16_t> out;
    for (const auto& p : live<oscPort>(portKind::oscIn)) out.push_back(p->udpPort());
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    return out;
}

} // namespace primitives
