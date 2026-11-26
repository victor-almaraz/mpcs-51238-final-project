/// @file
/// Test support: a protocol server session on a loopback socket, served
/// from its own thread, whose engine activates every plan it is played.
/// The editors' tests play against it in place of espresso-server.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <espresso/protocol/jthread.h>
#include <espresso/protocol/messages.h>
#include <espresso/protocol/osc.h>
#include <espresso/protocol/serverSession.h>
#include <espresso/protocol/udp.h>

#include "support/unwrap.h"

namespace support {

// Activates plans as soon as they are played, or refuses them with what
// `refuse` returns for their IR.
class immediateEngine : public protocol::engine {
public:
    std::optional<protocol::planError> loadPlan(std::int32_t planId, std::string ir) override {
        if (refuse) {
            if (auto why = refuse(ir)) return why;
        }
        plans[planId] = std::move(ir);
        return std::nullopt;
    }
    bool hasPlan(std::int32_t planId) const override { return plans.contains(planId); }
    void play(std::int32_t planId, float, std::string_view) override { pending.push_back(planId); }
    void stop(float) override {
        if (active != 0) retired.push_back(active);
        active = 0;
    }
    void setControl(std::int32_t, float) override {}
    float control(std::int32_t) const override { return 0.0f; }
    protocol::serverCounters counters() const override {
        protocol::serverCounters c;
        c.xruns = 3;
        c.activePlan = active;
        c.tempo = 96.0f;
        c.beat = 9.5;  // bar 3, beat 2 in 4/4: beats 8 to 11 are bar 3
        c.meter = 4;
        c.threads = 4;
        c.threadLoads = {0.1f, 0.62f, 0.3f, 0.0f};
        return c;
    }
    void quit() override {}

    std::function<std::optional<protocol::planError>(const std::string&)> refuse;
    std::map<std::int32_t, std::string> plans;
    std::vector<std::int32_t> pending;
    std::vector<std::int32_t> retired;
    std::int32_t active = 0;
};

// A server session on a loopback socket, served from its own thread.
class testServer {
public:
    explicit testServer(std::string metadataVersion)
        : testServer(std::move(metadataVersion), support::unwrap(protocol::udpSocket::bind(0))) {}

    // Serves on @p socket, which may have been bound, and received
    // datagrams, before the server started.
    testServer(std::string metadataVersion, protocol::udpSocket socket)
        : session_({.serverVersion = "test-server",
                    .metadataVersion = std::move(metadataVersion),
                    .sampleRate = 48000,
                    .blockSize = 256},
                   engine_),
          socket_(std::move(socket)),
          thread_([this](const espresso::stop_token& stop) { loop(stop); }) {}

    std::uint16_t port() const { return socket_.port(); }

    // Refuses plans with what @p f returns for their IR.
    void refuseWith(std::function<std::optional<protocol::planError>(const std::string&)> f) {
        std::scoped_lock lock(mutex_);
        engine_.refuse = std::move(f);
    }

    // Sends @p m to every client that asked for notifications.
    void broadcast(const protocol::message& m) {
        std::scoped_lock lock(mutex_);
        send(session_.broadcast(m));
    }

    std::string ir(std::int32_t plan) {
        std::scoped_lock lock(mutex_);
        const auto it = engine_.plans.find(plan);
        return it == engine_.plans.end() ? std::string() : it->second;
    }

private:
    void send(const std::vector<protocol::outgoing>& out) {
        for (const auto& o : out) socket_.send(protocol::encode(o.msg), o.to);
    }

    void loop(const espresso::stop_token& stop) {
        auto nextCounters = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            auto d = socket_.receive(5);
            std::scoped_lock lock(mutex_);
            if (d) send(session_.handleDatagram(d->bytes, d->from));
            for (auto plan : std::exchange(engine_.pending, {})) {
                engine_.active = plan;
                send(session_.broadcast({.address = "/plan.active", .args = {plan}}));
            }
            for (auto plan : std::exchange(engine_.retired, {})) {
                send(session_.broadcast({.address = "/plan.retired", .args = {plan}}));
            }
            if (std::chrono::steady_clock::now() >= nextCounters) {
                send(session_.countersMessages());
                nextCounters += std::chrono::milliseconds(100);
            }
        }
    }

    immediateEngine engine_;
    protocol::serverSession session_;
    protocol::udpSocket socket_;
    std::mutex mutex_;
    espresso::jthread thread_;  // last: stops and joins before the members it uses go away
};

}  // namespace support
