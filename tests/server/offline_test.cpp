// Offline rendering, WAVE output, and the OSC bridge.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

#include <sys/resource.h>

#include <proj/metadata/graph.h>
#include <proj/primitives/engine.h>
#include <proj/protocol/osc.h>
#include <proj/protocol/udp.h>

#include "offline.h"
#include "oscBridge.h"
#include "support/graph.h"
#include "support/tempDir.h"
#include "support/unwrap.h"

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
    primitives::engine e({.audio = {.sampleRate = 48000.0, .blockSize = 100}, .pipes = {}});
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
    // Header: RIFF (12) + fmt chunk (8 + 18) + fact chunk (8 + 4) + data header (8) = 58.
    REQUIRE(server::wavHeaderBytes == 58);
    REQUIRE(bytes.size() == 58 + 16);  // 2 frames x 2 channels x 4 bytes
    // The little-endian field of type T at byte @p at.
    const auto field = [&bytes]<typename T>(std::size_t at, T) {
        std::array<char, sizeof(T)> raw{};
        bytes.copy(raw.data(), raw.size(), at);
        return std::bit_cast<T>(raw);
    };
    CHECK(bytes.substr(0, 4) == "RIFF");
    CHECK(field(4, std::uint32_t{}) == bytes.size() - 8);
    CHECK(bytes.substr(8, 8) == "WAVEfmt ");
    CHECK(field(16, std::uint32_t{}) == 18);  // fmt chunk size, with cbSize
    CHECK(field(20, std::uint16_t{}) == 3);   // WAVE_FORMAT_IEEE_FLOAT
    CHECK(field(22, std::uint16_t{}) == 2);   // channels
    CHECK(field(24, std::uint32_t{}) == 44100);
    CHECK(field(28, std::uint32_t{}) == 44100 * 8);  // bytes per second
    CHECK(field(32, std::uint16_t{}) == 8);          // block align
    CHECK(field(34, std::uint16_t{}) == 32);         // bits per sample
    CHECK(field(36, std::uint16_t{}) == 0);          // cbSize
    CHECK(bytes.substr(38, 4) == "fact");
    CHECK(field(42, std::uint32_t{}) == 4);
    CHECK(field(46, std::uint32_t{}) == 2);  // frames
    CHECK(bytes.substr(50, 4) == "data");
    CHECK(field(54, std::uint32_t{}) == 16);
    const auto frame = field(58, std::array<float, 4>{});
    CHECK(frame[0] == 0.5f);  // interleaved: L0 R0 L1 R1
    CHECK(frame[1] == 1.0f);
    CHECK(frame[2] == -0.25f);

    // 32-bit RIFF size: header after the first 8 bytes plus samples.
    CHECK(server::maxWavFrames(2) == (std::size_t{0xFFFFFFFF} - 50) / 8);
    CHECK(server::maxWavFrames(2) > std::size_t{48000} * 60 * 60 * 3);
    CHECK(server::maxWavFrames(2) < std::size_t{48000} * 86400);
    CHECK_FALSE(server::readText((dir / "missing.ir.json").string()));
}

namespace {

// Limits the size of files this process writes, for the scope, and
// ignores SIGXFSZ, so a write past the limit fails with EFBIG instead of
// ending the process. setrlimit takes its limits and std::signal its
// disposition by pointer: the narrow unsafe use, confined to this guard.
class fileSizeLimit {
public:
    explicit fileSizeLimit(rlim_t bytes) {
        getrlimit(RLIMIT_FSIZE, &previous_);
        std::signal(SIGXFSZ, SIG_IGN);
        rlimit limit = previous_;
        limit.rlim_cur = bytes;
        setrlimit(RLIMIT_FSIZE, &limit);
    }
    ~fileSizeLimit() {
        setrlimit(RLIMIT_FSIZE, &previous_);
        std::signal(SIGXFSZ, SIG_DFL);  // the test process's disposition
    }
    fileSizeLimit(const fileSizeLimit&) = delete;
    fileSizeLimit& operator=(const fileSizeLimit&) = delete;

private:
    rlimit previous_{};
};

}  // namespace

TEST_CASE("WAVE output reports failure when the samples do not reach the file") {
    tempDir dir;
    const auto path = (dir / "short.wav").string();
    const std::vector<std::vector<float>> channels(2, std::vector<float>(48000, 0.25f));  // 384 KB
    {
        const fileSizeLimit limit(rlim_t{64} * 1024);
        CHECK_FALSE(server::writeWav(path, channels, 48000.0));
    }
    CHECK(server::writeWav(path, channels, 48000.0));
}

TEST_CASE("the OSC bridge carries messages in and out over loopback") {
    const auto sender = support::unwrap(protocol::udpSocket::bind(0));
    const auto listener = support::unwrap(protocol::udpSocket::bind(0));
    // A free port for the bridge's input: bound, read, and closed at once.
    const auto inPort = support::unwrap(protocol::udpSocket::bind(0)).port();

    graph g;
    g.pipe(control(1));
    auto& in = g.node("in", "oscIn");
    in.params = {{"port", static_cast<double>(inPort)}, {"address", std::string("/knob")}};
    in.outputs["value"] = {control(1)};
    auto& out = g.node("out", "oscOut");
    out.params = {{"port", static_cast<double>(listener.port())}, {"address", std::string("/echo")}};
    out.inputs["value"] = {control(1)};
    renderer r;
    r.play(g.spec);
    r.run(64);

    server::oscBridge bridge;
    std::ostringstream errors;
    bridge.pump(r.engine.io(), 0.0, errors);  // opens the input socket
    CHECK(errors.str().empty());
    sender.send(protocol::encode({.address = "/knob", .args = {0.625f}}), {inPort});
    std::optional<float> echoed;
    for (int i = 0; i < 200 && echoed != 0.625f; ++i) {
        bridge.pump(r.engine.io(), 0.0, errors);
        r.run(64);
        bridge.pump(r.engine.io(), 0.0, errors);
        while (auto d = listener.receive(1)) {
            if (const auto m = protocol::decode(d->bytes); m && m->address == "/echo") echoed = m->float32(0);
        }
    }
    CHECK(echoed == 0.625f);
}

TEST_CASE("the OSC bridge reads a bounded number of datagrams per call") {
    const auto sender = support::unwrap(protocol::udpSocket::bind(0));
    // A free port for the bridge's input: bound, read, and closed at once.
    const auto inPort = support::unwrap(protocol::udpSocket::bind(0)).port();

    graph g;
    g.pipe(control(1));
    auto& in = g.node("in", "oscIn");
    in.params = {{"port", static_cast<double>(inPort)}, {"address", std::string("/knob")}};
    in.outputs["value"] = {control(1)};
    renderer r;
    r.play(g.spec);
    r.run(64);

    server::oscBridge bridge;
    std::ostringstream errors;
    CHECK(bridge.pump(r.engine.io(), 0.0, errors) == 0);  // opens the input socket
    const std::size_t sent = server::oscBridge::maxDatagramsPerPump + 44;
    const auto packet = protocol::encode({.address = "/knob", .args = {0.5f}});
    for (std::size_t i = 0; i < sent; ++i) REQUIRE(sender.send(packet, {inPort}));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // loopback delivery
    CHECK(bridge.pump(r.engine.io(), 0.0, errors) == server::oscBridge::maxDatagramsPerPump);
    CHECK(bridge.pump(r.engine.io(), 0.0, errors) == sent - server::oscBridge::maxDatagramsPerPump);
}
