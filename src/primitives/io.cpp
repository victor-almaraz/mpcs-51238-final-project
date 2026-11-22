// Port registry between primitives and the I/O layer (primitives.md §7.2, §7.3).

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/primitives/event.h>
#include <espresso/primitives/io.h>
#include <espresso/primitives/realtime.h>

namespace primitives {

// --- oscQueue ----------------------------------------------------------------

oscQueue::oscQueue(std::size_t bytes)
    : ring_(std::bit_ceil(std::max(bytes, header + maxOscBytes))), mask_(ring_.size() - 1) {}

void oscQueue::put(std::size_t at, std::span<const std::byte> from) noexcept ESPRESSO_NONBLOCKING {
    const std::size_t start = at & mask_;
    const std::size_t first = std::min(from.size(), ring_.size() - start);
    std::ranges::copy(from.first(first), ring_.begin() + static_cast<std::ptrdiff_t>(start));
    std::ranges::copy(from.subspan(first), ring_.begin());
}

void oscQueue::get(std::size_t at, std::span<std::byte> to) const noexcept ESPRESSO_NONBLOCKING {
    const std::size_t start = at & mask_;
    const std::size_t first = std::min(to.size(), ring_.size() - start);
    const auto from = std::span(ring_);
    std::ranges::copy(from.subspan(start, first), to.begin());
    std::ranges::copy(from.first(to.size() - first), to.begin() + static_cast<std::ptrdiff_t>(first));
}

bool oscQueue::push(const oscPacket& packet) noexcept ESPRESSO_NONBLOCKING {
    const auto bytes = packet.used();
    const std::size_t length = header + bytes.size();
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_.load(std::memory_order_acquire) + length > ring_.size()) return false;
    const auto time = std::bit_cast<std::array<std::byte, sizeof(double)>>(packet.time);
    const auto size =
        std::bit_cast<std::array<std::byte, sizeof(std::uint16_t)>>(static_cast<std::uint16_t>(bytes.size()));
    put(tail, time);
    put(tail + time.size(), size);
    put(tail + header, bytes);
    tail_.store(tail + length, std::memory_order_release);
    return true;
}

bool oscQueue::pop(oscPacket& out) noexcept ESPRESSO_NONBLOCKING {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire)) return false;
    std::array<std::byte, sizeof(double)> time{};
    std::array<std::byte, sizeof(std::uint16_t)> size{};
    get(head, time);
    get(head + time.size(), size);
    out.time = std::bit_cast<double>(time);
    // At most maxOscBytes: push() wrote no more.
    out.size = std::bit_cast<std::uint16_t>(size);
    get(head + header, std::span(out.bytes).first(out.used().size()));
    head_.store(head + header + out.size, std::memory_order_release);
    return true;
}

bool oscQueue::empty() const noexcept ESPRESSO_NONBLOCKING {
    return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_acquire);
}

template <typename Port, typename... Args>
std::shared_ptr<Port> ioHub::open(Args&&... args) {
    auto port = std::make_shared<Port>(portKey{generation_}, std::forward<Args>(args)...);
    const std::scoped_lock lock(mutex_);
    std::erase_if(ports_, [](const std::weak_ptr<ioPort>& p) { return p.expired(); });
    ports_.push_back(port);
    generation_->fetch_add(1, std::memory_order_seq_cst);  // after the port is in the list
    return port;
}

std::shared_ptr<midiPort> ioHub::openMidiIn(std::string owner, std::string device) {
    return open<midiPort>(portKind::midiIn, std::move(owner), std::move(device));
}
std::shared_ptr<midiPort> ioHub::openMidiOut(std::string device) {
    return open<midiPort>(portKind::midiOut, std::string{}, std::move(device));
}
std::shared_ptr<oscPort> ioHub::openOscIn(std::string owner, std::uint16_t port, std::string address) {
    return open<oscPort>(portKind::oscIn, std::move(owner), port, std::move(address));
}
std::shared_ptr<oscPort> ioHub::openOscOut(std::uint16_t port) {
    return open<oscPort>(portKind::oscOut, std::string{}, port);
}

std::shared_ptr<serialPort> ioHub::openSerialIn(std::string owner, std::string device, std::uint32_t baud) {
    return open<serialPort>(portKind::serialIn, std::move(owner), std::move(device), baud);
}
std::shared_ptr<serialPort> ioHub::openSerialOut(std::string owner, std::string device, std::uint32_t baud) {
    return open<serialPort>(portKind::serialOut, std::move(owner), std::move(device), baud);
}

std::shared_ptr<webPort> ioHub::openWebIn(portKind kind, std::string owner, webRequest request) {
    return open<webPort>(kind, std::move(owner), std::move(request));
}

std::shared_ptr<webPort> ioHub::openWebOut(std::string owner, webRequest request) {
    return open<webPort>(portKind::webOut, std::move(owner), std::move(request));
}

std::shared_ptr<savePort> ioHub::openSave(std::string owner, std::string bufferId, std::string file,
                                          int channels, std::size_t frames, double sampleRate) {
    return open<savePort>(std::move(owner), std::move(bufferId), std::move(file), channels, frames,
                          sampleRate);
}

savePort::savePort(portKey key, std::string owner, std::string bufferId, std::string file, int channels,
                   std::size_t frames, double sampleRate)
    : ioPort(std::move(key), portKind::save, std::move(owner), {}, 0),
      bufferId_(std::move(bufferId)),
      file_(std::move(file)),
      snapshot_(channels, frames, sampleRate) {}

// Only the thread that runs the node leaves idle and copying; only the
// control thread leaves ready and writing. Each hand-over releases what
// its side wrote.
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
        // thread, when `port` goes out of scope; its destructor takes no
        // lock (it only advances the generation, an atomic).
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
std::size_t ioHub::deliver(portKind kind, const Packet& packet, Accepts&& accepts) {
    // During a swap an adopting node activates its port before the engine
    // deactivates the port of the node it replaces, so a node may have two
    // active ports. The older one gets the packet; the adopting node reads
    // both, so a packet is neither lost nor doubled.
    std::vector<std::string_view> reached;
    const std::scoped_lock lock(mutex_);
    forEachLocked<Port>(kind, [&](const std::shared_ptr<Port>& p) {
        // Started before the active check: the thread that runs the node
        // keeps draining a port that a delivery may still push into
        // (ioPort::closed).
        const ioPort::delivery d(*p);
        if (!d.admitted() || !accepts(*p)) return;
        if (std::find(reached.begin(), reached.end(), p->owner()) != reached.end()) return;
        reached.push_back(p->owner());
        if (!p->queue.push(packet)) countDrop();
    });
    return reached.size();
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

std::size_t ioHub::deliverOsc(std::uint16_t port, const oscPacket& packet) {
    // Filtering by address here, not only in the node, keeps a message to
    // one of many inputs on a port, such as a page's controls, out of the
    // others' queues.
    const auto message = packet.used();
    return deliver<oscPort>(portKind::oscIn, packet, [&](const oscPort& p) {
        return p.udpPort() == port && (p.address().empty() || oscAddressIs(message, p.address()));
    });
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

void ioHub::drainSerialOut(const std::function<void(const serialPort&, const dataFrame&)>& send) {
    drain<serialPort, dataFrame>(portKind::serialOut, send);
}

void ioHub::drainWebOut(const std::function<void(const webPort&, const dataFrame&)>& send) {
    drain<webPort, dataFrame>(portKind::webOut, send);
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

std::vector<serialDevice> ioHub::serialDevices() const {
    std::vector<serialDevice> out;
    for (const auto kind : {portKind::serialIn, portKind::serialOut}) {
        for (const auto& p : live<serialPort>(kind)) {
            auto d = std::ranges::find(out, p->device(), &serialDevice::pattern);
            if (d == out.end()) {
                out.push_back({.pattern = p->device(), .baud = p->baud(), .readers = {}, .writers = {}});
                d = std::prev(out.end());
            }
            auto& nodes = kind == portKind::serialIn ? d->readers : d->writers;
            if (std::ranges::find(nodes, p->owner()) == nodes.end()) nodes.push_back(p->owner());
        }
    }
    std::ranges::sort(out, {}, &serialDevice::pattern);
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
