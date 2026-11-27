// Server: the audio engine behind the client-server protocol
// (protocol.md). One control thread receives messages, builds plans,
// frees retired plans, and moves MIDI and OSC between devices and the
// engine's ports. The audio device thread runs the engine. The options
// and their usage are in options.cpp.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include <espresso/metadata/version.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/io.h>
#include <espresso/primitives/plan.h>
#include <espresso/protocol/messages.h>
#include <espresso/protocol/osc.h>
#include <espresso/protocol/serverSession.h>
#include <espresso/protocol/udp.h>

#include "../arguments.h"
#include "../signals.h"
#include "clips.h"
#include "cores.h"
#include "curlTransport.h"
#include "devices.h"
#include "engineHost.h"
#include "offline.h"
#include "options.h"
#include "oscBridge.h"
#include "pageBridge.h"
#include "pageHelper.h"
#include "pagePairing.h"
#include "samples.h"
#include "serialBridge.h"

namespace {

using clock_type = std::chrono::steady_clock;

// The engine for @p o at @p sampleRate and @p blockSize, or null, with a
// message, when the system refuses its worker threads.
std::unique_ptr<primitives::engine> makeEngine(const server::options& o, double sampleRate, int blockSize) {
    try {
        return std::make_unique<primitives::engine>(server::engineOptionsFor(o, sampleRate, blockSize));
    } catch (const std::system_error& e) {
        std::cerr << "espresso-server: cannot start " << o.threads << " threads: " << e.what()
                  << "; try a smaller --threads\n";
        return nullptr;
    }
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
    auto plan = server::buildFromIr(engine, *text, id, error).plan;  // offline: no page (io.md §12.12)
    if (!plan) std::cerr << path << ": invalid IR\n" << error.message << "\n";
    return plan;
}

int render(const server::options& o) {
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
    const auto owned = makeEngine(o, o.sampleRate, o.blockSize);
    if (!owned) return 1;
    primitives::engine& engine = *owned;
    auto plan = loadPlan(engine, o.render, 1);
    if (plan) {
        for (const auto& e : server::clipFiles(o.root).sync(engine.clips()))
            std::cerr << "espresso-server: clip " << e << "\n";
    }
    if (!plan) return 1;
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
    const auto samples = server::renderOffline(engine, std::move(plan), frames, o.outputs, writeSaves);
    if (o.profile) std::cerr << server::profileReport(*played, static_cast<double>(frames) / o.sampleRate);
    if (!server::writeWav(o.out, samples, o.sampleRate)) {
        std::cerr << "espresso-server: cannot write " << o.out << "\n";
        return 1;
    }
    return unsaved > 0 ? 1 : 0;
}

// --- Server -------------------------------------------------------------------

int serve(const server::options& o, const apps::signalWaiter& signals) {
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
        // JUCE refuses a named device that does not list the inputs asked
        // for, so a device with outputs only is named as an input only
        // while inputs are wanted.
        setup.outputDeviceName = juce::String(o.device);
        if (o.inputs > 0) setup.inputDeviceName = setup.outputDeviceName;
        // On Linux, PipeWire's JACK interface, whose callback thread is
        // real-time; ALSA's is not.
        if (const auto warning = server::preferJack(devices, o.device); !warning.empty())
            std::cerr << "espresso-server: " << warning << "\n";
        // JUCE takes the optional saved state and the setup by pointer.
        juce::String result =
            devices.initialise(o.inputs, deviceOutputs, nullptr, true, juce::String(o.device), &setup);
        if (result.isNotEmpty() && o.inputs > 0) {
            std::cerr << "espresso-server: audio input unavailable (" << result
                      << "); continuing without inputs\n";
            setup.inputDeviceName = {};
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
    // A worker sleeps only once it has spun as long as --worker-spin with
    // no task; blocks a period apart restart its spin (threads.md §6.5).
    if (const double period = 1e6 * blockSize / sampleRate; o.threads > 1 && o.workerSpin >= period) {
        std::cerr << "espresso-server: --worker-spin " << o.workerSpin << " us reaches the block period ("
                  << period << " us), so workers never sleep\n";
    }

    const auto owned = makeEngine(o, sampleRate, blockSize);
    if (!owned) return 1;
    primitives::engine& engine = *owned;
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
         .controls = std::string(server::pageControls()),
         .report = [](std::string_view m) { std::cerr << "espresso-server: " << m << '\n'; },
         .lan = o.pageLan,
         .token = o.pageLan ? server::pairingToken() : std::string(),
         .announce =
             [](const std::vector<std::string>& links) {
                 std::cout << "pages on the network (plain HTTP: trusted networks only):\n";
                 for (const auto& l : links) std::cout << "  " << l << '\n';
                 std::cout << std::flush;
             }});
    // OSC over UDP and over pages, on one path (io.md §12.3); after the
    // page bridge, which it holds.
    server::oscBridge osc(page);
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
    // allows its MIDI and device calls only on the message thread, which is
    // this one.
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
    // The control loop, on the main thread: protocol, notifications, OSC,
    // counters, and the device work.
    while (!signals.requested() && !host.quitRequested()) {
        link.serveOnce(2);
        for (const auto& m : host.poll()) {
            if (m.address == protocol::address::ioError) {
                std::cerr << "espresso-server: " << m.string(0).value_or("") << ": "
                          << m.string(1).value_or("") << " (" << m.int32(2).value_or(0) << ")\n";
            }
            link.broadcast(m);
        }
        // The page listener follows the active plan, whose page port
        // takes pages' messages and sends to pages, not UDP (io.md §12.2,
        // §12.3).
        page.serve(host.page());
        page.keep(host.pageAddresses());
        if (page.takeClipsChanged()) host.clipsChanged();
        osc.pump(engine.io(), server::hostSeconds(), std::cerr,
                 o.verbose ? std::optional(std::ref(std::cout)) : std::nullopt, host.pagePort());
        deviceWork();
        link.sendCountersIfDue(clock_type::now());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    server::options o;
    const std::string version = "espresso-server " ESPRESSO_VERSION " (metadata " +
                                std::string(metadata::version) + ", " +
                                juce::SystemStats::getJUCEVersion().toStdString() + ")";
    if (const auto code = server::parse(apps::arguments(argc, argv), o, version, std::cout, std::cerr))
        return *code;
    // The default never exceeds them (threads.md §7.1).
    if (const int usable = server::usableProcessors(); o.threads > usable) {
        std::cerr << "espresso-server: --threads " << o.threads << " exceeds the " << usable
                  << " processors this process may use\n";
    }
    if (!o.render.empty()) return render(o);
    const apps::signalWaiter signals;
    const juce::ScopedJuceInitialiser_GUI juceRuntime;
    if (o.listDevices) {
        server::listDevices();
        return 0;
    }
    return serve(o, signals);
}
