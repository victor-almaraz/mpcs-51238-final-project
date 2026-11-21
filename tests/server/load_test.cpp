// The load piece of the v2 load test, examples/straintest (threads.md §1.2,
// §11 step 1): it compiles, keeps its 64 voices sounding, and profiles by
// node.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/engine.h>
#include <espresso/protocol/jthread.h>

#include "offline.h"
#include "realtime.h"
#include "support/compile.h"
#include "support/examples.h"
#include "support/graph.h"
#include "support/rounds.h"

using Catch::Matchers::ContainsSubstring;

namespace {

constexpr double rate = 48000.0;
constexpr int block = 256;

}  // namespace

TEST_CASE("the load piece has 64 layered voices through 16 effects, profiled by node") {
    const auto g = support::loadPiece();
    CHECK(support::count(g, "voiceSplit") == 8);
    CHECK(support::count(g, "noteToFreq") == 64);  // a voice reads its notes' pitch once
    // Eight layers a voice: a ladder each in the three stacks, a comb each in the two strings.
    CHECK(support::count(g, "ladder") == 3 * 8 * 8);
    CHECK(support::count(g, "comb") >= 2 * 8 * 8);

    // On the audio thread alone, and with workers, which time the nodes
    // they run.
    for (const int threads : {1, 4}) {
        INFO(threads << " threads");
        support::renderer r({.audio = {.sampleRate = rate, .blockSize = block},
                             .pipes = {},
                             .profile = true,
                             .threads = threads},
                            2);
        const auto plan = r.engine.build(g, 1);
        r.engine.play(plan, 0.0, primitives::quantization::none);
        constexpr int blocks = 8;
        r.run(block * blocks);
        const double total = std::accumulate(plan->seconds.begin(), plan->seconds.end(), 0.0);
        CHECK(total > 0.0);
        const auto report = server::profileReport(*plan, static_cast<double>(blocks * block) / rate);
        CHECK_THAT(report, ContainsSubstring("profile: " + std::to_string(plan->nodes.size()) + " nodes"));
        CHECK_THAT(report, ContainsSubstring("ladder"));
        CHECK_THAT(report, ContainsSubstring("slowest nodes:"));
    }
}

TEST_CASE("the load piece keeps its 64 voices sounding") {
    // One layer a voice: the same notes and voices at a cost the Debug suite can render.
    support::renderer r(2, rate, block);
    const auto plan = r.engine.build(support::loadPiece(1), 1);
    r.engine.play(plan, 0.0, primitives::quantization::none);
    for (int i = 0; i < static_cast<int>(2.5 * rate) / block; ++i) r.run(block);
    // Every voice sounds after two beats at 120 BPM (1 s); the mix holds its level.
    const auto& left = r.out(0);
    CHECK(support::rms(left, left.size() - 24000, left.size()) > 0.02);
}

TEST_CASE("an engine that does not profile leaves the plan's node times at zero") {
    // One layer a voice, at a cost the Debug suite can build twice.
    for (const int threads : {1, 4}) {
        INFO(threads << " threads");
        support::renderer r(
            {.audio = {.sampleRate = rate, .blockSize = block}, .pipes = {}, .threads = threads}, 2);
        const auto plan = r.engine.build(support::loadPiece(1), 1);
        r.engine.play(plan, 0.0, primitives::quantization::none);
        r.run(block * 4);
        CHECK(std::accumulate(plan->seconds.begin(), plan->seconds.end(), 0.0) == 0.0);
    }
}

TEST_CASE("report: the load piece's tasks", "[.bench]") {
    support::renderer r(2, rate, block);
    const auto plan = r.engine.build(support::loadPiece(), 1);
    const auto& t = plan->schedule.tasks;
    // Each task's estimated cost, the sum of its nodes'.
    std::vector<double> cost(t.size(), 0.0);
    for (std::size_t i = 0; i < t.size(); ++i) {
        for (const auto n : t.nodesOf(i)) cost[i] += plan->info[n].cost;
    }
    // Longest path by estimated cost, in an order where every task follows
    // those it waits for.
    std::vector<std::uint32_t> left = t.waits;
    std::vector<double> finish(t.size(), 0.0);
    std::vector<std::uint32_t> ready;
    for (std::uint32_t i = 0; i < t.size(); ++i) {
        if (left[i] == 0) ready.push_back(i);
    }
    double longest = 0.0;
    while (!ready.empty()) {
        const auto i = ready.back();
        ready.pop_back();
        finish[i] += cost[i];
        longest = std::max(longest, finish[i]);
        for (const auto u : t.successorsOf(i)) {
            finish[u] = std::max(finish[u], finish[i]);
            if (--left[u] == 0) ready.push_back(u);
        }
    }
    const double work = std::accumulate(cost.begin(), cost.end(), 0.0);
    auto sorted = cost;
    std::ranges::sort(sorted);
    std::cout << "tasks " << t.size() << " of " << plan->nodes.size() << " nodes; estimated work "
              << work / 1000.0 << " us a block; median task " << sorted[sorted.size() / 2] / 1000.0
              << " us, largest " << sorted.back() / 1000.0 << " us; critical path " << longest / 1000.0
              << " us (work / path " << work / longest << ")\n";
    CHECK(longest > 0.0);
}

// The process's user and system CPU time, in seconds.
static double processSeconds() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    auto secs = [](const timeval& t) {
        return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_usec) * 1e-6;
    };
    return secs(u.ru_utime) + secs(u.ru_stime);
}

// One run of the block-time bench: the block times of a render, against
// the period.
struct blockTimes {
    double median = 0.0;  // percent of the period
    double p99 = 0.0;
    double max = 0.0;
};

// Times every block of @p seconds of the load piece on @p threads threads,
// its tasks grouped at @p minimum nanoseconds a task, and prints the task
// count, the total, and the median, 99th percentile, and largest block
// time against the period (threads.md §11 step 7).
static blockTimes benchBlocks(int threads, double minimum, double seconds, std::size_t round) {
    primitives::engine e({.audio = {.sampleRate = rate, .blockSize = block},
                          .pipes = {},
                          .threads = threads,
                          .minimumTaskCost = minimum});
    const auto plan = e.build(support::loadPiece(), 1);
    const auto& schedule = plan->schedule;
    std::vector<double> blocks;
    auto last = std::chrono::steady_clock::now();
    const auto start = last;
    server::renderOffline(e, plan, static_cast<std::size_t>(seconds * rate), 2, 0.0, {}, [&] {
        const auto now = std::chrono::steady_clock::now();
        blocks.push_back(std::chrono::duration<double, std::micro>(now - last).count());
        last = now;
    });
    const double total = std::chrono::duration<double>(last - start).count();
    const auto firstSecond = static_cast<std::ptrdiff_t>(rate / block);
    const double later = *std::ranges::max_element(blocks.begin() + firstSecond, blocks.end());
    const auto worst = std::ranges::max_element(blocks);
    const auto worstAt = worst - blocks.begin();
    std::ranges::sort(blocks);
    const double period = 1e6 * block / rate;
    auto at = [&](double q) {
        return blocks[static_cast<std::size_t>(q * static_cast<double>(blocks.size() - 1))] / period * 100.0;
    };
    const blockTimes out{.median = at(0.5), .p99 = at(0.99), .max = blocks.back() / period * 100.0};
    std::cout << "bench blocks round " << round + 1 << " threads " << threads << " minimum "
              << minimum / 1000.0 << " us tasks " << schedule.tasks.size() << " total " << total
              << " s median " << out.median << "% p99 " << out.p99 << "% max " << out.max << "% (block "
              << worstAt << ") max after 1 s " << later / period * 100.0 << "%\n";
    return out;
}

TEST_CASE("bench: the control thread's swap to the load piece", "[.bench]") {
    primitives::engine e({.audio = {.sampleRate = rate, .blockSize = block}, .pipes = {}, .threads = 4});
    const auto plan = e.build(support::loadPiece(), 1);
    const auto start = std::chrono::steady_clock::now();
    e.play(plan, 0.0, primitives::quantization::none);
    const std::chrono::duration<double, std::micro> took = std::chrono::steady_clock::now() - start;
    std::cout << "bench swap play() " << took.count() << " us\n";
    std::vector<float> out(block);
    std::vector<std::span<float>> outs{out, out};
    for (int i = 0; i < 3; ++i) {
        const auto b = std::chrono::steady_clock::now();
        e.process({.inputs = {}, .outputs = outs}, block, 0.0);
        const std::chrono::duration<double, std::micro> t = std::chrono::steady_clock::now() - b;
        std::cout << "bench swap block " << i << " " << t.count() << " us\n";
    }
    CHECK(true);
}

// Rounds of each bench below: every configuration runs once a round, in
// an order rotated from round to round (support::inRotation).
constexpr std::size_t benchRounds = 3;

TEST_CASE("bench: the load piece's block times by thread count and task size", "[.bench]") {
    struct configuration {
        int threads;
        double minimum;
    };
    std::vector<configuration> configurations;
    for (const double minimum : {5'000.0, 10'000.0, 20'000.0, 40'000.0, 80'000.0}) {
        for (const int threads : {1, 2, 4})
            configurations.push_back({.threads = threads, .minimum = minimum});
    }
    std::vector<std::vector<blockTimes>> runs(configurations.size());
    support::inRotation(configurations.size(), benchRounds, [&](std::size_t round, std::size_t c) {
        runs[c].push_back(benchBlocks(configurations[c].threads, configurations[c].minimum, 5.0, round));
    });
    for (std::size_t c = 0; c < configurations.size(); ++c) {
        auto figure = [&](double blockTimes::* f) {
            std::vector<double> v;
            for (const auto& r : runs[c]) v.push_back(r.*f);
            return support::spreadOf(v);
        };
        std::cout << "bench blocks summary threads " << configurations[c].threads << " minimum "
                  << configurations[c].minimum / 1000.0 << " us, " << runs[c].size()
                  << " rounds, % of the period, median (range): median " << figure(&blockTimes::median)
                  << " p99 " << figure(&blockTimes::p99) << " max " << figure(&blockTimes::max) << "\n";
    }
    CHECK(true);
}

// One run of the paced bench, from the second second on.
struct pacedTimes {
    double median = 0.0;  // percent of the period
    double p99 = 0.0;
    double max = 0.0;
    std::size_t over = 0;  // blocks over the period
    std::size_t blocks = 0;
    double cpu = 0.0;  // CPU seconds a second of audio
};

// Plays the load piece paced as a device plays it, one block a period on
// a real-time thread, with @p threads threads whose workers spin for
// @p spin, and prints each block's time against the period, from the
// second second on, and the process's CPU time a second of audio.
static pacedTimes benchPaced(int threads, std::chrono::microseconds spin, double seconds, std::size_t round) {
    const auto period =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(block / rate));
    primitives::engine e({.audio = {.sampleRate = rate, .blockSize = block},
                          .pipes = {},
                          .threads = threads,
                          .workerStart = [] { (void)server::makeCurrentThreadRealtimeFor(rate, block); },
                          .workerSpin = spin});
    std::vector<double> blocks;
    const auto blocksTotal = static_cast<std::size_t>(seconds * rate / block);
    blocks.reserve(blocksTotal);
    double cpu = 0.0;
    {
        espresso::jthread device([&] {
            (void)server::makeCurrentThreadRealtimeFor(rate, block);
            e.play(e.build(support::loadPiece(), 1), 0.0, primitives::quantization::none);
            std::vector<float> out(block);
            std::vector<std::span<float>> outs{out, out};
            const double cpuStart = processSeconds();
            auto next = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < blocksTotal; ++i) {
                const auto b = std::chrono::steady_clock::now();
                e.process({.inputs = {}, .outputs = outs}, block, 0.0);
                blocks.push_back(
                    std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - b).count());
                next += period;
                std::this_thread::sleep_until(next);
            }
            cpu = processSeconds() - cpuStart;
        });
    }
    const auto skip =
        static_cast<std::ptrdiff_t>(rate / block);  // the first second: caches and voices warm up
    std::vector<double> steady(blocks.begin() + skip, blocks.end());
    std::ranges::sort(steady);
    const double p = 1e6 * block / rate;
    auto at = [&](double q) {
        return steady[static_cast<std::size_t>(q * static_cast<double>(steady.size() - 1))] / p * 100.0;
    };
    const pacedTimes out{
        .median = at(0.5),
        .p99 = at(0.99),
        .max = steady.back() / p * 100.0,
        .over = static_cast<std::size_t>(std::ranges::count_if(steady, [&](double t) { return t > p; })),
        .blocks = steady.size(),
        .cpu = cpu / seconds};
    std::cout << "bench paced round " << round + 1 << " threads " << threads << " spin " << spin.count()
              << " us median " << out.median << "% p99 " << out.p99 << "% max " << out.max << "% over "
              << out.over << " of " << out.blocks << " cpu " << out.cpu << " s a second\n";
    return out;
}

TEST_CASE("bench: the load piece paced in real time by thread count and worker spin", "[.bench]") {
    struct configuration {
        int threads;
        std::chrono::microseconds spin;
    };
    std::vector<configuration> configurations;
    for (const int threads : {2, 4}) {
        for (const auto spin : {0, 20, 50, 200, 1000, 6000})
            configurations.push_back({.threads = threads, .spin = std::chrono::microseconds(spin)});
    }
    std::vector<std::vector<pacedTimes>> runs(configurations.size());
    support::inRotation(configurations.size(), benchRounds, [&](std::size_t round, std::size_t c) {
        runs[c].push_back(benchPaced(configurations[c].threads, configurations[c].spin, 6.0, round));
    });
    for (std::size_t c = 0; c < configurations.size(); ++c) {
        auto figure = [&](auto pacedTimes::* f) {
            std::vector<double> v;
            for (const auto& r : runs[c]) v.push_back(static_cast<double>(r.*f));
            return support::spreadOf(v);
        };
        std::size_t over = 0;
        std::size_t blocks = 0;
        for (const auto& r : runs[c]) {
            over += r.over;
            blocks += r.blocks;
        }
        std::cout << "bench paced summary threads " << configurations[c].threads << " spin "
                  << configurations[c].spin.count() << " us, " << runs[c].size()
                  << " rounds, % of the period, median (range): median " << figure(&pacedTimes::median)
                  << " p99 " << figure(&pacedTimes::p99) << " max " << figure(&pacedTimes::max) << " over "
                  << over << " of " << blocks << " in all cpu " << figure(&pacedTimes::cpu)
                  << " s a second\n";
    }
    CHECK(true);
}
