// The server's protocol engine, through the protocol session.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/ir/ir.h>
#include <espresso/metadata/version.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/io.h>
#include <espresso/primitives/plan.h>
#include <espresso/protocol/irTransfer.h>
#include <espresso/protocol/messages.h>
#include <espresso/protocol/osc.h>
#include <espresso/protocol/serverSession.h>
#include <espresso/protocol/udp.h>

#include "engineHost.h"
#include "samples.h"
#include "support/graph.h"
#include "support/tempDir.h"
#include "support/unwrap.h"

using Catch::Matchers::ContainsSubstring;
using namespace protocol;
using namespace support;

namespace {

const endpoint client{40100};

struct fixture {
    primitives::engine audio{{.audio = {.sampleRate = 48000.0, .blockSize = 64}, .pipes = {}}};
    ::server::engineHost host{audio};
    serverSession session{{.serverVersion = "espresso-server test",
                           .metadataVersion = std::string(metadata::version),
                           .sampleRate = 48000,
                           .blockSize = 64},
                          host};
    std::int32_t nextId = 1;

    std::vector<message> send(std::string_view address, std::vector<argument> args) {
        std::vector<message> out;
        for (auto& o : session.handle({.address = std::string(address), .args = std::move(args)}, client))
            out.push_back(o.msg);
        return out;
    }

    std::vector<message> request(std::string_view address, std::vector<argument> args = {}) {
        args.insert(args.begin(), nextId++);
        return send(address, std::move(args));
    }

    // Transfers @p ir as plan @p plan and returns the /ir.end replies.
    std::vector<message> submit(std::int32_t plan, const std::string& ir) {
        const auto chunks = splitChunks(ir);
        request(address::irBegin, irBeginArgs(plan, ir));
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            session.handle(chunkMessage(plan, static_cast<std::int32_t>(i), chunks[i]), client);  // no reply
        }
        return request(address::irEnd, {plan});
    }

    void run(int frames) {
        std::vector<float> l(static_cast<std::size_t>(frames)), r(static_cast<std::size_t>(frames));
        const std::array outs = {std::span(l), std::span(r)};
        audio.process({.inputs = {}, .outputs = outs}, frames, 0.0);
    }

    std::vector<std::string> notifications() {
        std::vector<std::string> out;
        for (const auto& m : host.poll()) out.push_back(describe(m));
        return out;
    }
};

std::string sineIr(const std::string& id = "sine") {
    graph g;
    g.pipe(audio(0)).pipe(control(1));
    auto& s = g.node(id, "sineOsc");
    s.params["freq"] = control(1);
    s.outputs["out"] = {audio(0)};
    g.node("out", "audioOut").inputs["in"] = {audio(0)};
    return ir::write(g.spec);
}

} // namespace

TEST_CASE("IR submitted over the protocol plays, swaps, and retires") {
    fixture s;
    REQUIRE(s.request(address::hello, {std::string("test"), std::string(metadata::version)}).at(0).address == address::helloOk);
    s.request(address::notify, {1});

    CHECK(s.submit(5, sineIr()).at(0).address == address::done);
    CHECK(s.host.hasPlan(5));
    CHECK(s.request(address::play, {5, 0.0f, std::string("none")}).at(0).address == address::done);
    CHECK_FALSE(s.host.hasPlan(5));  // moved into the engine
    s.run(64);
    CHECK(s.notifications() == std::vector<std::string>{"/plan.active 5"});
    CHECK(s.host.counters().activePlan == 5);

    s.send(address::controlSet, {1, 330.0f});
    s.run(64);
    CHECK(s.request(address::controlGet, {1}).at(0).args.at(2) == argument{330.0f});

    CHECK(s.submit(6, sineIr("other")).at(0).address == address::done);
    s.request(address::play, {6, 0.01f, std::string("none")});
    s.run(64);
    CHECK(s.notifications() == std::vector<std::string>{"/plan.active 6"});
    s.run(960);
    CHECK(s.notifications() == std::vector<std::string>{"/plan.retired 5"});

    s.request(address::stop, {0.0f});
    s.run(64);
    CHECK(s.notifications() == std::vector<std::string>{"/plan.retired 6"});
    CHECK(s.host.counters().activePlan == 0);
}

TEST_CASE("invalid IR fails with the node and message") {
    fixture s;
    s.request(address::hello, {std::string("test"), std::string(metadata::version)});
    auto ir = sineIr();
    ir.replace(ir.find("sineOsc"), 7, "sineOss");
    const auto reply = s.submit(1, ir).at(0);
    CHECK(reply.address == address::fail);
    CHECK(reply.args.at(1) == argument{static_cast<std::int32_t>(errorCode::irInvalid)});
    CHECK_THAT(std::get<std::string>(reply.args.at(2)), ContainsSubstring("node sine: unknown primitive type sineOss"));
    CHECK_FALSE(s.host.hasPlan(1));
}

TEST_CASE("the client's placeholder graph is valid IR") {
    fixture s;
    s.request(address::hello, {std::string("test"), std::string(metadata::version)});
    CHECK(s.submit(1, R"({"format":"espresso-ir","version":1,"pipes":[],"nodes":[]})").at(0).address ==
          address::done);
}

TEST_CASE("counters carry the tempo and the beat position (protocol.md 5)") {
    fixture s;
    s.request(address::hello, {std::string("test"), std::string(metadata::version)});
    s.request(address::notify, {1});
    graph g;
    g.spec.tempo = 90.0;
    g.spec.meter = 3;
    REQUIRE(s.submit(1, ir::write(g.spec)).at(0).address == address::done);
    s.request(address::play, {1, 0.0f, std::string("none")});
    s.run(48000);  // one second at 90 BPM from the first block: 1.5 beats

    const auto out = s.session.countersMessages();
    REQUIRE(out.size() == 1);
    const message& m = out[0].msg;
    CHECK(m.tags() == "iiiiffdi");  // missed, xruns, drops, overflows, load, tempo, beat, meter
    CHECK(unwrap(m.float32(5)) == 90.0f);
    CHECK(unwrap(m.float64(6)) == Catch::Approx(1.5));
    CHECK(unwrap(m.int32(7)) == 3);
}

TEST_CASE("counters report device xruns") {
    fixture s;
    s.host.setXruns(3);
    CHECK(s.host.counters().xruns == 3);
}

TEST_CASE("signal faults reach clients as unsolicited failures") {
    fixture s;
    s.audio.io().reportFault("main.lisp/main/audio-out@3:5#0", "non-finite audio samples");
    const auto out = s.host.poll();
    REQUIRE(out.size() == 1);
    CHECK(out[0].address == address::fail);
    CHECK(out[0].args.at(0) == argument{std::int32_t{0}});
    CHECK(out[0].args.at(1) == argument{static_cast<std::int32_t>(errorCode::signalFault)});
    CHECK(std::get<std::string>(out[0].args.at(2)) == "node main.lisp/main/audio-out@3:5#0: non-finite audio samples");
}

namespace {

// A complete snapshot of one channel of @p samples, from node @p node, on
// its way to @p file.
std::shared_ptr<primitives::savePort> snapshot(primitives::ioHub& io, const std::string& node,
                                               const std::string& file, const std::vector<float>& samples) {
    auto port = io.openSave(node, "main.lisp/main/buffer@3:9#0", file, 1, samples.size(), 48000.0);
    REQUIRE(port->begin());
    std::ranges::copy(samples, port->channel(0).begin());
    port->finish();
    return port;
}

}  // namespace

TEST_CASE("saved buffers are written under the project root and reported (samples.md 18.3)") {
    const tempDir dir;
    primitives::engine audio{{.audio = {.sampleRate = 48000.0, .blockSize = 64}, .pipes = {}}};
    ::server::engineHost host{audio, dir.path()};
    const auto good = snapshot(audio.io(), "save", "takes/one.wav", {0.5f, -0.25f, 0.125f});
    const auto bad = snapshot(audio.io(), "escape", "../one.wav", {0.5f});
    const auto absolute = snapshot(audio.io(), "absolute", (dir / "abs.wav").string(), {0.5f});
    const auto out = host.poll();
    REQUIRE(out.size() == 3);
    CHECK(out[0] ==
          message{.address = std::string(address::bufferSaved),
                  .args = {std::string("main.lisp/main/buffer@3:9#0"), std::string("takes/one.wav")}});
    std::string error;
    const auto written = ::server::decodeAudioFile(dir / "takes/one.wav", error);
    INFO(error);
    const auto& w = support::unwrap(written);
    CHECK(w.sampleRate == 48000.0);
    CHECK(w.audio.channels == std::vector<std::vector<float>>{{0.5f, -0.25f, 0.125f}});
    // A file outside the root fails at its node.
    CHECK(out[1].address == address::fail);
    CHECK(out[1].args.at(1) == argument{static_cast<std::int32_t>(errorCode::fileError)});
    CHECK_THAT(
        std::get<std::string>(out[1].args.at(2)),
        ContainsSubstring("node escape: cannot save buffer main.lisp/main/buffer@3:9#0 to ../one.wav: the "
                          "file must be a path under the project root"));
    CHECK(out[1].args.at(3) == argument{std::string("node")});
    CHECK(out[1].args.at(4) == argument{std::string("escape")});
    CHECK_FALSE(std::filesystem::exists(dir.path().parent_path() / "one.wav"));
    CHECK(out[2].args.at(4) == argument{std::string("absolute")});
    CHECK_FALSE(std::filesystem::exists(dir / "abs.wav"));
    // Each snapshot is written once; the ports take the next save.
    CHECK(host.poll().empty());
    CHECK(good->begin());
}

TEST_CASE("a plan ID that is loaded, in flight, or active cannot be loaded again") {
    fixture s;
    s.request(address::hello, {std::string("test"), std::string(metadata::version)});
    s.request(address::notify, {1});
    auto failsAs = [](const std::vector<message>& replies, std::string_view text) {
        REQUIRE(replies.size() == 1);
        CHECK(replies[0].address == address::fail);
        CHECK_THAT(std::get<std::string>(replies[0].args.at(2)), ContainsSubstring(std::string(text)));
    };
    REQUIRE(s.submit(5, sineIr()).at(0).address == address::done);
    failsAs(s.submit(5, sineIr("other")), "plan 5 is loaded");
    s.request(address::play, {5, 0.0f, std::string("none")});
    failsAs(s.submit(5, sineIr("other")), "plan 5 is playing");  // in flight
    s.run(64);
    CHECK(s.notifications() == std::vector<std::string>{"/plan.active 5"});
    failsAs(s.submit(5, sineIr("other")), "plan 5 is playing");  // active

    REQUIRE(s.submit(6, sineIr("other")).at(0).address == address::done);
    s.request(address::play, {6, 0.0f, std::string("none")});
    s.run(128);
    CHECK(s.notifications() == std::vector<std::string>{"/plan.active 6", "/plan.retired 5"});
    CHECK(s.submit(5, sineIr()).at(0).address == address::done);  // retired: the ID is free
}

TEST_CASE("a plan plays once: a repeated /play of its ID fails") {
    fixture s;
    s.request(address::hello, {std::string("test"), std::string(metadata::version)});
    REQUIRE(s.submit(5, sineIr()).at(0).address == address::done);
    CHECK(s.request(address::play, {5, 0.0f, std::string("none")}).at(0).address == address::done);
    const auto again = s.request(address::play, {5, 0.0f, std::string("none")});
    REQUIRE(again.size() == 1);
    CHECK(again[0].address == address::fail);
    CHECK(again[0].args.at(1) == argument{static_cast<std::int32_t>(errorCode::unknownPlan)});
}

TEST_CASE("a newer /play withdraws a plan waiting for its bar, and frees its ID at /plan.retired") {
    fixture s;
    s.request(address::hello, {std::string("test"), std::string(metadata::version)});
    REQUIRE(s.submit(2, sineIr()).at(0).address == address::done);
    s.request(address::play, {2, 0.0f, std::string("none")});
    s.run(64);
    CHECK(s.notifications() == std::vector<std::string>{"/plan.active 2"});

    REQUIRE(s.submit(3, sineIr("waiting")).at(0).address == address::done);
    s.request(address::play, {3, 0.0f, std::string("bar")});
    s.run(64);
    CHECK(s.notifications().empty());  // waits for the next bar
    CHECK(s.submit(3, sineIr("waiting")).at(0).address == address::fail);

    REQUIRE(s.submit(4, sineIr("newer")).at(0).address == address::done);
    s.request(address::play, {4, 0.0f, std::string("none")});
    s.run(64);
    CHECK(s.notifications() ==
          std::vector<std::string>{"/plan.active 4", "/plan.retired 3", "/plan.retired 2"});
    CHECK(s.submit(3, sineIr("waiting")).at(0).address == address::done);
}

TEST_CASE("/quit asks the server to exit") {
    fixture s;
    s.request(address::hello, {std::string("test"), std::string(metadata::version)});
    CHECK_FALSE(s.host.quitRequested());
    CHECK(s.request(address::quit).at(0).address == address::done);
    CHECK(s.host.quitRequested());
}

TEST_CASE("plans loaded and never played are bounded, oldest dropped first") {
    fixture s;
    s.request(address::hello, {std::string("test"), std::string(metadata::version)});
    const auto limit = static_cast<std::int32_t>(::server::engineHost::maxLoaded);
    for (std::int32_t id = 1; id <= limit + 1; ++id)
        REQUIRE(s.submit(id, sineIr()).at(0).address == address::done);
    CHECK_FALSE(s.host.hasPlan(1));
    for (std::int32_t id = 2; id <= limit + 1; ++id) CHECK(s.host.hasPlan(id));
}

TEST_CASE("a build that fails with any exception returns an error") {
    // A delay line holds max-time: 3600 s at 2 PHz is 7.2e18 samples, more
    // than a std::vector<float> can hold (at most 2^64 / 4), so building
    // throws std::length_error before allocating.
    primitives::engine huge({.audio = {.sampleRate = 2e15, .blockSize = 64}, .pipes = {}});
    graph g;
    g.pipe(audio(0)).pipe(audio(1));
    g.node("sine", "sineOsc").outputs["out"] = {audio(0)};
    auto& d = g.node("delay", "delayLine");
    d.params["max-time"] = 3600.0;
    d.inputs["in"] = {audio(0)};
    d.outputs["out"] = {audio(1)};
    protocol::planError error;
    std::shared_ptr<primitives::plan> plan;
    CHECK_NOTHROW(plan = ::server::buildFromIr(huge, ir::write(g.spec), 1, error));
    CHECK_FALSE(plan);
    CHECK_FALSE(error.message.empty());
}
