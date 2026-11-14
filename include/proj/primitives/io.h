/// @file
/// Queues between sources and sinks and the I/O layer
/// (`primitives.md` §1.2, §7.2, §7.3). Primitives never touch devices or
/// sockets. A source owns an input port that an I/O thread fills; a sink
/// owns an output port that an I/O thread drains. The hub tracks the
/// ports so the I/O layer can find them.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <proj/primitives/spscQueue.h>

namespace primitives {

/// A MIDI message with its time on the device clock.
struct midiPacket {
    /// Receive or send time, in seconds on the device clock.
    double time = 0.0;
    /// Bytes used, 1 to 3.
    std::uint8_t size = 0;
    /// Status and data bytes.
    std::array<std::uint8_t, 3> bytes{};
};

/// Largest OSC message a port carries.
inline constexpr std::size_t maxOscBytes = 1024;

/// An OSC message with its time on the device clock.
struct oscPacket {
    /// Receive or send time, in seconds on the device clock.
    double time = 0.0;
    /// Bytes used.
    std::uint16_t size = 0;
    /// The encoded OSC message.
    std::array<std::byte, maxOscBytes> bytes{};
};

/// A signal fault reported by a sink on the audio thread, such as
/// non-finite samples. Fixed-size so reporting does not allocate.
struct fault {
    /// Node ID of the sink, truncated.
    std::array<char, 160> node{};
    /// What happened, truncated.
    std::array<char, 96> what{};
};

class ioHub;

/// What a port connects to.
enum class portKind : std::uint8_t {
    midiIn,   ///< MIDI input device
    midiOut,  ///< MIDI output device
    oscIn,    ///< OSC messages received on a UDP port
    oscOut,   ///< OSC messages sent to a UDP port
};

/// Proof that a port is made by its hub: only ioHub can create one.
class portKey {
    friend class ioHub;
    portKey() = default;
};

/// A queue between one primitive and the I/O layer. Shared by the
/// primitive and its plan; the hub keeps only a weak reference, so a port
/// is gone when its plan is. Input ports accept data only while active,
/// so a plan that is loaded but not yet playing does not accumulate stale
/// messages.
class ioPort {
public:
    virtual ~ioPort() = default;

    ioPort(const ioPort&) = delete;
    ioPort& operator=(const ioPort&) = delete;

    /// What the port connects to.
    portKind kind() const noexcept { return kind_; }
    /// MIDI device name pattern; empty matches every device.
    const std::string& device() const noexcept { return device_; }
    /// UDP port, for OSC ports.
    std::uint16_t udpPort() const noexcept { return udpPort_; }

    /// Activates or deactivates the port (audio thread, at a swap).
    void setActive(bool on) noexcept { active_.store(on, std::memory_order_release); }
    /// True while the port's plan is running.
    bool active() const noexcept { return active_.load(std::memory_order_acquire); }

protected:
    /// A port of @p kind for @p device or @p udpPort.
    ioPort(portKind kind, std::string device, std::uint16_t udpPort)
        : kind_(kind), device_(std::move(device)), udpPort_(udpPort) {}

private:
    portKind kind_;
    std::string device_;
    std::uint16_t udpPort_;
    std::atomic<bool> active_{false};
};

/// A port carrying MIDI packets.
class midiPort final : public ioPort {
public:
    /// Made by ioHub only.
    midiPort(portKey, portKind kind, std::string device) : ioPort(kind, std::move(device), 0) {}

    /// The queue: filled by the I/O thread for inputs, by the sink for
    /// outputs.
    spscQueue<midiPacket> queue{1024};
};

/// A port carrying OSC packets.
class oscPort final : public ioPort {
public:
    /// Made by ioHub only.
    oscPort(portKey, portKind kind, std::uint16_t udpPort) : ioPort(kind, {}, udpPort) {}

    /// The queue: filled by the I/O thread for inputs, by the sink for
    /// outputs.
    spscQueue<oscPacket> queue{256};
};

/// Registry of ports. Opening ports happens on the builder thread;
/// delivery and draining happen on I/O threads. All of them lock the hub;
/// the audio thread never does.
class ioHub {
public:
    /// Opens a MIDI input for devices matching @p device.
    std::shared_ptr<midiPort> openMidiIn(std::string device);
    /// Opens a MIDI output for the device matching @p device.
    std::shared_ptr<midiPort> openMidiOut(std::string device);
    /// Opens an OSC input on UDP port @p port.
    std::shared_ptr<oscPort> openOscIn(std::uint16_t port);
    /// Opens an OSC output to UDP port @p port.
    std::shared_ptr<oscPort> openOscOut(std::uint16_t port);

    /// I/O thread: queues @p packet for every active MIDI input matching
    /// @p deviceName. Full queues drop it and count a drop.
    void deliverMidi(std::string_view deviceName, const midiPacket& packet);
    /// I/O thread: queues @p packet for every active OSC input on @p port.
    void deliverOsc(std::uint16_t port, const oscPacket& packet);

    /// I/O thread: passes every queued MIDI output packet to @p send with
    /// the port's device pattern.
    void drainMidiOut(const std::function<void(std::string_view device, const midiPacket&)>& send);
    /// I/O thread: passes every queued OSC output packet to @p send with
    /// its destination port.
    void drainOscOut(const std::function<void(std::uint16_t port, const oscPacket&)>& send);

    /// UDP ports that OSC inputs listen on, sorted and unique.
    std::vector<std::uint16_t> oscInputPorts() const;

    /// Audio thread: reports a fault in node @p node. Real-time safe; a
    /// full queue drops the report but still counts it.
    void reportFault(std::string_view node, std::string_view what) noexcept;
    /// Control thread: removes the reported faults, each as
    /// `node ID: what`.
    std::vector<std::string> takeFaults();
    /// Faults reported so far.
    std::uint64_t faults() const noexcept { return faultCount_.load(std::memory_order_relaxed); }

    /// Counts a dropped packet (full queue). Real-time safe.
    void countDrop() noexcept { drops_.fetch_add(1, std::memory_order_relaxed); }
    /// Packets dropped by full queues.
    std::uint64_t drops() const noexcept { return drops_.load(std::memory_order_relaxed); }

    /// True when the pattern @p device selects @p deviceName: empty
    /// matches everything; otherwise a case-insensitive substring match.
    static bool matches(std::string_view device, std::string_view deviceName) noexcept;

private:
    // Calls @p visit with each live port of @p kind, as its concrete type,
    // holding the hub lock. A port whose plan is gone is dropped.
    template <typename Port, typename Visit>
    void forEach(portKind kind, Visit&& visit) const;

    template <typename Port, typename... Args>
    std::shared_ptr<Port> open(Args&&... args);

    mutable std::mutex mutex_;
    mutable std::vector<std::weak_ptr<ioPort>> ports_;
    std::atomic<std::uint64_t> drops_{0};
    spscQueue<fault> faults_{64};  // audio thread to control thread
    std::atomic<std::uint64_t> faultCount_{0};
};

} // namespace primitives
