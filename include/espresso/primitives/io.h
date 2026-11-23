/// @file
/// Queues between sources and sinks and the I/O layer
/// (`primitives.md` §1.2, §7.2, §7.3). Primitives never touch devices or
/// sockets. A source owns an input port that an I/O thread fills; a sink
/// owns an output port that an I/O thread drains. The hub tracks the
/// ports so the I/O layer can find them.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/buffer.h>
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

    /// The packet holding @p message at @p time; nothing when it is larger
    /// than maxOscBytes.
    static std::optional<oscPacket> of(std::span<const std::uint8_t> message, double time = 0.0) noexcept {
        if (message.size() > maxOscBytes) return std::nullopt;
        oscPacket p;
        p.time = time;
        p.size = static_cast<std::uint16_t>(message.size());
        std::ranges::transform(message, p.bytes.begin(), [](std::uint8_t b) { return std::byte{b}; });
        return p;
    }
    /// The bytes it holds: the first `size`, at most all of them.
    std::span<const std::byte> used() const noexcept {
        return std::span(bytes).first(std::min<std::size_t>(size, bytes.size()));
    }
};

/// A single-producer, single-consumer queue of OSC packets that keeps of
/// each only its time, its size, and the bytes it uses, one after another
/// in a ring (`primitives.md` §7.3.5). A message of a few dozen bytes
/// takes a few dozen, where a slot of oscPacket takes over 1 KB: a piece
/// of hundreds of OSC inputs, each with its own port, holds megabytes of
/// queues rather than a hundred. The ring is written at construction, so
/// neither end takes a page fault on its first use.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding): cache-line separation
class oscQueue {
public:
    /// A ring of at least @p bytes bytes, rounded up to a power of two.
    explicit oscQueue(std::size_t bytes);

    ~oscQueue() = default;
    oscQueue(const oscQueue&) = delete;
    oscQueue& operator=(const oscQueue&) = delete;
    oscQueue(oscQueue&&) = delete;
    oscQueue& operator=(oscQueue&&) = delete;

    /// Producer: appends @p packet's time, size, and used bytes (at most
    /// maxOscBytes, oscPacket::used). Returns false when the ring has no
    /// room for them.
    [[nodiscard]] bool push(const oscPacket& packet) noexcept ESPRESSO_NONBLOCKING;
    /// Consumer: moves the oldest packet's time, size, and bytes into
    /// @p out; its bytes past the size are left as they were. Returns
    /// false when the queue is empty.
    [[nodiscard]] bool pop(oscPacket& out) noexcept ESPRESSO_NONBLOCKING;
    /// Consumer: true when there is nothing to pop.
    bool empty() const noexcept ESPRESSO_NONBLOCKING;
    /// The most packets the ring can hold at once, all empty: a bound for
    /// a loop that drains it.
    std::size_t capacity() const noexcept ESPRESSO_NONBLOCKING { return ring_.size() / header; }

private:
    // A packet's time and size, ahead of its bytes.
    static constexpr std::size_t header = sizeof(double) + sizeof(std::uint16_t);

    // Copies @p from into the ring from position @p at, wrapping at its end.
    void put(std::size_t at, std::span<const std::byte> from) noexcept ESPRESSO_NONBLOCKING;
    // Copies the ring from position @p at into @p to, wrapping at its end.
    void get(std::size_t at, std::span<std::byte> to) const noexcept ESPRESSO_NONBLOCKING;

    std::vector<std::byte> ring_;
    std::size_t mask_;
    static_assert(realtimeAtomic<std::size_t>);
    // Positions are counts of bytes since construction: head_ the first
    // byte not yet popped, tail_ the first not yet pushed.
    alignas(cacheLine) std::atomic<std::size_t> head_{0};
    alignas(cacheLine) std::atomic<std::size_t> tail_{0};
};

/// Bytes of an OSC port's queue: room for 256 messages of up to 54 bytes
/// (a float sent to an address of up to 40 characters), or for 15 of the
/// largest.
inline constexpr std::size_t oscQueueBytes = std::size_t{16} * 1024;

/// The numbers of one serial line, or of one JSON document, with their
/// time on the device clock (`io.md` §3.1). Fixed-size so ports do not
/// allocate; the I/O threads parse and format, the thread that runs the
/// node never does.
struct dataFrame {
    /// Receive or send time, in seconds on the device clock.
    double time = 0.0;
    /// Values used, up to `metadata::maxChannels`.
    std::uint8_t count = 0;
    /// Bit *i* set: value *i* is missing, as a JSON field can be (§8.2),
    /// and its channel keeps its value.
    std::uint8_t missing = 0;
    /// The values; value *i* is channel *i*.
    std::array<float, metadata::maxChannels> values{};

    /// True when value @p i is used and not missing.
    constexpr bool has(std::size_t i) const noexcept {
        return i < count && i < values.size() && (missing & (1U << i)) == 0;
    }
    /// The values used: the first `count`, at most all of them.
    constexpr std::span<const float> used() const noexcept {
        return std::span(values).first(std::min<std::size_t>(count, values.size()));
    }
};
// The missing mask has a bit per channel.
static_assert(metadata::maxChannels <= 8);

/// A signal fault, such as non-finite samples, reported by a sink on the
/// thread that runs the node. Fixed-size so reporting does not allocate.
struct fault {
    /// Node ID of the sink, truncated.
    std::array<char, 160> node{};
    /// What happened, truncated.
    std::array<char, 96> what{};
};

class ioHub;

/// What a port connects to.
enum class portKind : std::uint8_t {
    midiIn,     ///< MIDI input device
    midiOut,    ///< MIDI output device
    oscIn,      ///< OSC messages received on a UDP port
    oscOut,     ///< OSC messages sent to a UDP port
    save,       ///< A buffer's snapshot, written to a file
    serialIn,   ///< Lines of numbers from a serial device
    serialOut,  ///< Lines of numbers to a serial device
    webIn,      ///< Numbers from a JSON document polled over HTTP
    webStream,  ///< Numbers from the messages of a server-sent event stream
    webSocket,  ///< Numbers from the messages of a WebSocket
    webOut,     ///< HTTP requests carrying numbers as JSON
};

/// Whether a hub's generation (ioHub::generation) has changed since the
/// last call: a scan of the hub's ports that asks first, with the
/// generation read before the scan, skips the scan while it stays the
/// same.
class generationWatch {
public:
    /// True on the first call, and when @p generation differs from the one
    /// the last call was given.
    bool changed(std::uint64_t generation) noexcept {
        if (generation == seen_) return false;
        seen_ = generation;
        return true;
    }

private:
    std::optional<std::uint64_t> seen_;
};

/// Proof that a port is made by its hub: only ioHub can create one. It
/// carries the hub's generation, which the port advances
/// (ioHub::generation).
class portKey {
    friend class ioHub;
    friend class ioPort;
    explicit portKey(std::shared_ptr<std::atomic<std::uint64_t>> generation) noexcept
        : generation_(std::move(generation)) {}
    std::shared_ptr<std::atomic<std::uint64_t>> generation_;
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
    /// Advances the hub's generation: the port is gone.
    virtual ~ioPort() { generation_->fetch_add(1, std::memory_order_seq_cst); }

    ioPort(const ioPort&) = delete;
    ioPort& operator=(const ioPort&) = delete;

    /// What the port connects to.
    portKind kind() const noexcept { return kind_; }
    /// Node ID of the node reading an input port or writing an output
    /// port; empty for MIDI and OSC outputs.
    const std::string& owner() const noexcept { return owner_; }
    /// MIDI device name pattern; empty matches every device.
    const std::string& device() const noexcept { return device_; }
    /// UDP port, for OSC ports.
    std::uint16_t udpPort() const noexcept { return udpPort_; }

    /// Activates or deactivates the port (audio thread, at a swap), then
    /// advances the hub's generation.
    void setActive(bool on) noexcept ESPRESSO_NONBLOCKING {
        active_.store(on, std::memory_order_seq_cst);
        generation_->fetch_add(1, std::memory_order_seq_cst);
    }
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
    /// A port of @p kind for @p device or @p udpPort, read by node @p owner,
    /// made by the hub that gave @p key.
    ioPort(portKey key, portKind kind, std::string owner, std::string device, std::uint16_t udpPort)
        : kind_(kind),
          owner_(std::move(owner)),
          device_(std::move(device)),
          udpPort_(udpPort),
          generation_(std::move(key.generation_)) {}

private:
    portKind kind_ = portKind::midiIn;
    std::string owner_;
    std::string device_;
    std::uint16_t udpPort_ = 0;
    // The hub's generation, which may outlive the hub.
    std::shared_ptr<std::atomic<std::uint64_t>> generation_;
    static_assert(realtimeAtomic<bool> && realtimeAtomic<int> && realtimeAtomic<std::uint64_t>);
    std::atomic<bool> active_{false};
    std::atomic<int> delivering_{0};
};

/// A port carrying MIDI packets.
class midiPort final : public ioPort {
public:
    /// Made by ioHub only.
    midiPort(portKey key, portKind kind, std::string owner, std::string device)
        : ioPort(std::move(key), kind, std::move(owner), std::move(device), 0) {}

    /// The queue: filled by the I/O thread for inputs, by the sink for
    /// outputs.
    spscQueue<midiPacket> queue{1024};
};

/// A port carrying OSC packets.
class oscPort final : public ioPort {
public:
    /// Made by ioHub only.
    oscPort(portKey key, portKind kind, std::string owner, std::uint16_t udpPort, std::string address = {})
        : ioPort(std::move(key), kind, std::move(owner), {}, udpPort), address_(std::move(address)) {}

    /// For an input, the address its node reads, or empty for every
    /// address: the hub delivers only matching messages.
    const std::string& address() const noexcept { return address_; }

    /// The queue: filled by the I/O thread for inputs, by the sink for
    /// outputs.
    oscQueue queue{oscQueueBytes};

private:
    std::string address_;
};

/// A port carrying frames from or to a serial device (`io.md` §4). Its
/// device is the pattern the node gives (§4.1); the serial bridge resolves
/// it to a device.
class serialPort final : public ioPort {
public:
    /// Made by ioHub only.
    serialPort(portKey key, portKind kind, std::string owner, std::string device, std::uint32_t baud)
        : ioPort(std::move(key), kind, std::move(owner), std::move(device), 0), baud_(baud) {}

    /// Speed, in bits per second (§4.2).
    std::uint32_t baud() const noexcept { return baud_; }

    /// The queue: filled by the I/O thread for inputs, by the sink for
    /// outputs.
    spscQueue<dataFrame> queue{256};

private:
    std::uint32_t baud_ = 0;
};

/// An HTTP method (`io.md` §7.1), in the order of webOut's `:method`
/// choices.
enum class httpMethod : std::uint8_t {
    post,  ///< POST, webOut's default
    put,   ///< PUT
    get,   ///< GET
};

/// What a web port asks of the web bridge (`io.md` §5–§7): the node's
/// static parameters, in canonical units. Members a kind does not use stay
/// empty.
struct webRequest {
    /// `http://` or `https://` URL; `ws://` or `wss://` for a WebSocket.
    std::string url;
    /// Request headers, each `Name: value`.
    std::vector<std::string> headers;
    /// For inputs, JSON Pointers, one per channel; none reads the whole
    /// document (§8.1). For webOut, the body's keys, one per value.
    std::vector<std::string> fields;
    /// webIn: polling interval, in seconds.
    double every = 0.0;
    /// webIn and webOut: request timeout, in seconds.
    double timeout = 0.0;
    /// webStream: the SSE event name read; empty reads every message.
    std::string event;
    /// webSocket: the message sent after each connection opens; empty
    /// sends none.
    std::string hello;
    /// webOut: the method of its requests.
    httpMethod method = httpMethod::post;

    /// Equal when every field is equal.
    friend bool operator==(const webRequest&, const webRequest&) = default;
};

/// A port carrying frames from or to a web endpoint (`io.md` §5–§7). For
/// an input, the web bridge fetches what its request describes and
/// delivers the numbers it reads; for webOut, it sends a request for each
/// frame the node queues.
class webPort final : public ioPort {
public:
    /// Frames an input holds (§6.7).
    static constexpr std::size_t inputFrames = 256;
    /// Requests a webOut queues (§7.3).
    static constexpr std::size_t outputFrames = 64;

    /// Made by ioHub only.
    webPort(portKey key, portKind kind, std::string owner, webRequest request)
        : ioPort(std::move(key), kind, std::move(owner), {}, 0),
          queue(kind == portKind::webOut ? outputFrames : inputFrames),
          request_(std::move(request)) {}

    /// What the bridge fetches or sends.
    const webRequest& request() const noexcept { return request_; }

    /// The queue: filled by the I/O thread for inputs, by the sink for
    /// webOut.
    spscQueue<dataFrame> queue;

private:
    webRequest request_;
};

/// A serial device that live ports ask for: its pattern and speed.
struct serialDevice {
    /// The device pattern, as the node gives it (`io.md` §4.1).
    std::string pattern;
    /// Speed, in bits per second.
    std::uint32_t baud = 0;
    /// The nodes that read it, in port order.
    std::vector<std::string> readers;
    /// The nodes that write it, in port order.
    std::vector<std::string> writers;

    /// Equal when every field is.
    friend bool operator==(const serialDevice&, const serialDevice&) = default;
};

/// A buffer's snapshot on its way to a file (`samples.md` §18), from a
/// `bufferSave` node to the control thread. The node copies the buffer in
/// while no save is pending and marks the snapshot ready; the control
/// thread writes the file and frees the port for the next save.
class savePort final : public ioPort {
public:
    /// Made by ioHub only: a snapshot of @p channels channels of @p frames
    /// frames at @p sampleRate, for node @p owner saving buffer
    /// @p bufferId to @p file.
    savePort(portKey key, std::string owner, std::string bufferId, std::string file, int channels,
             std::size_t frames, double sampleRate);

    /// The ID of the buffer saved.
    const std::string& bufferId() const noexcept { return bufferId_; }
    /// The file written, relative to the project root.
    const std::string& file() const noexcept { return file_; }
    /// The snapshot's sample rate, in hertz.
    double sampleRate() const noexcept { return snapshot_.sampleRate(); }

    /// The thread that runs the node: starts a snapshot when no save is
    /// pending. False, counting nothing, when one is.
    bool begin() noexcept ESPRESSO_NONBLOCKING;
    /// The thread that runs the node: channel @p c of the snapshot being
    /// copied.
    std::span<float> channel(int c) noexcept ESPRESSO_NONBLOCKING { return snapshot_.channel(c); }
    /// The thread that runs the node: the snapshot is complete, for the
    /// control thread to write.
    void finish() noexcept ESPRESSO_NONBLOCKING;

    /// What writes a complete snapshot: the port, and the snapshot.
    using writer = std::function<void(const savePort&, const sampleBuffer&)>;
    /// Control thread: when a snapshot is complete, passes it to @p write,
    /// then frees the port for the next save, also when @p write throws.
    void writeReady(const writer& write);

private:
    enum class state : std::uint8_t { idle, copying, ready, writing };

    std::string bufferId_;
    std::string file_;
    sampleBuffer snapshot_;
    static_assert(realtimeAtomic<state>);
    std::atomic<state> state_{state::idle};
};

/// Registry of ports. Opening ports happens on the builder thread;
/// delivery and draining happen on I/O threads. They lock the hub to find
/// ports, never while calling back; the thread that runs a node never
/// locks it.
class ioHub {
public:
    /// Opens a MIDI input for devices matching @p device, read by node
    /// @p owner.
    std::shared_ptr<midiPort> openMidiIn(std::string owner, std::string device);
    /// Opens a MIDI output for the device matching @p device.
    std::shared_ptr<midiPort> openMidiOut(std::string device);
    /// Opens an OSC input on UDP port @p port, read by node @p owner, for
    /// messages to @p address, or to any address when it is empty.
    std::shared_ptr<oscPort> openOscIn(std::string owner, std::uint16_t port, std::string address = {});
    /// Opens an OSC output to UDP port @p port.
    std::shared_ptr<oscPort> openOscOut(std::uint16_t port);
    /// Opens a serial input from the device @p device names, at @p baud,
    /// read by node @p owner.
    std::shared_ptr<serialPort> openSerialIn(std::string owner, std::string device, std::uint32_t baud);
    /// Opens a serial output to the device @p device names, at @p baud,
    /// written by node @p owner.
    std::shared_ptr<serialPort> openSerialOut(std::string owner, std::string device, std::uint32_t baud);
    /// Opens a web input of @p kind (webIn, webStream, or webSocket) for
    /// @p request, read by node @p owner.
    std::shared_ptr<webPort> openWebIn(portKind kind, std::string owner, webRequest request);
    /// Opens a webOut port for @p request, written by node @p owner.
    std::shared_ptr<webPort> openWebOut(std::string owner, webRequest request);
    /// Opens a save port for node @p owner, as savePort's constructor
    /// describes.
    std::shared_ptr<savePort> openSave(std::string owner, std::string bufferId, std::string file,
                                       int channels, std::size_t frames, double sampleRate);

    /// I/O thread: queues @p packet for each node with an active MIDI
    /// input matching @p deviceName, in that node's oldest active port.
    /// Full queues drop it and count a drop. Deliveries hold the hub lock,
    /// so each queue has one producer at a time.
    void deliverMidi(std::string_view deviceName, const midiPacket& packet);
    /// I/O thread: queues @p packet for each node with an active OSC input
    /// on @p port whose address matches the packet's, as deliverMidi()
    /// does; returns how many nodes it reached, so a message that none
    /// reads can be counted.
    std::size_t deliverOsc(std::uint16_t port, const oscPacket& packet);

    /// I/O thread: queues @p frame for each node with an active serial
    /// input whose device pattern is @p device, as deliverMidi() does.
    void deliverSerial(std::string_view device, const dataFrame& frame);
    /// I/O thread: queues @p frame for node @p owner in its oldest active
    /// web input of @p kind. The bridge reads each node's own fields, so it
    /// delivers to nodes, not to every port of a request.
    void deliverWeb(portKind kind, std::string_view owner, const dataFrame& frame);

    /// I/O thread: passes every queued MIDI output packet to @p send with
    /// the port's device pattern. Called from one thread at a time; @p send
    /// runs without the hub lock and may call the hub.
    void drainMidiOut(const std::function<void(std::string_view device, const midiPacket&)>& send);
    /// I/O thread: passes every queued OSC output packet to @p send with
    /// its destination port. Called from one thread at a time; @p send runs
    /// without the hub lock and may call the hub.
    void drainOscOut(const std::function<void(std::uint16_t port, const oscPacket&)>& send);

    /// I/O thread: passes every queued serial output frame to @p send with
    /// its port, as drainMidiOut() does.
    void drainSerialOut(const std::function<void(const serialPort&, const dataFrame&)>& send);

    /// I/O thread: passes every queued webOut frame to @p send with its
    /// port, as drainMidiOut() does.
    void drainWebOut(const std::function<void(const webPort&, const dataFrame&)>& send);

    /// Control thread: calls savePort::writeReady() with @p write on each
    /// live save port. @p write runs without the hub lock.
    void drainSaves(const savePort::writer& write);

    /// The serial devices that live serial ports ask for, inputs and
    /// outputs, one per pattern, in pattern order. A pattern asked for at
    /// two speeds takes its oldest input's, or else its oldest output's.
    std::vector<serialDevice> serialDevices() const;

    /// The live web inputs of @p kind, oldest first: what the web bridge
    /// fetches, each for its node.
    std::vector<std::shared_ptr<const webPort>> webInputs(portKind kind) const;

    /// UDP ports that OSC inputs listen on, sorted and unique.
    std::vector<std::uint16_t> oscInputPorts() const;

    /// A count that advances after a port opens, is activated or
    /// deactivated, or is gone. While it stays the same, so do the live
    /// ports and whether each is active, and so what serialDevices(),
    /// webInputs(), and oscInputPorts() return: a bridge that reads it
    /// before it scans skips the next scan until it changes.
    std::uint64_t generation() const noexcept { return generation_->load(std::memory_order_seq_cst); }

    /// The thread that runs the node: reports a fault in node @p node.
    /// Real-time safe; a full queue drops the report but still counts it.
    /// The queue takes one producer at a time, so a node calls this only
    /// through the handle that declaring it returns
    /// (`nodeContext::reportFaults`): every two such nodes depend on each
    /// other, and run one at a time in plan order (`threads.md` §4.1).
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
    std::size_t deliver(portKind kind, const Packet& packet, Accepts&& accepts);
    // Pops every packet of each live port of @p kind into @p send.
    template <typename Port, typename Packet, typename Send>
    void drain(portKind kind, Send&& send);

    // The ports a drain sends from, found again only when the generation
    // changes. Weak, so that the list keeps no port alive; each drain runs
    // on one thread at a time, so its list takes no lock.
    struct drainList {
        generationWatch watch;
        std::vector<std::weak_ptr<ioPort>> ports;
    };

    template <typename Port, typename... Args>
    std::shared_ptr<Port> open(Args&&... args);

    // From the thread that runs a reporting node to the control thread.
    spscQueue<fault> faults_{64};
    static_assert(realtimeAtomic<std::uint64_t>);
    std::atomic<std::uint64_t> drops_{0};
    std::atomic<std::uint64_t> faultCount_{0};
    // Shared with every port, which advances it and may outlive the hub.
    std::shared_ptr<std::atomic<std::uint64_t>> generation_ = std::make_shared<std::atomic<std::uint64_t>>(0);
    mutable std::vector<std::weak_ptr<ioPort>> ports_;
    mutable std::mutex mutex_;
    std::array<drainList, static_cast<std::size_t>(portKind::webOut) + 1> drains_;  // by kind
};

}  // namespace primitives
