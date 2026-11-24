#include "options.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

#include <espresso/primitives/engine.h>
#include <espresso/protocol/messages.h>

#include "../arguments.h"
#include "cores.h"
#include "realtime.h"
#include "samples.h"

namespace server {

namespace {

// Gives each worker real-time scheduling, as a device's audio thread has,
// for blocks of @p blockSize at @p sampleRate; reports the first refusal,
// with its cost and remedy (threads-review RT-F2).
std::function<void()> realtimeWorkers(double sampleRate, int blockSize) {
    auto reported = std::make_shared<std::once_flag>();
    return [sampleRate, blockSize, reported] {
        if (const std::string error = makeCurrentThreadRealtimeFor(sampleRate, blockSize); !error.empty()) {
            std::call_once(*reported, [&] {
                std::cerr
                    << "espresso-server: workers run without real-time priority (" << error
                    << "), so a busy system can delay them past a block's deadline; give the server "
                       "an rtprio limit (the audio group's, on most distributions) or use --threads 1\n";
            });
        }
    };
}

}  // namespace

primitives::engineOptions engineOptionsFor(const options& o, double sampleRate, int blockSize) {
    primitives::engineOptions out{.audio = {.sampleRate = sampleRate, .blockSize = blockSize},
                                  .pipes = {},
                                  .loadSample = sampleLoaderFor(o.root, std::make_shared<sampleCache>()),
                                  .bufferMemory = static_cast<std::size_t>(o.bufferMemory) * 1024 * 1024,
                                  .profile = o.profile,
                                  .threads = o.threads,
                                  .workerStart = nullptr,
                                  .workerSpin = std::chrono::microseconds(o.workerSpin)};
    if (o.render.empty()) out.workerStart = realtimeWorkers(sampleRate, blockSize);
    return out;
}

void usage(std::ostream& out) {
    out << "usage: espresso-server [--port N] [--device NAME] [--sample-rate R] [--block-size B]\n"
           "                   [--inputs N] [--outputs N] [--no-audio] [--verbose] [--play IR_FILE]\n"
           "                   [--buffer-memory MB] [--root DIR] [--page-lan] [--threads N]\n"
           "                   [--worker-spin US]\n"
           "       espresso-server --render IR_FILE --out WAV_FILE [--seconds S] [--sample-rate R]\n"
           "                   [--block-size B] [--outputs N] [--root DIR] [--buffer-memory MB]\n"
           "                   [--profile] [--threads N]\n"
           "       espresso-server --list-devices\n"
           "\n"
           "Runs the audio engine and listens on 127.0.0.1:N (default "
        << protocol::defaultServerPort
        << ").\n"
           "--outputs 0 runs the engine on the device's clock and writes silence.\n"
           "--no-audio runs the engine on a timer instead of an audio device.\n"
           "--play plays an IR file at startup, as plan -1, until a client swaps in another.\n"
           "--render plays IR offline and writes a 32-bit float WAV file; with --profile, it\n"
           "prints the time the nodes took, by type and by node.\n"
           "--root is the project root that sample file paths resolve against,\n"
           "and under which buffer saves are written (default .).\n"
           "--buffer-memory bounds the memory of the buffers that pieces declare, in MiB\n"
           "(default 1024); a plan that needs more is refused.\n"
           "--threads runs each block on N threads, the audio thread included (1 to "
        << primitives::maxThreads
        << ";\n"
           "default the physical cores this process may run on, at most 16, here "
        << defaultThreads()
        << ");\n"
           "the output is the same at every count.\n"
           "--worker-spin is how long a worker with no task spins before it sleeps, in\n"
           "microseconds (default "
        << primitives::defaultWorkerSpin.count()
        << "): longer wakes it sooner and burns more of a core.\n"
           "--page-lan serves a project's page on every network interface, not only\n"
           "127.0.0.1, so a phone or tablet on the same network can open it with the\n"
           "pairing link the server prints. The page port speaks plain HTTP: use it only\n"
           "on a network you trust.\n"
           "An IR_FILE of - is read from standard input.\n";
}

std::optional<int> parse(std::span<const std::string_view> args, options& o, std::string_view version,
                         std::ostream& out, std::ostream& errors) {
    const std::size_t argc = args.size();
    // The value after option i, read into @p into when it is in range.
    auto value = [&]<typename T>(std::size_t& i, T& into, T lo, T hi) {
        const auto next = apps::nextValue(args, i);
        const auto v = next ? apps::parseInRange<T>(*next, lo, hi) : std::nullopt;
        if (v) into = *v;
        return v.has_value();
    };
    // The text after option i, read into @p into when there is one.
    auto text = [&](std::size_t& i, std::string& into) {
        const auto next = apps::nextValue(args, i);
        if (next) into = std::string(*next);
        return next.has_value();
    };
    for (std::size_t i = 1; i < argc; ++i) {
        const std::string_view a = args[i];
        bool ok = true;
        if (a == "--port") {
            const auto next = apps::nextValue(args, i);
            const auto port = next ? apps::parsePort(*next) : std::nullopt;
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
        } else if (a == "--buffer-memory") {
            ok = value(i, o.bufferMemory, 1, 1024 * 1024);
        } else if (a == "--threads") {
            ok = value(i, o.threads, 1, primitives::maxThreads);
        } else if (a == "--worker-spin") {
            ok = value(i, o.workerSpin, 0, static_cast<int>(primitives::maxWorkerSpin.count()));
        } else if (a == "-h" || a == "--help") {
            usage(out);
            return 0;
        } else if (a == "--version") {
            out << version << "\n";
            return 0;
        } else if (a == "--no-audio") {
            o.noAudio = true;
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
        } else if (a == "--root") {
            ok = text(i, o.root);
        } else {
            ok = false;
        }
        if (!ok) {
            errors << "espresso-server: invalid argument " << a;
            if (args[i] != a) errors << ' ' << args[i];  // the value that was refused
            errors << "\n";
            usage(errors);
            return 2;
        }
    }
    if (!o.render.empty() && o.out.empty()) {
        errors << "espresso-server: --render needs --out\n";
        return 2;
    }
    return std::nullopt;
}

}  // namespace server
