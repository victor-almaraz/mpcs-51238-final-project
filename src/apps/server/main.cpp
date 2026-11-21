// Server: the audio engine behind the client-server protocol
// (protocol.md). One control thread receives messages, builds plans,
// frees retired plans, and moves MIDI and OSC between devices and the
// engine's ports. The audio device thread runs the engine.
//
// Usage:
//   espresso-server [--port N] [--device NAME] [--sample-rate R] [--block-size B]
//               [--inputs N] [--outputs N] [--no-audio] [--verbose] [--play IR_FILE]
//               [--buffer-memory MB] [--threads N]
//   espresso-server --render IR_FILE --out WAV_FILE [--seconds S] [--sample-rate R]
//               [--block-size B] [--outputs N] [--frames DIR] [--fps F]
//               [--width W] [--height H] [--root DIR] [--buffer-memory MB] [--threads N]
//   espresso-server --list-devices

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include <espresso/metadata/version.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/frame.h>
#include <espresso/primitives/io.h>
#include <espresso/primitives/plan.h>
#include <espresso/protocol/jthread.h>
#include <espresso/protocol/messages.h>
#include <espresso/protocol/osc.h>
#include <espresso/protocol/serverSession.h>
#include <espresso/protocol/udp.h>

#include "../arguments.h"
#include "../signals.h"
#include "cores.h"
#include "curlTransport.h"
#include "devices.h"
#include "engineHost.h"
#include "images.h"
#include "offline.h"
#include "oscBridge.h"
#include "pageBridge.h"
#include "pageHelper.h"
#include "pageMessages.h"
#include "pagePairing.h"
#include "realtime.h"
#include "samples.h"
#include "serialBridge.h"
#include "splash.h"
#include "window.h"

namespace {

using clock_type = std::chrono::steady_clock;

// --- Options --------------------------------------------------------------

struct options {
    std::uint16_t port = protocol::defaultServerPort;
    std::string device;
    double sampleRate = 48000.0;
    int blockSize = 256;
    int inputs = 2;
    int outputs = 2;
    bool noAudio = false;
    bool noVideo = false;  // no window: the visual layer is not shown
    bool pageLan = false;  // the page listener binds every interface (io.md §12.8)
    bool verbose = false;
    bool listDevices = false;
    std::string render;
    std::string play;
    std::string out;
    double seconds = 10.0;
    std::string frames;  // offline frame directory; empty: none
    double fps = 60.0;   // frames per second
    bool profile = false;  // offline: print where the nodes' time went
    int width = 1280;    // frame size, in pixels
    int height = 720;
    std::string root = ".";   // project root, against which image and sample paths resolve
    int bufferMemory = 1024;  // bound on live buffers, in MiB (samples.md §5.6)
    int threads = server::defaultThreads();  // threads that run a block (threads.md §7.1)
};

// Gives each worker real-time scheduling, as a device's audio thread has,
// for blocks of @p period; reports the first refusal.
std::function<void()> realtimeWorkers(std::chrono::nanoseconds period) {
    auto reported = std::make_shared<std::once_flag>();
    return [period, reported] {
        if (const std::string error = server::makeCurrentThreadRealtime(period, period / 2); !error.empty()) {
            std::call_once(*reported, [&] {
                std::cerr << "espresso-server: workers run without real-time priority (" << error << ")\n";
            });
        }
    };
}

// The engine's options for @p o at @p sampleRate and @p blockSize: the
// frame size, 16 video pipes (a frame each), images under --root, the
// buffer memory bound, and the threads; workers are real-time in a live
// engine, not an offline render.
primitives::engineOptions engineOptionsFor(const options& o, double sampleRate, int blockSize) {
    constexpr int videoPipes = 16;
    const auto period = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(blockSize / sampleRate));
    primitives::engineOptions out{
        .audio = {.sampleRate = sampleRate,
                  .blockSize = blockSize,
                  .frameWidth = o.width,
                  .frameHeight = o.height},
        .pipes = {.video = videoPipes},
        .loadImage = server::imageLoaderFor(o.root),
        .loadSample = server::sampleLoaderFor(o.root, std::make_shared<server::sampleCache>()),
        .bufferMemory = static_cast<std::size_t>(o.bufferMemory) * 1024 * 1024,
        .profile = o.profile,
        .threads = o.threads,
        .workerStart = nullptr};
    if (o.render.empty()) out.workerStart = realtimeWorkers(period);
    return out;
}

void usage(std::ostream& out) {
    out << "usage: espresso-server [--port N] [--device NAME] [--sample-rate R] [--block-size B]\n"
           "                   [--inputs N] [--outputs N] [--no-audio] [--verbose] [--play IR_FILE]\n"
           "                   [--buffer-memory MB] [--root DIR] [--page-lan] [--threads N]\n"
           "       espresso-server --render IR_FILE --out WAV_FILE [--seconds S] [--sample-rate R]\n"
           "                   [--block-size B] [--outputs N] [--frames DIR] [--fps F]\n"
           "                   [--width W] [--height H] [--root DIR] [--buffer-memory MB] [--profile]\n"
           "                   [--threads N]\n"
           "       espresso-server --list-devices\n"
           "\n"
           "Runs the audio engine and listens on 127.0.0.1:N (default "
        << protocol::defaultServerPort
        << ").\n"
           "--outputs 0 runs the engine on the device's clock and writes silence.\n"
           "--no-audio runs the engine on a timer instead of an audio device.\n"
           "--no-video opens no window; otherwise a window of --width x --height shows the\n"
           "visual layer.\n"
           "--play plays an IR file at startup, as plan -1, until a client swaps in another.\n"
           "--render plays IR offline and writes a 32-bit float WAV file; with --frames, it\n"
           "also writes the visual layer as DIR/frame-00000.png onward, F frames a second;\n"
           "with --profile, it prints the time the nodes took, by type and by node.\n"
           "--root is the project root that image and sample file paths resolve against,\n"
           "and under which buffer saves are written (default .).\n"
           "--buffer-memory bounds the memory of the buffers that pieces declare, in MiB\n"
           "(default 1024); a plan that needs more is refused.\n"
           "--threads runs each block on N threads, the audio thread included (1 to "
        << primitives::maxThreads << "; default the machine's physical cores, at most 16, here "
        << server::defaultThreads()
        << ");\n"
           "the output is the same at every count.\n"
           "--page-lan serves a project's page on every network interface, not only\n"
           "127.0.0.1, so a phone or tablet on the same network can open it with the\n"
           "pairing link the server prints. The page port speaks plain HTTP: use it only\n"
           "on a network you trust.\n"
           "An IR_FILE of - is read from standard input.\n";
}

// Parses the arguments; returns an exit code when the program should stop.
std::optional<int> parse(const std::vector<std::string_view>& args, options& o) {
    const std::size_t argc = args.size();
    // The value after option i, read into @p into when it is in range.
    auto value = [&]<typename T>(std::size_t& i, T& into, T lo, T hi) {
        if (i + 1 >= argc) return false;
        const auto v = apps::parseInRange<T>(args[++i], lo, hi);
        if (v) into = *v;
        return v.has_value();
    };
    // The text after option i, read into @p into when there is one.
    auto text = [&](std::size_t& i, std::string& into) {
        if (i + 1 >= argc) return false;
        into = std::string(args[++i]);
        return true;
    };
    for (std::size_t i = 1; i < argc; ++i) {
        const std::string_view a = args[i];
        bool ok = true;
        if (a == "--port") {
            std::optional<std::uint16_t> port;
            if (i + 1 < argc) port = apps::parsePort(args[++i]);
            if (port) o.port = *port;
            ok = port.has_value();
        } else if (a == "--sample-rate") {
            ok = value(i, o.sampleRate, 8000.0, 384000.0);
        } else if (a == "--block-size") {
            ok = value(i, o.blockSize, 16, 8192);
        } else if (a == "--inputs") {
            ok = value(i, o.inputs, 0, 64);
        } else if (a == "--outputs") {
            ok = value(i, o.outputs, 0, 64);
        } else if (a == "--seconds") {
            ok = value(i, o.seconds, 0.0, 86400.0);
        } else if (a == "--fps") {
            ok = value(i, o.fps, 1.0, 240.0);
        } else if (a == "--width") {
            ok = value(i, o.width, 16, 8192);
        } else if (a == "--height") {
            ok = value(i, o.height, 16, 8192);
        } else if (a == "--buffer-memory") {
            ok = value(i, o.bufferMemory, 1, 1024 * 1024);
        } else if (a == "--threads") {
            ok = value(i, o.threads, 1, primitives::maxThreads);
        } else if (a == "-h" || a == "--help") {
            usage(std::cout);
            return 0;
        } else if (a == "--version") {
            std::cout << "espresso-server " << ESPRESSO_VERSION << " (metadata " << metadata::version << ", "
                      << juce::SystemStats::getJUCEVersion() << ")\n";
            return 0;
        } else if (a == "--no-audio") {
            o.noAudio = true;
        } else if (a == "--no-video") {
            o.noVideo = true;
        } else if (a == "--profile") {
            o.profile = true;
        } else if (a == "--page-lan") {
            o.pageLan = true;
        } else if (a == "--verbose") {
            o.verbose = true;
        } else if (a == "--list-devices") {
            o.listDevices = true;
        } else if (a == "--device") {
            ok = text(i, o.device);
        } else if (a == "--render") {
            ok = text(i, o.render);
        } else if (a == "--out") {
            ok = text(i, o.out);
        } else if (a == "--play") {
            ok = text(i, o.play);
        } else if (a == "--frames") {
            ok = text(i, o.frames);
        } else if (a == "--root") {
            ok = text(i, o.root);
        } else {
            ok = false;
        }
        if (!ok) {
            std::cerr << "espresso-server: invalid argument " << a;
            if (args[i] != a) std::cerr << ' ' << args[i];  // the value that was refused
            std::cerr << "\n";
            usage(std::cerr);
            return 2;
        }
    }
    if (!o.render.empty() && o.out.empty()) {
        std::cerr << "espresso-server: --render needs --out\n";
        return 2;
    }
    return std::nullopt;
}

// --- Offline render (primitives.md §11.2) ----------------------------------

// Reads, validates, and builds the IR in @p path ("-" for standard
// input). Prints errors and returns null when it cannot.
std::shared_ptr<primitives::plan> loadPlan(primitives::engine& engine, const std::string& path,
                                           std::int32_t id) {
    const auto text = server::readText(path);
    if (!text) {
        std::cerr << "espresso-server: cannot read " << path << "\n";
        return nullptr;
    }
    protocol::planError error;
    auto plan = server::buildFromIr(engine, *text, id, error);
    if (!plan) std::cerr << path << ": invalid IR\n" << error.message << "\n";
    return plan;
}

int render(const options& o) {
    if (o.outputs == 0) {
        std::cerr << "espresso-server: --render writes a WAV file, which needs at least one output\n";
        return 2;
    }
    const auto frames = static_cast<std::size_t>(o.seconds * o.sampleRate);
    const auto most = server::maxWavFrames(o.outputs);
    if (frames > most) {
        std::cerr << "espresso-server: a render that long does not fit in a WAV file; render at most "
                  << static_cast<std::int64_t>(static_cast<double>(most) / o.sampleRate) << " seconds\n";
        return 2;
    }
    primitives::engine engine(engineOptionsFor(o, o.sampleRate, o.blockSize));
    auto plan = loadPlan(engine, o.render, 1);
    if (!plan) return 1;
    std::size_t unwritten = 0;
    const server::frameSink writeFrame = [&](std::size_t index, const primitives::frame& f) {
        std::ostringstream name;
        name << "frame-" << std::setw(5) << std::setfill('0') << index << ".png";
        if (!server::writePng(std::filesystem::path(o.frames) / name.str(), f)) ++unwritten;
    };
    if (!o.frames.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(o.frames, ec);
        if (ec) {
            std::cerr << "espresso-server: cannot create " << o.frames << ": " << ec.message() << "\n";
            return 1;
        }
    }
    // Saves are written after the block that completes them, as a live
    // server's control thread would (samples.md §18.4).
    std::size_t unsaved = 0;
    const auto writeSaves = [&] {
        for (const auto& s : server::writeSaves(engine.io(), o.root)) {
            if (s.error.empty()) continue;
            std::cerr << "espresso-server: " << s.message() << "\n";
            ++unsaved;
        }
    };
    const auto played = plan;  // read for the profile once the render ends
    const auto samples =
        server::renderOffline(engine, std::move(plan), frames, o.outputs, o.fps,
                              o.frames.empty() ? server::frameSink{} : writeFrame, writeSaves);
    if (o.profile) std::cerr << server::profileReport(*played, static_cast<double>(frames) / o.sampleRate);
    if (!server::writeWav(o.out, samples, o.sampleRate)) {
        std::cerr << "espresso-server: cannot write " << o.out << "\n";
        return 1;
    }
    if (unwritten > 0) {
        std::cerr << "espresso-server: cannot write " << unwritten << " frames to " << o.frames << "\n";
        return 1;
    }
    return unsaved > 0 ? 1 : 0;
}

// --- Server -------------------------------------------------------------------

int serve(const options& o, const apps::signalWaiter& signals) {
    std::string error;
    auto socket = protocol::udpSocket::bind(o.port, error);
    if (!socket) {
        std::cerr << "espresso-server: cannot listen on 127.0.0.1:" << o.port << ": " << error << "\n";
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
        // A device gives its clock through its outputs: with --outputs 0 it
        // opens two and the callback keeps them silent.
        const int deviceOutputs = o.outputs > 0 ? o.outputs : 2;
        setup.inputDeviceName = setup.outputDeviceName = juce::String(o.device);
        // JUCE takes the optional saved state and the setup by pointer.
        juce::String result =
            devices.initialise(o.inputs, deviceOutputs, nullptr, true, juce::String(o.device), &setup);
        if (result.isNotEmpty() && o.inputs > 0) {
            std::cerr << "espresso-server: audio input unavailable (" << result
                      << "); continuing without inputs\n";
            result = devices.initialise(0, deviceOutputs, nullptr, true, juce::String(o.device), &setup);
        }
        auto* device = devices.getCurrentAudioDevice();
        if (result.isNotEmpty() || !device) {
            std::cerr << "espresso-server: cannot open an audio device: "
                      << (result.isNotEmpty() ? result : juce::String("none found"))
                      << "\nUse --list-devices to see devices, or --no-audio to run without one.\n";
            return 1;
        }
        sampleRate = device->getCurrentSampleRate();
        blockSize = device->getCurrentBufferSizeSamples();
        deviceName = (device->getTypeName() + ": " + device->getName()).toStdString();
        if (o.outputs == 0) deviceName += ", silent";
    }

    primitives::engine engine(engineOptionsFor(o, sampleRate, blockSize));
    server::engineHost host(engine, o.root);
    if (!o.play.empty()) {
        // Through the host, as a client's plan is, so it follows the plan's
        // page port (io.md §12.2). Plan -1 cannot collide with the client's
        // plan IDs, which start at 1.
        const auto text = server::readText(o.play);
        if (!text) {
            std::cerr << "espresso-server: cannot read " << o.play << "\n";
            return 1;
        }
        if (const auto invalid = host.loadPlan(-1, *text)) {
            std::cerr << o.play << ": invalid IR\n" << invalid->message << "\n";
            return 1;
        }
        host.play(-1, 0.05f, "none");
    }
    protocol::serverSession session({.serverVersion = "espresso-server " ESPRESSO_VERSION,
                                     .metadataVersion = std::string(metadata::version),
                                     .sampleRate = static_cast<std::int32_t>(sampleRate),
                                     .blockSize = blockSize},
                                    host);

    // Declared after the engine and the callback, so the device or the
    // timer stops calling into them first on every exit path.
    server::audioCallback callback(engine, o.outputs);
    std::unique_ptr<server::timerDevice> timer;
    std::optional<server::audioCallbackRegistration> registration;
    if (o.noAudio) {
        timer = std::make_unique<server::timerDevice>(engine, o.outputs);
    } else {
        registration.emplace(devices, callback);
    }
    server::midiInputs midiIn(engine.io());
    server::midiOutputs midiOut;
    server::oscBridge osc;
    midiIn.rescan();
    // Serial devices and web endpoints, each on its bridge's own thread
    // (io.md §4–§7); an offline render opens none (§11.1). Their errors
    // reach clients as /io.error (§10.2).
    const server::serialBridge serial(engine.io(), host.io(), {.clock = server::hostSeconds});
    // A new pairing token each start; it appears in no IR and no log file
    // (io.md §12.8).
    server::pageBridge page(
        {.root = o.root,
         .helper = std::string(server::pageHelper()),
         .report = [](std::string_view m) { std::cerr << "espresso-server: " << m << '\n'; },
         .lan = o.pageLan,
         .token = o.pageLan ? server::pairingToken() : std::string(),
         .announce =
             [](const std::vector<std::string>& links) {
                 std::cout << "pages on the network (plain HTTP: trusted networks only):\n";
                 for (const auto& l : links) std::cout << "  " << l << '\n';
                 std::cout << std::flush;
             }});
    const server::webService web(engine.io(), host.io(), {.clock = server::hostSeconds});

    protocol::trafficLog log;
    if (o.verbose) {
        log.onMessage = [](std::string_view direction, protocol::endpoint peer, const protocol::message& m) {
            if (m.address == protocol::address::counters) return;
            std::cout << direction << " :" << peer.port << ' ' << protocol::describe(m) << '\n' << std::flush;
        };
        log.onMalformed = [](protocol::endpoint peer) {
            std::cout << "<- :" << peer.port << " malformed datagram dropped\n" << std::flush;
        };
    }
    protocol::serverLink link(*socket, session, std::move(log));

    std::cout << "espresso-server " << ESPRESSO_VERSION << " listening on 127.0.0.1:" << o.port << " ("
              << deviceName << ", " << sampleRate << " Hz, " << blockSize << " samples; metadata "
              << metadata::version << ")\n"
              << std::flush;
    if (o.verbose) {
        std::cout << "receive buffer: " << socket->receiveBufferBytes() << " bytes (requested "
                  << protocol::udpSocket::requestedBufferBytes << ")\n"
                  << std::flush;
    }

    // Device work: MIDI output, MIDI rescans, and the device's xruns. JUCE
    // allows its MIDI and device calls only on the message thread.
    auto nextScan = clock_type::now() + std::chrono::seconds(2);
    const auto deviceWork = [&] {
        engine.io().drainMidiOut([&](std::string_view device, const primitives::midiPacket& p) {
            if (!midiOut.send(device, p)) engine.io().countDrop();
        });
        if (auto* device = devices.getCurrentAudioDevice()) host.setXruns(device->getXRunCount());
        if (const auto now = clock_type::now(); now >= nextScan) {
            midiIn.rescan();
            midiOut.rescan();
            nextScan = now + std::chrono::seconds(2);
        }
    };
    // The control loop: protocol, notifications, OSC, and counters, plus
    // the device work when it runs on the message thread.
    const auto control = [&](const espresso::stop_token& stop, bool onMessageThread) {
        while (!stop.stop_requested() && !signals.requested() && !host.quitRequested()) {
            link.serveOnce(2);
            for (const auto& m : host.poll()) {
                if (m.address == protocol::address::ioError) {
                    std::cerr << "espresso-server: " << m.string(0).value_or("") << ": "
                              << m.string(1).value_or("") << " (" << m.int32(2).value_or(0) << ")\n";
                }
                link.broadcast(m);
            }
            // The page listener follows the active plan; pages' messages
            // reach OSC input port P, and packets for P go to pages, not UDP,
            // even while the listener cannot open (io.md §12.2, §12.3).
            const auto pagePort = host.pagePort();
            page.listen(pagePort);
            for (const auto& bytes : page.takeMessages()) {
                const auto p = primitives::oscPacket::of(bytes, server::hostSeconds());
                if (!pagePort || !p || engine.io().deliverOsc(*pagePort, *p) == 0) engine.io().countDrop();
            }
            osc.pump(engine.io(), server::hostSeconds(), std::cerr,
                     o.verbose ? std::optional(std::ref(std::cout)) : std::nullopt,
                     [&page, pagePort](std::uint16_t port, const primitives::oscPacket& p) {
                         if (!pagePort || port != *pagePort) return false;
                         const auto used = p.used();
                         server::oscBytes bytes(used.size());
                         std::ranges::transform(used, bytes.begin(),
                                                [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
                         page.send(bytes);
                         return true;
                     });
            if (onMessageThread) deviceWork();
            link.sendCountersIfDue(clock_type::now());
        }
    };
    if (o.noVideo) {
        control(espresso::stop_token{}, true);
        return 0;
    }
    // With a window the main thread runs the message loop: the control loop
    // moves to its own thread and a timer does the device work.
    auto& messages = *juce::MessageManager::getInstance();
    const server::videoWindow window(engine, server::splashTextFor(ESPRESSO_VERSION, o.port),
                                     [&messages] { messages.stopDispatchLoop(); });
    juce::TimedCallback devicesTimer(deviceWork);
    devicesTimer.startTimer(2);
    const espresso::jthread controlThread([&](const espresso::stop_token& stop) {
        control(stop, false);
        messages.stopDispatchLoop();  // a quit or a signal closes the window too
    });
    messages.runDispatchLoop();
    return 0;  // controlThread stops and joins, then the timer, then the window
}

}  // namespace

int main(int argc, char** argv) {
    options o;
    if (const auto code = parse(apps::arguments(argc, argv), o)) return *code;
    if (!o.render.empty()) return render(o);
    const apps::signalWaiter signals;
    const juce::ScopedJuceInitialiser_GUI juceRuntime;
    if (o.listDevices) {
        server::listDevices();
        return 0;
    }
    return serve(o, signals);
}
