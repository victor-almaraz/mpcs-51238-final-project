// Server: the audio engine behind the client-server protocol
// (protocol.md). One control thread receives messages, builds plans,
// frees retired plans, and moves MIDI and OSC between devices and the
// engine's ports. The audio device thread runs the engine.
//
// Usage:
//   proj-server [--port N] [--device NAME] [--sample-rate R] [--block-size B]
//               [--inputs N] [--outputs N] [--no-audio] [--verbose]
//   proj-server --render IR_FILE --out WAV_FILE [--seconds S] [--sample-rate R]
//               [--block-size B] [--outputs N]
//   proj-server --list-devices

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <pthread.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include <proj/ir/ir.h>
#include <proj/metadata/version.h>
#include <proj/primitives/engine.h>
#include <proj/protocol/messages.h>
#include <proj/protocol/serverSession.h>
#include <proj/protocol/udp.h>

#include "engineHost.h"

namespace {

using clock_type = std::chrono::steady_clock;

std::atomic<bool> interrupted{false};

// Blocks SIGINT and SIGTERM in every thread, including the device and
// MIDI threads created later, and waits for them on one thread. The
// default disposition is restored first: a shell starts background jobs
// with SIGINT ignored, and an ignored signal is discarded, not queued.
void waitForSignals() {
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
    static sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    std::thread([] {
        int sig = 0;
        sigwait(&set, &sig);
        interrupted = true;
    }).detach();
}

// The device clock shared by audio callbacks and MIDI timestamps, in seconds.
double hostSeconds() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }

// --- Options --------------------------------------------------------------

struct options {
    std::uint16_t port = protocol::defaultServerPort;
    std::string device;
    double sampleRate = 48000.0;
    int blockSize = 256;
    int inputs = 2;
    int outputs = 2;
    bool noAudio = false;
    bool verbose = false;
    bool listDevices = false;
    std::string render;
    std::string out;
    double seconds = 10.0;
};

void usage(std::ostream& out) {
    out << "usage: proj-server [--port N] [--device NAME] [--sample-rate R] [--block-size B]\n"
           "                   [--inputs N] [--outputs N] [--no-audio] [--verbose]\n"
           "       proj-server --render IR_FILE --out WAV_FILE [--seconds S] [--sample-rate R]\n"
           "                   [--block-size B] [--outputs N]\n"
           "       proj-server --list-devices\n"
           "\n"
           "Runs the audio engine and listens on 127.0.0.1:N (default "
        << protocol::defaultServerPort
        << ").\n"
           "--no-audio runs the engine on a timer instead of an audio device.\n"
           "--render plays IR offline and writes a 32-bit float WAV file.\n";
}

// Parses argv; returns an exit code when the program should stop.
std::optional<int> parse(int argc, char** argv, options& o) {
    auto number = [&](int& i, double lo, double hi) -> std::optional<double> {
        if (i + 1 >= argc) return std::nullopt;
        char* end = nullptr;
        const double v = std::strtod(argv[++i], &end);
        if (*end != '\0' || v < lo || v > hi) return std::nullopt;
        return v;
    };
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        std::optional<double> v;
        if (a == "-h" || a == "--help") {
            usage(std::cout);
            return 0;
        } else if (a == "--version") {
            std::cout << "proj-server " << PROJ_VERSION << " (metadata " << metadata::version << ", "
                      << juce::SystemStats::getJUCEVersion() << ")\n";
            return 0;
        } else if (a == "--no-audio") {
            o.noAudio = true;
        } else if (a == "--verbose") {
            o.verbose = true;
        } else if (a == "--list-devices") {
            o.listDevices = true;
        } else if ((a == "--device" || a == "--render" || a == "--out") && i + 1 < argc) {
            (a == "--device" ? o.device : a == "--render" ? o.render : o.out) = argv[++i];
        } else if (a == "--port" && (v = number(i, 1, 65535))) {
            o.port = static_cast<std::uint16_t>(*v);
        } else if (a == "--sample-rate" && (v = number(i, 8000, 384000))) {
            o.sampleRate = *v;
        } else if (a == "--block-size" && (v = number(i, 16, 8192))) {
            o.blockSize = static_cast<int>(*v);
        } else if (a == "--inputs" && (v = number(i, 0, 64))) {
            o.inputs = static_cast<int>(*v);
        } else if (a == "--outputs" && (v = number(i, 1, 64))) {
            o.outputs = static_cast<int>(*v);
        } else if (a == "--seconds" && (v = number(i, 0, 86400))) {
            o.seconds = *v;
        } else {
            std::cerr << "proj-server: invalid argument " << a << "\n";
            usage(std::cerr);
            return 2;
        }
    }
    if (!o.render.empty() && o.out.empty()) {
        std::cerr << "proj-server: --render needs --out\n";
        return 2;
    }
    return std::nullopt;
}

// --- Offline render (primitives.md §11.2) ----------------------------------

// Writes interleaved 32-bit float samples as a WAVE file.
bool writeWav(const std::string& path, const std::vector<std::vector<float>>& channels, double sampleRate) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const auto put = [&](auto v) { f.write(reinterpret_cast<const char*>(&v), sizeof v); };
    const auto numChannels = static_cast<std::uint16_t>(channels.size());
    const auto frames = static_cast<std::uint32_t>(channels.empty() ? 0 : channels[0].size());
    const std::uint32_t dataBytes = frames * numChannels * 4u;
    f.write("RIFF", 4);
    put(std::uint32_t{36 + dataBytes});
    f.write("WAVEfmt ", 8);
    put(std::uint32_t{16});
    put(std::uint16_t{3});  // IEEE float
    put(numChannels);
    put(static_cast<std::uint32_t>(sampleRate));
    put(static_cast<std::uint32_t>(sampleRate) * numChannels * 4u);
    put(static_cast<std::uint16_t>(numChannels * 4u));
    put(std::uint16_t{32});
    f.write("data", 4);
    put(dataBytes);
    for (std::uint32_t i = 0; i < frames; ++i) {
        for (const auto& c : channels) put(c[i]);
    }
    return static_cast<bool>(f);
}

int render(const options& o) {
    std::ifstream in(o.render, std::ios::binary);
    if (!in) {
        std::cerr << "proj-server: cannot read " << o.render << "\n";
        return 1;
    }
    std::stringstream text;
    text << in.rdbuf();
    const auto r = ir::read(text.str());
    if (!r.ok()) {
        std::cerr << o.render << ": invalid IR\n" << r.text() << "\n";
        return 1;
    }
    primitives::engine engine({{o.sampleRate, o.blockSize}, {}});
    try {
        engine.play(engine.build(*r.graph, 1), 0.0, primitives::quantization::none);
    } catch (const primitives::buildError& e) {
        std::cerr << o.render << ": node " << e.node() << ": " << e.what() << "\n";
        return 1;
    }
    const auto frames = static_cast<std::size_t>(o.seconds * o.sampleRate);
    std::vector<std::vector<float>> channels(static_cast<std::size_t>(o.outputs), std::vector<float>(frames, 0.0f));
    std::vector<float*> pointers(channels.size());
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(o.blockSize), frames - done);
        for (std::size_t c = 0; c < channels.size(); ++c) pointers[c] = channels[c].data() + done;
        const primitives::deviceBuffers device{nullptr, 0, pointers.data(), o.outputs};
        engine.process(device, static_cast<int>(n), static_cast<double>(done) / o.sampleRate);
        done += n;
    }
    if (!writeWav(o.out, channels, o.sampleRate)) {
        std::cerr << "proj-server: cannot write " << o.out << "\n";
        return 1;
    }
    return 0;
}

// --- Devices ----------------------------------------------------------------

void listDevices() {
    juce::AudioDeviceManager manager;
    for (auto* type : manager.getAvailableDeviceTypes()) {
        type->scanForDevices();
        std::cout << "audio " << type->getTypeName() << ":\n";
        for (const auto& name : type->getDeviceNames(false)) std::cout << "  output  " << name << "\n";
        for (const auto& name : type->getDeviceNames(true)) std::cout << "  input   " << name << "\n";
    }
    std::cout << "midi:\n";
    for (const auto& d : juce::MidiInput::getAvailableDevices()) std::cout << "  input   " << d.name << "\n";
    for (const auto& d : juce::MidiOutput::getAvailableDevices()) std::cout << "  output  " << d.name << "\n";
}

class audioCallback final : public juce::AudioIODeviceCallback {
public:
    explicit audioCallback(primitives::engine& e) : engine_(e) {}

    void audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override {
        engine_.process({in, numIn, out, numOut}, numSamples, hostSeconds());
    }
    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

private:
    primitives::engine& engine_;
};

// Opens every MIDI input and forwards messages to the engine's ports.
class midiInputs final : public juce::MidiInputCallback {
public:
    explicit midiInputs(primitives::ioHub& io) : io_(io) {}

    // Opens inputs that appeared since the last scan.
    void rescan() {
        for (const auto& d : juce::MidiInput::getAvailableDevices()) {
            if (open_.count(d.identifier)) continue;
            if (auto input = juce::MidiInput::openDevice(d.identifier, this)) {
                input->start();
                open_[d.identifier] = std::move(input);
            }
        }
    }

    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& m) override {
        const int size = m.getRawDataSize();
        if (size < 1 || size > 3 || m.isSysEx()) return;
        primitives::midiPacket p;
        p.time = m.getTimeStamp();
        p.size = static_cast<std::uint8_t>(size);
        std::memcpy(p.bytes.data(), m.getRawData(), static_cast<std::size_t>(size));
        io_.deliverMidi(source->getName().toStdString(), p);
    }

private:
    primitives::ioHub& io_;
    std::map<juce::String, std::unique_ptr<juce::MidiInput>> open_;
};

// MIDI outputs opened on demand by device pattern.
class midiOutputs {
public:
    void send(std::string_view pattern, const primitives::midiPacket& p) {
        auto& slot = open_[std::string(pattern)];
        if (!slot) {
            for (const auto& d : juce::MidiOutput::getAvailableDevices()) {
                if (primitives::ioHub::matches(pattern, d.name.toStdString())) {
                    slot = juce::MidiOutput::openDevice(d.identifier);
                    break;
                }
            }
        }
        if (slot) slot->sendMessageNow(juce::MidiMessage(p.bytes.data(), p.size));
    }

private:
    std::map<std::string, std::unique_ptr<juce::MidiOutput>> open_;
};

// UDP sockets for OSC sources, opened and closed as plans ask for ports.
class oscSockets {
public:
    void pump(primitives::ioHub& io, bool verbose) {
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
                std::cerr << "proj-server: cannot listen for OSC on 127.0.0.1:" << port << ": " << error << "\n";
            }
        }
        for (auto& [port, socket] : inputs_) {
            while (auto d = socket.receive(0)) {
                primitives::oscPacket p;
                if (d->bytes.size() > p.bytes.size()) {
                    io.countDrop();
                    continue;
                }
                p.time = hostSeconds();
                p.size = static_cast<std::uint16_t>(d->bytes.size());
                std::memcpy(p.bytes.data(), d->bytes.data(), d->bytes.size());
                io.deliverOsc(port, p);
            }
        }
        io.drainOscOut([&](std::uint16_t port, const primitives::oscPacket& p) {
            if (!output_) output_ = protocol::udpSocket::bind(0);
            if (output_) output_->send({reinterpret_cast<const std::uint8_t*>(p.bytes.data()), p.size}, {port});
            if (verbose) std::cout << "osc -> :" << port << " (" << p.size << " bytes)" << std::endl;
        });
    }

private:
    std::map<std::uint16_t, protocol::udpSocket> inputs_;
    std::set<std::uint16_t> failed_;
    std::optional<protocol::udpSocket> output_;
};

// Runs the engine in real time without a device (--no-audio).
class timerDevice {
public:
    timerDevice(primitives::engine& e, int outputs)
        : engine_(e), buffers_(static_cast<std::size_t>(outputs), std::vector<float>(static_cast<std::size_t>(e.info().blockSize))),
          thread_([this] { run(); }) {}
    ~timerDevice() {
        stop_ = true;
        thread_.join();
    }

private:
    void run() {
        std::vector<float*> outs;
        for (auto& b : buffers_) outs.push_back(b.data());
        const int n = engine_.info().blockSize;
        const auto period = std::chrono::duration<double>(n / engine_.info().sampleRate);
        auto next = clock_type::now();
        while (!stop_) {
            engine_.process({nullptr, 0, outs.data(), static_cast<int>(outs.size())}, n, hostSeconds());
            next += std::chrono::duration_cast<clock_type::duration>(period);
            std::this_thread::sleep_until(next);
        }
    }

    primitives::engine& engine_;
    std::vector<std::vector<float>> buffers_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// --- Server -------------------------------------------------------------------

int serve(const options& o) {
    std::string error;
    auto socket = protocol::udpSocket::bind(o.port, &error);
    if (!socket) {
        std::cerr << "proj-server: cannot listen on 127.0.0.1:" << o.port << ": " << error << "\n";
        return 1;
    }

    juce::AudioDeviceManager devices;
    double sampleRate = o.sampleRate;
    int blockSize = o.blockSize;
    std::string deviceName = "no audio device";
    if (!o.noAudio) {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.sampleRate = o.sampleRate;
        setup.bufferSize = o.blockSize;
        setup.inputDeviceName = setup.outputDeviceName = juce::String(o.device);
        juce::String result = devices.initialise(o.inputs, o.outputs, nullptr, true, juce::String(o.device), &setup);
        if (result.isNotEmpty() && o.inputs > 0) {
            std::cerr << "proj-server: audio input unavailable (" << result << "); continuing without inputs\n";
            result = devices.initialise(0, o.outputs, nullptr, true, juce::String(o.device), &setup);
        }
        auto* device = devices.getCurrentAudioDevice();
        if (result.isNotEmpty() || !device) {
            std::cerr << "proj-server: cannot open an audio device: "
                      << (result.isNotEmpty() ? result : juce::String("none found"))
                      << "\nUse --list-devices to see devices, or --no-audio to run without one.\n";
            return 1;
        }
        sampleRate = device->getCurrentSampleRate();
        blockSize = device->getCurrentBufferSizeSamples();
        deviceName = (device->getTypeName() + ": " + device->getName()).toStdString();
    }

    primitives::engine engine({{sampleRate, blockSize}, {}});
    server::engineHost host(engine);
    protocol::serverSession session(
        {"proj-server " PROJ_VERSION, std::string(metadata::version), static_cast<std::int32_t>(sampleRate), blockSize},
        host);

    audioCallback callback(engine);
    std::unique_ptr<timerDevice> timer;
    if (o.noAudio) {
        timer = std::make_unique<timerDevice>(engine, o.outputs);
    } else {
        devices.addAudioCallback(&callback);
    }
    midiInputs midiIn(engine.io());
    midiOutputs midiOut;
    oscSockets osc;
    midiIn.rescan();

    std::cout << "proj-server " << PROJ_VERSION << " listening on 127.0.0.1:" << o.port << " (" << deviceName << ", "
              << sampleRate << " Hz, " << blockSize << " samples; metadata " << metadata::version << ")" << std::endl;

    auto sendAll = [&](const std::vector<protocol::outgoing>& out) {
        for (const auto& m : out) {
            socket->send(protocol::encode(m.msg), m.to);
            if (o.verbose && m.msg.address != protocol::address::counters) {
                std::cout << "-> :" << m.to.port << ' ' << protocol::describe(m.msg) << std::endl;
            }
        }
    };

    auto nextCounters = clock_type::now();
    auto nextScan = clock_type::now() + std::chrono::seconds(2);
    while (!interrupted && !host.quitRequested()) {
        if (auto d = socket->receive(2)) {
            if (o.verbose) {
                if (const auto m = protocol::decode(d->bytes)) std::cout << "<- :" << d->from.port << ' ' << protocol::describe(*m) << std::endl;
            }
            sendAll(session.handleDatagram(d->bytes, d->from));
        }
        for (const auto& m : host.poll()) sendAll(session.broadcast(m));
        osc.pump(engine.io(), o.verbose);
        engine.io().drainMidiOut([&](std::string_view device, const primitives::midiPacket& p) { midiOut.send(device, p); });

        const auto now = clock_type::now();
        if (now >= nextCounters) {
            if (auto* device = devices.getCurrentAudioDevice()) host.setXruns(device->getXRunCount());
            sendAll(session.countersMessages());
            nextCounters = now + std::chrono::milliseconds(protocol::countersIntervalMs);
        }
        if (now >= nextScan) {
            midiIn.rescan();
            nextScan = now + std::chrono::seconds(2);
        }
    }

    devices.removeAudioCallback(&callback);
    timer.reset();
    devices.closeAudioDevice();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    options o;
    if (const auto code = parse(argc, argv, o)) return *code;
    if (!o.render.empty()) return render(o);
    waitForSignals();
    const juce::ScopedJuceInitialiser_GUI juceRuntime;
    if (o.listDevices) {
        listDevices();
        return 0;
    }
    return serve(o);
}
