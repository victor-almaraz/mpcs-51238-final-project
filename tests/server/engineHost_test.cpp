// The server's protocol engine, through the protocol session.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <bit>

#include <proj/ir/ir.h>
#include <proj/metadata/version.h>
#include <proj/protocol/crc32.h>
#include <proj/protocol/irTransfer.h>
#include <proj/protocol/messages.h>

#include "engineHost.h"
#include "support/graph.h"

using Catch::Matchers::ContainsSubstring;
using namespace protocol;
using namespace support;

namespace {

const endpoint client{40100};

struct fixture {
    primitives::engine audio{{{48000.0, 64}, {}}};
    ::server::engineHost host{audio};
    serverSession session{{"proj-server test", std::string(metadata::version), 48000, 64}, host};
    std::int32_t nextId = 1;

    std::vector<message> send(std::string_view address, std::vector<argument> args) {
        std::vector<message> out;
        for (auto& o : session.handle({std::string(address), std::move(args)}, client)) out.push_back(o.msg);
        return out;
    }

    std::vector<message> request(std::string_view address, std::vector<argument> args = {}) {
        args.insert(args.begin(), nextId++);
        return send(address, std::move(args));
    }

    // Transfers @p ir as plan @p plan and returns the /ir.end replies.
    std::vector<message> submit(std::int32_t plan, const std::string& ir) {
        const auto chunks = splitChunks(ir);
        request(address::irBegin, {plan, static_cast<std::int64_t>(ir.size()), static_cast<std::int32_t>(chunks.size()),
                                   std::bit_cast<std::int32_t>(crc32(ir))});
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            send(address::irChunk, {plan, static_cast<std::int32_t>(i), blob{{chunks[i].begin(), chunks[i].end()}}});
        }
        return request(address::irEnd, {plan});
    }

    void run(int frames) {
        std::vector<float> l(static_cast<std::size_t>(frames)), r(static_cast<std::size_t>(frames));
        const std::span<float> outs[] = {std::span(l), std::span(r)};
        audio.process({{}, outs}, frames, 0.0);
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
    CHECK(s.submit(1, R"({"format":"proj-ir","version":1,"pipes":[],"nodes":[]})").at(0).address == address::done);
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
