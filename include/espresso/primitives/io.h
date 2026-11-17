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
#include <utility>
#include <vector>

#include <espresso/primitives/realtime.h>
#include <espresso/primitives/spscQueue.h>

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
/// messages. An input port names the node that reads it: the hub delivers
/// each message to one active port per node, so an adopting node and the
/// node it replaces, both active during a swap, never receive it twice.
class ioPort {
public:
    virtual ~ioPort() = default;

    ioPort(const ioPort&) = delete;
    ioPort& operator=(const ioPort&) = delete;

    /// What the port connects to.
    portKind kind() const noexcept { return kind_; }
    /// Node ID of the source reading an input port; empty for outputs.
    const std::string& owner() const noexcept { return owner_; }
    /// MIDI device name pattern; empty matches every device.
    const std::string& device() const noexcept { return device_; }
    /// UDP port, for OSC ports.
    std::uint16_t udpPort() const noexcept { return udpPort_; }

    /// Activates or deactivates the port (audio thread, at a swap).
    void setActive(bool on) noexcept ESPRESSO_NONBLOCKING { active_.store(on, std::memory_order_seq_cst); }
    /// True while the port's plan is running.
    bool active() const noexcept { return active_.load(std::memory_order_seq_cst); }

    /// A delivery in progress (hub thread), from before it reads whether
    /// the port is active until after it pushes. The audio thread may
    /// deactivate the port in between; closed() tells it when no delivery
    /// can still push.
    class delivery {
    public:
        /// Starts a delivery to @p port.
        explicit delivery(ioPort& port) noexcept : port_(port) {
            port_.delivering_.fetch_add(1, std::memory_order_seq_cst);
        }
        ~delivery() { port_.delivering_.fetch_sub(1, std::memory_order_seq_cst); }

        delivery(const delivery&) = delete;
        delivery& operator=(const delivery&) = delete;
        delivery(delivery&&) = delete;
        delivery& operator=(delivery&&) = delete;

        /// True when the port was active once the delivery had started.
        bool admitted() const noexcept { return port_.active(); }

    private:
        ioPort& port_;
    };

    /// True when no delivery can push into the port any more: it is
    /// inactive and none is in progress. A delivery that starts later
    /// reads the port inactive and pushes nothing (sequentially consistent
    /// order).
    bool closed() const noexcept ESPRESSO_NONBLOCKING {
        return !active() && delivering_.load(std::memory_order_seq_cst) == 0;
    }

protected:
    /// A port of @p kind for @p device or @p udpPort, read by node @p owner.
    ioPort(portKind kind, std::string owner, std::string device, std::uint16_t udpPort)
        : kind_(kind), owner_(std::move(owner)), device_(std::move(device)), udpPort_(udpPort) {}

private:
    portKind kind_ = portKind::midiIn;
    std::string owner_;
    std::string device_;
    std::uint16_t udpPort_ = 0;
    std::atomic<bool> active_{false};
    std::atomic<int> delivering_{0};
};

/// A port carrying MIDI packets.
class midiPort final : public ioPort {
public:
    /// Made by ioHub only.
    midiPort(portKey, portKind kind, std::string owner, std::string device)
        : ioPort(kind, std::move(owner), std::move(device), 0) {}

    /// The queue: filled by the I/O thread for inputs, by the sink for
    /// outputs.
    spscQueue<midiPacket> queue{1024};
};

/// A port carrying OSC packets.
class oscPort final : public ioPort {
public:
    /// Made by ioHub only.
    oscPort(portKey, portKind kind, std::string owner, std::uint16_t udpPort)
        : ioPort(kind, std::move(owner), {}, udpPort) {}

    /// The queue: filled by the I/O thread for inputs, by the sink for
    /// outputs.
    spscQueue<oscPacket> queue{256};
};

/// Registry of ports. Opening ports happens on the builder thread;
/// delivery and draining happen on I/O threads. They lock the hub to find
/// ports, never while calling back; the audio thread never locks it.
class ioHub {
public:
    /// Opens a MIDI input for devices matching @p device, read by node
    /// @p owner.
    std::shared_ptr<midiPort> openMidiIn(std::string owner, std::string device);
    /// Opens a MIDI output for the device matching @p device.
    std::shared_ptr<midiPort> openMidiOut(std::string device);
    /// Opens an OSC input on UDP port @p port, read by node @p owner.
    std::shared_ptr<oscPort> openOscIn(std::string owner, std::uint16_t port);
    /// Opens an OSC output to UDP port @p port.
    std::shared_ptr<oscPort> openOscOut(std::uint16_t port);

    /// I/O thread: queues @p packet for each node with an active MIDI
    /// input matching @p deviceName, in that node's oldest active port.
    /// Full queues drop it and count a drop. Deliveries hold the hub lock,
    /// so each queue has one producer at a time.
    void deliverMidi(std::string_view deviceName, const midiPacket& packet);
    /// I/O thread: queues @p packet for each node with an active OSC input
    /// on @p port, as deliverMidi() does.
    void deliverOsc(std::uint16_t port, const oscPacket& packet);

    /// I/O thread: passes every queued MIDI output packet to @p send with
    /// the port's device pattern. Called from one thread at a time; @p send
    /// runs without the hub lock and may call the hub.
    void drainMidiOut(const std::function<void(std::string_view device, const midiPacket&)>& send);
    /// I/O thread: passes every queued OSC output packet to @p send with
    /// its destination port. Called from one thread at a time; @p send runs
    /// without the hub lock and may call the hub.
    void drainOscOut(const std::function<void(std::uint16_t port, const oscPacket&)>& send);

    /// UDP ports that OSC inputs listen on, sorted and unique.
    std::vector<std::uint16_t> oscInputPorts() const;

    /// Audio thread: reports a fault in node @p node. Real-time safe; a
    /// full queue drops the report but still counts it.
    void reportFault(std::string_view node, std::string_view what) noexcept ESPRESSO_NONBLOCKING;
    /// Control thread: removes the reported faults, each as
    /// `node ID: what`.
    std::vector<std::string> takeFaults();
    /// Faults reported so far.
    std::uint64_t faults() const noexcept { return faultCount_.load(std::memory_order_relaxed); }

    /// Counts a dropped packet (full queue). Real-time safe.
    void countDrop() noexcept ESPRESSO_NONBLOCKING { drops_.fetch_add(1, std::memory_order_relaxed); }
    /// Packets dropped by full queues.
    std::uint64_t drops() const noexcept { return drops_.load(std::memory_order_relaxed); }

    /// True when the pattern @p device selects @p deviceName: empty
    /// matches everything; otherwise a case-insensitive substring match.
    static bool matches(std::string_view device, std::string_view deviceName) noexcept;

private:
    // Calls @p visit with each live port of @p kind, as its concrete type,
    // oldest first. The caller holds the hub lock. A port whose plan is
    // gone is dropped.
    template <typename Port, typename Visit>
    void forEachLocked(portKind kind, Visit&& visit) const;
    // The live ports of @p kind, found under the hub lock.
    template <typename Port>
    std::vector<std::shared_ptr<Port>> live(portKind kind) const;
    // Pushes @p packet into the oldest active port of each owner among the
    // ports of @p kind that @p accepts.
    template <typename Port, typename Packet, typename Accepts>
    void deliver(portKind kind, const Packet& packet, Accepts&& accepts);
    // Pops every packet of each live port of @p kind into @p send.
    template <typename Port, typename Packet, typename Send>
    void drain(portKind kind, Send&& send);

    template <typename Port, typename... Args>
    std::shared_ptr<Port> open(Args&&... args);

    spscQueue<fault> faults_{64};  // audio thread to control thread
    std::atomic<std::uint64_t> drops_{0};
    std::atomic<std::uint64_t> faultCount_{0};
    mutable std::vector<std::weak_ptr<ioPort>> ports_;
    mutable std::mutex mutex_;
};

}  // namespace primitives
