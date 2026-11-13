// Stub server: speaks the client-server protocol (protocol.md) without an
// audio engine. It accepts IR, schedules swaps on a 120 BPM 4/4 clock,
// reports plan activation and retirement, and sends zeroed counters. It
// produces no sound. Use it to exercise the client's run controls.
//
// Usage: proj-stub-server [--port N] [--quiet]

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <proj/metadata/version.h>
#include <proj/protocol/messages.h>
#include <proj/protocol/serverSession.h>
#include <proj/protocol/udp.h>

namespace {

using clock_type = std::chrono::steady_clock;
using protocol::message;

std::atomic<bool> interrupted{false};

void onSignal(int) { interrupted = true; }

class stubEngine : public protocol::engine {
public:
    explicit stubEngine(clock_type::time_point start) : start_(start) {}

    std::optional<std::string> loadPlan(std::int32_t planId, std::string ir) override {
        const auto first = ir.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "IR is empty";
        if (ir[first] != '{') return "IR is not a JSON object";
        plans_[planId] = ir.size();
        return std::nullopt;
    }

    bool hasPlan(std::int32_t planId) const override { return plans_.count(planId) != 0; }

    void play(std::int32_t planId, float fade, std::string_view quant) override {
        const double period = quant == "beat" ? secondsPerBeat : quant == "bar" ? 4 * secondsPerBeat : 0.0;
        auto at = clock_type::now();
        if (period > 0.0) {
            const double elapsed = std::chrono::duration<double>(at - start_).count();
            const double next = std::ceil(elapsed / period) * period;
            at = start_ + std::chrono::duration_cast<clock_type::duration>(std::chrono::duration<double>(next));
        }
        swap_ = pendingSwap{planId, at, fade};
    }

    void stop(float fade) override {
        swap_.reset();
        if (active_ != 0) retire(active_, fade);
        active_ = 0;
    }

    void setControl(std::int32_t index, float value) override { controls_[index] = value; }
    float control(std::int32_t index) const override {
        const auto it = controls_.find(index);
        return it == controls_.end() ? 0.0f : it->second;
    }

    protocol::serverCounters counters() const override {
        protocol::serverCounters c;
        c.activePlan = active_;
        return c;
    }

    void quit() override { quit_ = true; }
    bool quitRequested() const noexcept { return quit_; }

    // Messages due at `now`, for every subscribed client.
    std::vector<message> tick(clock_type::time_point now) {
        std::vector<message> out;
        if (swap_ && swap_->at <= now) {
            const pendingSwap s = *swap_;
            swap_.reset();
            if (active_ != 0 && active_ != s.plan) retire(active_, s.fade);
            active_ = s.plan;
            out.push_back({std::string(protocol::address::planActive), {s.plan}});
        }
        for (auto it = retiring_.begin(); it != retiring_.end();) {
            if (it->second <= now) {
                plans_.erase(it->first);
                out.push_back({std::string(protocol::address::planRetired), {it->first}});
                it = retiring_.erase(it);
            } else {
                ++it;
            }
        }
        return out;
    }

private:
    static constexpr double secondsPerBeat = 60.0 / 120.0;

    struct pendingSwap {
        std::int32_t plan;
        clock_type::time_point at;
        float fade;
    };

    void retire(std::int32_t plan, float fade) {
        retiring_[plan] = clock_type::now() + std::chrono::duration_cast<clock_type::duration>(
                                                  std::chrono::duration<double>(fade));
    }

    clock_type::time_point start_;
    std::map<std::int32_t, std::size_t> plans_;
    std::map<std::int32_t, clock_type::time_point> retiring_;
    std::map<std::int32_t, float> controls_;
    std::optional<pendingSwap> swap_;
    std::int32_t active_ = 0;
    bool quit_ = false;
};

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
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage(std::cout);
            return 0;
        }
        if (arg == "--quiet") {
            quiet = true;
        } else if (arg == "--port" && i + 1 < argc) {
            const long p = std::strtol(argv[++i], nullptr, 10);
            if (p <= 0 || p > 65535) {
                std::cerr << "proj-stub-server: invalid port " << argv[i] << '\n';
                return 2;
            }
            port = static_cast<std::uint16_t>(p);
        } else {
            usage(std::cerr);
            return 2;
        }
    }

    std::string error;
    auto socket = protocol::udpSocket::bind(port, &error);
    if (!socket) {
        std::cerr << "proj-stub-server: cannot listen on 127.0.0.1:" << port << ": " << error << '\n';
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    const auto start = clock_type::now();
    stubEngine engine(start);
    protocol::serverSession session(
        {"proj-stub-server " PROJ_VERSION, std::string(metadata::version), 48000, 256}, engine);
    std::cout << "proj-stub-server " << PROJ_VERSION << " listening on 127.0.0.1:" << port
              << " (metadata " << metadata::version << "; no audio)" << std::endl;

    auto log = [&](std::string_view direction, protocol::endpoint peer, const message& m) {
        if (quiet || m.address == protocol::address::counters) return;
        std::cout << timestamp(start) << ' ' << direction << " :" << peer.port << ' '
                  << protocol::describe(m) << std::endl;
    };
    auto sendAll = [&](const std::vector<protocol::outgoing>& out) {
        for (const auto& o : out) {
            socket->send(protocol::encode(o.msg), o.to);
            log("->", o.to, o.msg);
        }
    };

    auto nextCounters = clock_type::now();
    while (!interrupted && !engine.quitRequested()) {
        if (auto d = socket->receive(5)) {
            if (const auto m = protocol::decode(d->bytes)) {
                log("<-", d->from, *m);
            } else if (!quiet) {
                std::cout << timestamp(start) << " dropped malformed datagram from :" << d->from.port
                          << std::endl;
            }
            sendAll(session.handleDatagram(d->bytes, d->from));
        }
        const auto now = clock_type::now();
        for (const auto& m : engine.tick(now)) sendAll(session.broadcast(m));
        if (now >= nextCounters) {
            sendAll(session.countersMessages());
            nextCounters = now + std::chrono::milliseconds(protocol::countersIntervalMs);
        }
    }
    std::cout << "proj-stub-server: exiting" << std::endl;
    return 0;
}
