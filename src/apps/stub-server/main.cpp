// Stub server: speaks the client-server protocol (protocol.md) without an
// audio engine. It accepts IR, schedules swaps on a 120 BPM 4/4 clock,
// reports plan activation and retirement, and sends zeroed counters. It
// produces no sound. Use it to exercise the client's run controls. Like
// the real server, it consumes a plan on /play (stubEngine.h).
//
// Usage: proj-stub-server [--port N] [--quiet]

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <proj/metadata/version.h>
#include <proj/protocol/messages.h>
#include <proj/protocol/osc.h>
#include <proj/protocol/serverSession.h>
#include <proj/protocol/udp.h>

#include "../arguments.h"
#include "../signals.h"
#include "stubEngine.h"

namespace {

using stub::clock_type;

void usage(std::ostream& out) {
    out << "usage: proj-stub-server [--port N] [--quiet]\n"
           "\n"
           "Listens on 127.0.0.1:N (default " << protocol::defaultServerPort << ") and answers the\n"
           "client-server protocol without producing sound.\n";
}

std::string timestamp(clock_type::time_point start) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3)
        << std::chrono::duration<double>(clock_type::now() - start).count() << 's';
    return out.str();
}

} // namespace

int main(int argc, char** argv) {
    std::uint16_t port = protocol::defaultServerPort;
    bool quiet = false;
    const auto args = apps::arguments(argc, argv);
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--help" || arg == "-h") {
            usage(std::cout);
            return 0;
        }
        if (arg == "--quiet") {
            quiet = true;
        } else if (arg == "--port" && i + 1 < args.size()) {
            const auto p = apps::parsePort(args[++i]);
            if (!p) {
                std::cerr << "proj-stub-server: invalid port " << args[i] << '\n';
                return 2;
            }
            port = *p;
        } else {
            usage(std::cerr);
            return 2;
        }
    }

    const apps::signalWaiter signals;  // before any other thread
    std::string error;
    auto socket = protocol::udpSocket::bind(port, error);
    if (!socket) {
        std::cerr << "proj-stub-server: cannot listen on 127.0.0.1:" << port << ": " << error << '\n';
        return 1;
    }

    const auto start = clock_type::now();
    stub::stubEngine engine(start);
    protocol::serverSession session({.serverVersion = "proj-stub-server " PROJ_VERSION,
                                     .metadataVersion = std::string(metadata::version),
                                     .sampleRate = 48000,
                                     .blockSize = 256},
                                    engine);
    protocol::trafficLog log;
    if (!quiet) {
        log.onMessage = [start](std::string_view direction, protocol::endpoint peer,
                                const protocol::message& m) {
            if (m.address == protocol::address::counters) return;
            std::cout << timestamp(start) << ' ' << direction << " :" << peer.port << ' '
                      << protocol::describe(m) << '\n'
                      << std::flush;
        };
        log.onMalformed = [start](protocol::endpoint peer) {
            std::cout << timestamp(start) << " dropped malformed datagram from :" << peer.port << '\n'
                      << std::flush;
        };
    }
    protocol::serverLink link(*socket, session, std::move(log));
    std::cout << "proj-stub-server " << PROJ_VERSION << " listening on 127.0.0.1:" << port << " (metadata "
              << metadata::version << "; no audio)\n"
              << std::flush;

    while (!signals.requested() && !engine.quitRequested()) {
        link.serveOnce(5);
        const auto now = clock_type::now();
        for (const auto& m : engine.tick(now)) link.broadcast(m);
        link.sendCountersIfDue(now);
    }
    std::cout << "proj-stub-server: exiting\n" << std::flush;
    return 0;
}
