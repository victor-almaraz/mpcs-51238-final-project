#include <catch2/catch_test_macros.hpp>

#include <proj/metadata/version.h>
#include <proj/protocol/serverSession.h>
#include <proj/protocol/udp.h>
#include <proj/tui/app.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
using namespace tui;

namespace {

// Activates plans as soon as they are played.
class immediateEngine : public protocol::engine {
public:
    std::optional<std::string> loadPlan(std::int32_t planId, std::string ir) override {
        plans[planId] = std::move(ir);
        return std::nullopt;
    }
    bool hasPlan(std::int32_t planId) const override { return plans.count(planId) != 0; }
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
        return c;
    }
    void quit() override {}

    std::map<std::int32_t, std::string> plans;
    std::vector<std::int32_t> pending;
    std::vector<std::int32_t> retired;
    std::int32_t active = 0;
};

// A server session on a loopback socket, served from its own thread.
class testServer {
public:
    explicit testServer(std::string metadataVersion)
        : session_({"test-server", std::move(metadataVersion), 48000, 256}, engine_),
          socket_(*protocol::udpSocket::bind(0)), thread_([this] { loop(); }) {}

    ~testServer() {
        stop_ = true;
        thread_.join();
    }

    std::uint16_t port() const { return socket_.port(); }

    std::string ir(std::int32_t plan) {
        std::lock_guard lock(mutex_);
        const auto it = engine_.plans.find(plan);
        return it == engine_.plans.end() ? std::string() : it->second;
    }

private:
    void send(const std::vector<protocol::outgoing>& out) {
        for (const auto& o : out) socket_.send(protocol::encode(o.msg), o.to);
    }

    void loop() {
        auto nextCounters = std::chrono::steady_clock::now();
        while (!stop_) {
            auto d = socket_.receive(5);
            std::lock_guard lock(mutex_);
            if (d) send(session_.handleDatagram(d->bytes, d->from));
            for (auto plan : std::exchange(engine_.pending, {})) {
                engine_.active = plan;
                send(session_.broadcast({"/plan.active", {plan}}));
            }
            for (auto plan : std::exchange(engine_.retired, {})) {
                send(session_.broadcast({"/plan.retired", {plan}}));
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
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

struct project {
    fs::path root;
    project() {
        std::string tmpl = (fs::temp_directory_path() / "proj-server-XXXXXX").string();
        root = ::mkdtemp(tmpl.data());
        std::ofstream(root / "main.lisp") << "(module main)\n(defpiece main () nil)\n";
        std::ofstream(root / ".settings.lisp") << "(settings)";
    }
    ~project() { fs::remove_all(root); }
    appOptions options(std::uint16_t port) const {
        return {root, {}, root / ".settings.lisp", false, linkOptions{{port, 100, 3}, "test", true}};
    }
};

bool consoleHas(app& a, std::string_view text) {
    a.collectReplies();
    for (const auto& e : a.console().entries()) {
        if (e.text.find(text) != std::string::npos) return true;
    }
    return false;
}

bool waitFor(app& a, std::string_view text) {
    for (int i = 0; i < 200; ++i) {
        if (consoleHas(a, text)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

TEST_CASE("play submits IR and reports the swap (protocol.md 4, 6)", "[tui][server]") {
    testServer server{std::string(metadata::version)};
    project p;
    app a(p.options(server.port()));

    REQUIRE(waitFor(a, "server: connected to test-server"));
    CHECK(a.status().connected);

    a.runCommand("play");
    REQUIRE(waitFor(a, "server: plan 1 active"));
    CHECK(consoleHas(a, "server: plan 1 accepted"));
    CHECK(server.ir(1) == interpreterLink::placeholderIr());
    CHECK(a.status().activePlan == 1);

    // Counters arrive ten times per second and update the status bar.
    for (int i = 0; i < 100 && a.status().xruns != 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        a.collectReplies();
    }
    CHECK(a.status().xruns == 3);

    a.runCommand("stop");
    REQUIRE(waitFor(a, "server: plan 1 retired"));
    CHECK(a.status().activePlan == 0);
}

TEST_CASE("a metadata mismatch blocks IR submission (protocol.md 4.2)", "[tui][server]") {
    testServer server{"other-metadata"};
    project p;
    app a(p.options(server.port()));
    REQUIRE(waitFor(a, "metadata version mismatch"));
    CHECK_FALSE(a.status().connected);
    a.runCommand("play");
    a.link().waitIdle();
    a.collectReplies();
    CHECK(server.ir(1).empty());
}

TEST_CASE("a missing server is reported, not fatal", "[tui][server]") {
    auto silent = protocol::udpSocket::bind(0);  // nobody answers on this port
    REQUIRE(silent);
    project p;
    auto options = p.options(silent->port());
    options.link.server.timeoutMs = 10;
    app a(options);
    REQUIRE(waitFor(a, "server: not reachable at 127.0.0.1:" + std::to_string(silent->port())));
    CHECK_FALSE(a.status().connected);
}
