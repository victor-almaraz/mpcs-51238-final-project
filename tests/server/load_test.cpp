// The load piece of the v2 load test, examples/straintest (threads.md §1.2,
// §11 step 1): it compiles, keeps its 64 voices sounding, and profiles by
// node. Hidden benches time it by thread count, task size, and worker
// spin, and every example in plan order and in parallel (§6.1.1).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numeric>
#include <ratio>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/resource.h>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/plan.h>
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
    const auto& g = support::loadPiece();
    CHECK(support::count(g, "voiceSplit") == 8);
    CHECK(support::count(g, "noteToFreq") == 64);  // a voice reads its notes' pitch once
    // Eight layers a voice: a ladder each in the three stacks, a comb each in the two strings.
    CHECK(support::count(g, "ladder") == 3 * 8 * 8);
    CHECK(support::count(g, "comb") >= 2 * 8 * 8);

    // On the audio thread alone, and with workers, which time the nodes
    // they run: with workers on one layer a voice, whose node types are the
    // same, at a cost the Debug suite can build again.
    for (const int threads : {1, 4}) {
        INFO(threads << " threads");
        support::renderer r({.audio = {.sampleRate = rate, .blockSize = block},
                             .pipes = {},
                             .profile = true,
                             .threads = threads},
                            2);
        const auto plan = r.engine.build(threads == 1 ? g : support::loadPiece(1), 1);
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
        for (const auto n : t.nodes.of(i)) cost[i] += plan->info[n].cost;
    }
    std::ranges::sort(cost);
    std::cout << "tasks " << t.size() << " of " << plan->nodes.size() << " nodes; estimated work "
              << t.serialWork / 1000.0 << " us a block in order, " << t.work / 1000.0
              << " us as tasks; median task " << cost[cost.size() / 2] / 1000.0 << " us, largest "
              << cost.back() / 1000.0 << " us by its nodes; critical path " << t.criticalPath / 1000.0
              << " us (work / path " << t.work / t.criticalPath << ")\n";
    CHECK(t.criticalPath > 0.0);
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

// Block times against the period, in percent: the median, the 99th
// percentile, and the largest.
struct blockTimes {
    double median = 0.0;
    double p99 = 0.0;
    double max = 0.0;
};

// The figures of @p blocks, block times in microseconds, against a period
// of @p period microseconds.
static blockTimes timesOf(std::vector<double> blocks, double period) {
    std::ranges::sort(blocks);
    auto at = [&](double q) {
        return blocks[static_cast<std::size_t>(q * static_cast<double>(blocks.size() - 1))] / period * 100.0;
    };
    return {.median = at(0.5), .p99 = at(0.99), .max = at(1.0)};
}

// Times every block of @p seconds of the load piece on @p threads threads,
// its tasks grouped at @p minimum nanoseconds a task, and prints the task
// count, the total, and the median, 99th percentile, and largest block
// time against the period (threads.md §11 step 7).
static blockTimes benchBlocks(int threads, double minimum, double seconds, std::size_t round) {
    primitives::engine e({.audio = {.sampleRate = rate, .blockSize = block},
                          .pipes = {},
                          .threads = threads,
                          .schedule = {.minimumTaskCost = minimum}});
    const auto plan = e.build(support::loadPiece(), 1);
    const auto& schedule = plan->schedule;
    std::vector<double> blocks;
    auto last = std::chrono::steady_clock::now();
    const auto start = last;
    server::renderOffline(e, plan, static_cast<std::size_t>(seconds * rate), 2, [&] {
        const auto now = std::chrono::steady_clock::now();
        blocks.push_back(std::chrono::duration<double, std::micro>(now - last).count());
        last = now;
    });
    const double total = std::chrono::duration<double>(last - start).count();
    const auto firstSecond = static_cast<std::ptrdiff_t>(rate / block);
    const double later = *std::ranges::max_element(blocks.begin() + firstSecond, blocks.end());
    const auto worstAt = std::ranges::max_element(blocks) - blocks.begin();
    const double period = 1e6 * block / rate;
    const blockTimes out = timesOf(blocks, period);
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
        std::cout << "bench blocks summary threads " << configurations[c].threads << " minimum "
                  << configurations[c].minimum / 1000.0 << " us, " << runs[c].size()
                  << " rounds, % of the period, median (range): median "
                  << support::spreadOf(runs[c], &blockTimes::median) << " p99 "
                  << support::spreadOf(runs[c], &blockTimes::p99) << " max "
                  << support::spreadOf(runs[c], &blockTimes::max) << "\n";
    }
    CHECK(true);
}

// One run of the paced bench, from the second second on.
struct pacedTimes : blockTimes {
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
    const std::vector<double> steady(blocks.begin() + skip, blocks.end());
    const double p = 1e6 * block / rate;
    pacedTimes out{timesOf(steady, p)};
    out.over = static_cast<std::size_t>(std::ranges::count_if(steady, [&](double t) { return t > p; }));
    out.blocks = steady.size();
    out.cpu = cpu / seconds;
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
        std::size_t over = 0;
        std::size_t blocks = 0;
        for (const auto& r : runs[c]) {
            over += r.over;
            blocks += r.blocks;
        }
        std::cout << "bench paced summary threads " << configurations[c].threads << " spin "
                  << configurations[c].spin.count() << " us, " << runs[c].size()
                  << " rounds, % of the period, median (range): median "
                  << support::spreadOf(runs[c], &pacedTimes::median) << " p99 "
                  << support::spreadOf(runs[c], &pacedTimes::p99) << " max "
                  << support::spreadOf(runs[c], &pacedTimes::max) << " over " << over << " of " << blocks
                  << " in all cpu " << support::spreadOf(runs[c], &pacedTimes::cpu) << " s a second\n";
    }
    CHECK(true);
}

// An example for the bench below: its name, its folder, and its graph.
struct benchExample {
    std::string name;
    std::filesystem::path root;
    metadata::graphSpec graph;
};

// Every example: those with an IR file, the load piece among them, and the
// projects without one, compiled from their main.lisp; sorted by name.
static std::vector<benchExample> benchExamples() {
    std::vector<benchExample> out;
    for (const auto& path : support::exampleIrFiles()) {
        out.push_back({.name = path.parent_path().filename().string(),
                       .root = path.parent_path(),
                       .graph = support::irGraph(path)});
    }
    for (const auto& root : support::exampleProjects()) {
        const auto name = root.filename().string();
        if (std::ranges::any_of(out, [&](const auto& b) { return b.name == name; })) continue;
        out.push_back({.name = name, .root = root, .graph = support::projectGraph(root)});
    }
    std::ranges::sort(out, {}, &benchExample::name);
    return out;
}

// The median and mean block time of one render, in microseconds.
struct renderTimes {
    double median = 0.0;
    double mean = 0.0;
};

// Renders @p seconds of example @p x on @p threads threads at the least
// parallel saving @p saving, with no input, as `espresso-server --render`
// does, and times each block.
static renderTimes benchRender(const benchExample& x, int threads, double saving, double seconds) {
    auto options = support::exampleOptions(x.root, threads);
    options.schedule.minimumParallelSaving = saving;
    primitives::engine e(std::move(options));
    const auto plan = e.build(x.graph, 1);
    std::vector<double> blocks;
    auto last = std::chrono::steady_clock::now();
    server::renderOffline(e, plan, static_cast<std::size_t>(seconds * rate), 2, [&] {
        const auto now = std::chrono::steady_clock::now();
        blocks.push_back(std::chrono::duration<double, std::micro>(now - last).count());
        last = now;
    });
    const double mean =
        std::accumulate(blocks.begin(), blocks.end(), 0.0) / static_cast<double>(blocks.size());
    return {.median = support::spreadOf(blocks).median, .mean = mean};
}

TEST_CASE("bench: every example in plan order and in parallel against the estimated saving", "[.bench]") {
    // Plan order and parallel tasks at 2 and 4 threads, forced by the
    // least saving: infinite never runs tasks, 0 always does when two can
    // run at once.
    struct configuration {
        int threads;
        bool parallel;
    };
    constexpr std::array configurations{
        configuration{.threads = 2, .parallel = false}, configuration{.threads = 2, .parallel = true},
        configuration{.threads = 4, .parallel = false}, configuration{.threads = 4, .parallel = true}};
    constexpr double never = std::numeric_limits<double>::infinity();
    constexpr double seconds = 3.0;
    constexpr std::size_t rounds = 5;
    for (const auto& x : benchExamples()) {
        // The schedule at 4 threads, forced: the tasks and their estimates
        // are the same at every thread count.
        auto options = support::exampleOptions(x.root, 4);
        options.schedule.minimumParallelSaving = 0.0;
        primitives::engine e(std::move(options));
        const auto plan = e.build(x.graph, 1);
        const auto& t = plan->schedule.tasks;
        double sums = 0.0;
        for (const auto& info : plan->info) {
            if (info.sums()) sums += info.cost;
        }
        // Whether the default least saving runs the tasks in parallel.
        const auto byDefault = [&](int threads) {
            return t.parallel && t.savingAt(threads) >= primitives::defaultMinimumParallelSaving ? "parallel"
                                                                                                 : "serial";
        };
        std::cout << "bench parallel " << x.name << " nodes " << plan->nodes.size() << " tasks " << t.size()
                  << " can-run-at-once " << t.parallel << " serial work " << t.serialWork / 1000.0
                  << " us work " << t.work / 1000.0 << " us path " << t.criticalPath / 1000.0 << " us sums "
                  << sums / 1000.0 << " us privates " << plan->privatePipes.size() << " saving2 "
                  << t.savingAt(2) / 1000.0 << " us saving4 " << t.savingAt(4) / 1000.0 << " us default2 "
                  << byDefault(2) << " default4 " << byDefault(4) << "\n";
        if (!t.parallel) continue;  // a chain: plan order at every setting
        std::vector<std::vector<renderTimes>> runs(configurations.size());
        support::inRotation(configurations.size(), rounds, [&](std::size_t round, std::size_t c) {
            const auto& k = configurations[c];
            const auto r = benchRender(x, k.threads, k.parallel ? 0.0 : never, seconds);
            std::cout << "bench parallel round " << round + 1 << " " << x.name << " threads " << k.threads
                      << (k.parallel ? " parallel" : " serial") << " median " << r.median << " us mean "
                      << r.mean << " us\n";
            runs[c].push_back(r);
        });
        for (std::size_t c = 0; c < configurations.size(); ++c) {
            const auto& k = configurations[c];
            std::cout << "bench parallel summary " << x.name << " threads " << k.threads
                      << (k.parallel ? " parallel" : " serial") << " saving "
                      << t.savingAt(k.threads) / 1000.0 << " us, " << runs[c].size()
                      << " rounds, block us, median (range): median "
                      << support::spreadOf(runs[c], &renderTimes::median) << " mean "
                      << support::spreadOf(runs[c], &renderTimes::mean) << "\n";
        }
    }
    CHECK(true);
}
