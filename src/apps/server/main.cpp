// Server: the audio engine behind the client-server protocol
// (protocol.md). One control thread receives messages, builds plans,
// frees retired plans, and moves MIDI and OSC between devices and the
// engine's ports. The audio device thread runs the engine.
//
// Usage:
//   proj-server [--port N] [--device NAME] [--sample-rate R] [--block-size B]
//               [--inputs N] [--outputs N] [--no-audio] [--verbose] [--play IR_FILE]
//   proj-server --render IR_FILE --out WAV_FILE [--seconds S] [--sample-rate R]
//               [--block-size B] [--outputs N]
//   proj-server --list-devices

#include <atomic>
#include <chrono>
#include <csignal>
#include <pthread.h>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
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

#include "devices.h"
#include "engineHost.h"
#include "offline.h"
#include "oscBridge.h"

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
    std::string play;
    std::string out;
    double seconds = 10.0;
};

void usage(std::ostream& out) {
    out << "usage: proj-server [--port N] [--device NAME] [--sample-rate R] [--block-size B]\n"
           "                   [--inputs N] [--outputs N] [--no-audio] [--verbose] [--play IR_FILE]\n"
           "       proj-server --render IR_FILE --out WAV_FILE [--seconds S] [--sample-rate R]\n"
           "                   [--block-size B] [--outputs N]\n"
           "       proj-server --list-devices\n"
           "\n"
           "Runs the audio engine and listens on 127.0.0.1:N (default "
        << protocol::defaultServerPort
        << ").\n"
           "--no-audio runs the engine on a timer instead of an audio device.\n"
           "--play plays an IR file at startup, as plan -1, until a client swaps in another.\n"
           "--render plays IR offline and writes a 32-bit float WAV file.\n"
           "An IR_FILE of - is read from standard input.\n";
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
        } else if ((a == "--device" || a == "--render" || a == "--out" || a == "--play") && i + 1 < argc) {
            (a == "--device" ? o.device : a == "--render" ? o.render : a == "--play" ? o.play : o.out) = argv[++i];
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

// Reads, validates, and builds the IR in @p path ("-" for standard
// input). Prints errors and returns null when it cannot.
std::unique_ptr<primitives::plan> loadPlan(primitives::engine& engine, const std::string& path, std::int32_t id) {
    const auto text = server::readText(path);
    if (!text) {
        std::cerr << "proj-server: cannot read " << path << "\n";
        return nullptr;
    }
    std::string error;
    auto plan = server::buildFromIr(engine, *text, id, error);
    if (!plan) std::cerr << path << ": invalid IR\n" << error << "\n";
    return plan;
}

int render(const options& o) {
    const auto frames = static_cast<std::size_t>(o.seconds * o.sampleRate);
    if (!server::fitsWav(frames, o.outputs)) {
        std::cerr << "proj-server: a render that long does not fit in a WAV file; render less than "
                  << static_cast<long>(4294967000.0 / (o.outputs * 4.0 * o.sampleRate)) << " seconds\n";
        return 2;
    }
    primitives::engine engine({{o.sampleRate, o.blockSize}, {}});
    auto plan = loadPlan(engine, o.render, 1);
    if (!plan) return 1;
    if (!server::writeWav(o.out, server::renderOffline(engine, std::move(plan), frames, o.outputs), o.sampleRate)) {
        std::cerr << "proj-server: cannot write " << o.out << "\n";
        return 1;
    }
    return 0;
}

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
    if (!o.play.empty()) {
        // Plan -1 cannot collide with the client's plan IDs, which start at 1.
        auto plan = loadPlan(engine, o.play, -1);
        if (!plan) return 1;
        engine.play(std::move(plan), 0.05, primitives::quantization::none);
    }
    server::engineHost host(engine);
    protocol::serverSession session(
        {"proj-server " PROJ_VERSION, std::string(metadata::version), static_cast<std::int32_t>(sampleRate), blockSize},
        host);

    server::audioCallback callback(engine);
    std::unique_ptr<server::timerDevice> timer;
    if (o.noAudio) {
        timer = std::make_unique<server::timerDevice>(engine, o.outputs);
    } else {
        devices.addAudioCallback(&callback);
    }
    server::midiInputs midiIn(engine.io());
    server::midiOutputs midiOut;
    server::oscBridge osc;
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
        osc.pump(engine.io(), server::hostSeconds(), std::cerr, o.verbose ? &std::cout : nullptr);
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
        server::listDevices();
        return 0;
    }
    return serve(o);
}
