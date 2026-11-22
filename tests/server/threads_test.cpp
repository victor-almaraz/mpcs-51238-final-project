// Determinism across thread counts and orders (threads.md §1.3, §5.5,
// §9.1): every example, the load piece among them, fed scripted input,
// renders the same bits on every device output at 1, 2, and 4 threads,
// with the finest partition at 4, and in two shuffled orders of its
// dependencies on one thread. A schedule that is not parallel runs on the
// audio thread alone at any count, so its renders at 2 and 4 threads
// would repeat the one at 1, and are left out.

#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/io.h>
#include <espresso/primitives/plan.h>
#include <espresso/primitives/random.h>
#include <espresso/protocol/osc.h>

#include "offline.h"
#include "support/bits.h"
#include "support/examples.h"
#include "support/graph.h"
#include "support/pages.h"

namespace {

// A render's length in blocks: a budget of node-blocks over the plan's
// nodes, between the least and the most. The budget gives every example
// but the load piece the most, a quarter second of 256-sample blocks at
// 48 kHz, and the load piece, three times the rack's 875 nodes, 15 blocks.
constexpr std::size_t nodeBlocks = 42'000;
constexpr std::size_t leastBlocks = 10;
constexpr std::size_t mostBlocks = 47;

// How a render runs its nodes.
struct schedule {
    int threads = 1;
    double minimumTaskCost = primitives::defaultMinimumTaskCost;
    std::optional<std::uint64_t> shuffleSeed = std::nullopt;
};

// A MIDI message delivered after @p block blocks, as a keyboard sends it.
struct midiAt {
    std::size_t block = 0;
    std::array<std::uint8_t, 3> bytes{};
};

// Keys, a sustain pedal, and their releases, from the second block: the
// plan's inputs open when it becomes active, in the first.
constexpr std::array keyboard{
    midiAt{.block = 1, .bytes = {0x90, 60, 100}}, midiAt{.block = 3, .bytes = {0xB0, 64, 127}},
    midiAt{.block = 8, .bytes = {0x90, 64, 90}},  midiAt{.block = 14, .bytes = {0x90, 67, 80}},
    midiAt{.block = 20, .bytes = {0x80, 60, 0}},  midiAt{.block = 30, .bytes = {0xB0, 64, 0}},
    midiAt{.block = 36, .bytes = {0x80, 64, 0}},  midiAt{.block = 36, .bytes = {0x80, 67, 0}},
};

// Two device inputs of seeded noise, @p frames long.
std::vector<std::vector<float>> noise(std::size_t frames) {
    std::vector<std::vector<float>> in(2, std::vector<float>(frames));
    primitives::rng random(7);
    for (auto& channel : in) {
        for (float& x : channel) x = 0.25f * random.bipolar();
    }
    return in;
}

// What the page of example @p name sends after the first block, so that
// the examples that wait for their pages play: mini-clone's panel, the
// rack's knobs and the cables of a played key, with VCO 2 modulating VCO 1
// through a feedback pipe, and a note from page-keys.
std::vector<protocol::message> pageScript(const std::string& name) {
    if (name == "mini-clone") return support::pageMessages(support::miniClonePanel());
    if (name == "rack") {
        auto out = support::pageMessages(support::rackKnobs());
        auto cables = support::rackKeyPatch();
        cables.emplace_back("vco2-tri", "vco1-fm");
        for (const auto& [from, to] : cables)
            out.push_back({.address = support::rackCable(from, to), .args = {1.0f}});
        out.push_back({.address = "/vco1/fm", .args = {0.3f}});
        return out;
    }
    if (name == "page-keys")
        return {{.address = "/cutoff", .args = {0.8f}}, {.address = "/note", .args = {60.0f, 100.0f}}};
    return {};
}

// The device outputs the audioOut nodes of @p g write: past the last
// channel any of them reaches, at least one.
int outputsOf(const metadata::graphSpec& g) {
    int outputs = 1;
    for (const auto& n : g.nodes) {
        if (n.type != "audioOut") continue;
        int first = 0;
        if (const auto c = n.params.find("channel"); c != n.params.end()) {
            if (const auto v = metadata::as<double>(c->second)) first = static_cast<int>(v->get());
        }
        int channels = 1;
        if (const auto in = n.inputs.find("in"); in != n.inputs.end() && !in->second.empty()) {
            if (const auto d = metadata::findPipe(g, in->second.front())) channels = d->get().channels;
        }
        outputs = std::max(outputs, first + channels);
    }
    return outputs;
}

// True when @p g reads MIDI, the device's audio inputs, or a page.
bool readsInput(const metadata::graphSpec& g) {
    return g.pagePort.has_value() || std::ranges::any_of(g.nodes, [](const auto& n) {
               return n.type == "midiIn" || n.type == "audioIn";
           });
}

// A render of an example: its device outputs, and whether its schedule is
// parallel.
struct rendering {
    std::vector<std::vector<float>> outputs;
    bool parallel = false;
};

// @p g, the example in folder @p root, run as @p s says, with the
// keyboard's messages, its page's script, and noise on the device inputs.
rendering render(const metadata::graphSpec& g, const std::filesystem::path& root, const schedule& s) {
    auto options = support::exampleOptions(root, s.threads);
    options.schedule = {.minimumTaskCost = s.minimumTaskCost, .shuffleSeed = s.shuffleSeed};
    primitives::engine e(std::move(options));
    const auto plan = e.build(g, 1);
    const auto page = pageScript(root.filename().string());
    std::size_t blocks = 0;
    const auto deliver = [&] {
        ++blocks;
        if (blocks == 1 && g.pagePort) {
            for (const auto& m : page)
                support::deliverPage(e.io(), static_cast<std::uint16_t>(*g.pagePort), m);
        }
        for (const auto& m : keyboard) {
            if (m.block == blocks) e.io().deliverMidi("Keyboard", {.time = 0.0, .size = 3, .bytes = m.bytes});
        }
    };
    const auto length =
        std::clamp(nodeBlocks / std::max<std::size_t>(plan->nodes.size(), 1), leastBlocks, mostBlocks);
    const auto frames = length * static_cast<std::size_t>(e.info().blockSize);
    const bool parallel = plan->schedule.tasks.parallel;
    return {.outputs = server::renderOffline(e, plan, frames, outputsOf(g), 0.0, {}, deliver, noise(frames)),
            .parallel = parallel};
}

}  // namespace

TEST_CASE("renders compare by their bits: -0 differs from +0 and the first difference is named") {
    CHECK(support::bitDifference(std::vector<float>{-0.0f}, std::vector<float>{0.0f}) != "");
    const std::vector<std::vector<float>> a{{0.0f, 1.0f}, {0.5f, 0.25f}};
    auto b = a;
    b[1][1] = 0.125f;
    CHECK(support::bitDifference(a, a) == "");
    CHECK_THAT(support::bitDifference(b, a), Catch::Matchers::StartsWith("channel 1, sample 1: 0x3e000000"));
    CHECK(support::bitDifference(std::vector<std::vector<float>>{{0.0f}}, a) != "");
}

TEST_CASE("every example fed input renders the same bits on 1 to 4 threads and in shuffled orders") {
    const auto paths = support::exampleIrFiles();
    REQUIRE(paths.size() >= 14);
    const std::uint64_t seed = Catch::getSeed();  // --rng-seed repeats the orders
    INFO("shuffle seeds " << seed << " and " << seed + 1);
    const std::array schedules{
        schedule{.threads = 2},
        schedule{.threads = 4},
        schedule{.threads = 4, .minimumTaskCost = 0.0},
        schedule{.threads = 1, .shuffleSeed = seed},
        schedule{.threads = 1, .shuffleSeed = seed + 1},
    };
    std::size_t sounding = 0;
    for (const auto& path : paths) {
        INFO(path.filename().string());
        const auto g = support::irGraph(path);
        const auto one = render(g, path.parent_path(), {});
        const bool silent =
            std::ranges::all_of(one.outputs, [](const auto& c) { return support::peak(c) == 0.0; });
        if (!silent) ++sounding;
        // Only the web and serial examples, whose sources keep their
        // initial values offline, may be silent.
        if (readsInput(g)) CHECK_FALSE(silent);
        for (const auto& s : schedules) {
            if (!one.parallel && s.threads > 1 && s.minimumTaskCost == primitives::defaultMinimumTaskCost)
                continue;  // on the audio thread alone, as at one thread
            INFO(s.threads << " threads, least task cost " << s.minimumTaskCost << " ns"
                           << (s.shuffleSeed ? ", shuffled" : ""));
            CHECK(support::bitDifference(render(g, path.parent_path(), s).outputs, one.outputs) == "");
        }
    }
    CHECK(sounding > 0);
}
