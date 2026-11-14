// Offline rendering, WAVE output, and the OSC bridge.

#include <catch2/catch_test_macros.hpp>

#include <proj/protocol/osc.h>
#include <proj/protocol/udp.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include "offline.h"
#include "oscBridge.h"
#include "support/graph.h"
#include "support/tempDir.h"

using namespace support;

namespace {

metadata::graphSpec noisy() {
    graph g;
    g.spec.seed = 5;
    g.pipe(audio(0));
    g.node("noise", "noise").outputs["out"] = {audio(0)};
    g.node("out", "audioOut").inputs["in"] = {audio(0)};
    return g.spec;
}

std::vector<std::vector<float>> renderNoise() {
    primitives::engine e({{48000.0, 100}, {}});
    return server::renderOffline(e, e.build(noisy(), 1), 1050, 2);
}

} // namespace

TEST_CASE("offline renders repeat exactly and fill the requested frames") {
    const auto a = renderNoise();
    REQUIRE(a.size() == 2);
    CHECK(a[0].size() == 1050);  // a partial last block
    CHECK(a == renderNoise());
    CHECK(rms(a[0]) > 0.1);
    CHECK(peak(a[1]) == 0.0);  // mono into output 0 only
}

TEST_CASE("WAVE files hold 32-bit float samples") {
    tempDir dir;
    const auto path = (dir / "x.wav").string();
    REQUIRE(server::writeWav(path, {{0.5f, -0.25f}, {1.0f, 0.0f}}, 44100.0));
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    const std::string bytes = s.str();
    REQUIRE(bytes.size() == 44 + 16);
    CHECK(bytes.substr(0, 4) == "RIFF");
    CHECK(bytes.substr(8, 8) == "WAVEfmt ");
    std::uint16_t format = 0, channels = 0;
    std::uint32_t rate = 0;
    std::memcpy(&format, bytes.data() + 20, 2);
    std::memcpy(&channels, bytes.data() + 22, 2);
    std::memcpy(&rate, bytes.data() + 24, 4);
    CHECK(format == 3);
    CHECK(channels == 2);
    CHECK(rate == 44100);
    float frame[4];
    std::memcpy(frame, bytes.data() + 44, 16);
    CHECK(frame[0] == 0.5f);  // interleaved: L0 R0 L1 R1
    CHECK(frame[1] == 1.0f);
    CHECK(frame[2] == -0.25f);

    CHECK(server::fitsWav(48000 * 60, 2));
    CHECK_FALSE(server::fitsWav(std::size_t{48000} * 86400, 2));
    CHECK_FALSE(server::readText((dir / "missing.ir.json").string()));
}

TEST_CASE("the OSC bridge carries messages in and out over loopback") {
    auto sender = protocol::udpSocket::bind(0);
    auto listener = protocol::udpSocket::bind(0);
    auto probe = protocol::udpSocket::bind(0);  // finds a free port for the bridge's input
    REQUIRE((sender && listener && probe));
    const auto inPort = probe->port();
    probe.reset();

    graph g;
    g.pipe(control(1));
    auto& in = g.node("in", "oscIn");
    in.params = {{"port", double(inPort)}, {"address", std::string("/knob")}};
    in.outputs["value"] = {control(1)};
    auto& out = g.node("out", "oscOut");
    out.params = {{"port", double(listener->port())}, {"address", std::string("/echo")}};
    out.inputs["value"] = {control(1)};
    renderer r;
    r.play(g.spec);
    r.run(64);

    server::oscBridge bridge;
    std::ostringstream errors;
    bridge.pump(r.engine.io(), 0.0, errors);  // opens the input socket
    CHECK(errors.str().empty());
    sender->send(protocol::encode({"/knob", {0.625f}}), {inPort});
    std::optional<float> echoed;
    for (int i = 0; i < 200 && echoed != 0.625f; ++i) {
        bridge.pump(r.engine.io(), 0.0, errors);
        r.run(64);
        bridge.pump(r.engine.io(), 0.0, errors);
        while (auto d = listener->receive(1)) {
            if (const auto m = protocol::decode(d->bytes); m && m->address == "/echo") echoed = m->float32(0);
        }
    }
    CHECK(echoed == 0.625f);
}
