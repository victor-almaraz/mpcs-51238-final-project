// Determinism across thread counts and orders (threads.md §1.3, §5.5,
// §9.1): every example, the load piece among them, fed scripted input,
// renders the same bits on every device output at 1, 2, and 4 threads,
// with the finest partition at 4, and in two shuffled orders of its
// dependencies on one thread.

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
#include "support/pages.h"
#include "support/unwrap.h"

namespace {

constexpr double rate = 48000.0;
// A quarter second; the load piece, which takes 40 times as long a block as
// the next example in Debug builds, a tenth.
constexpr auto frames = static_cast<std::size_t>(0.25 * rate);
constexpr auto loadPieceFrames = static_cast<std::size_t>(0.1 * rate);

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

// Two device inputs of seeded noise, for the length of a render.
const std::vector<std::vector<float>>& noiseInputs() {
    static const auto inputs = [] {
        std::vector<std::vector<float>> in(2, std::vector<float>(frames));
        primitives::rng random(7);
        for (auto& channel : in) {
            for (float& x : channel) x = 0.25f * random.bipolar();
        }
        return in;
    }();
    return inputs;
}

// What the page of example @p name sends after the first block, so that
// the examples that wait for their pages play: mini-clone's panel, the
// rack's knobs and the cables of a played key, with VCO 2 modulating VCO 1
// through a feedback pipe, and a note from page-keys.
std::vector<protocol::message> pageScript(const std::string& name) {
    std::vector<protocol::message> out;
    const auto send = [&](const std::vector<support::pageValue>& values) {
        for (const auto& [address, value] : values) out.push_back({.address = address, .args = {value}});
    };
    if (name == "mini-clone") send(support::miniClonePanel());
    if (name == "rack") {
        send(support::rackKnobs());
        auto cables = support::rackKeyPatch();
        cables.emplace_back("vco2-tri", "vco1-fm");
        for (const auto& [from, to] : cables)
            out.push_back({.address = support::rackCable(from, to), .args = {1.0f}});
        send({{"/vco1/fm", 0.3f}});
    }
    if (name == "page-keys") {
        send({{"/cutoff", 0.8f}});
        out.push_back({.address = "/note", .args = {60.0f, 100.0f}});
    }
    return out;
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
            const auto d = std::ranges::find(g.pipes, in->second.front(), &metadata::pipeDecl::pipe);
            if (d != g.pipes.end()) channels = d->channels;
        }
        outputs = std::max(outputs, first + channels);
    }
    return outputs;
}

// True when @p g reads MIDI or the device's audio inputs.
bool readsInput(const metadata::graphSpec& g) {
    return std::ranges::any_of(g.nodes,
                               [](const auto& n) { return n.type == "midiIn" || n.type == "audioIn"; });
}

bool silent(const std::vector<std::vector<float>>& out) {
    return std::ranges::all_of(
        out, [](const auto& c) { return std::ranges::all_of(c, [](float x) { return x == 0.0f; }); });
}

// @p g, the example in folder @p root, run as @p s says, with the
// keyboard's messages, its page's script, and noise on the device inputs.
std::vector<std::vector<float>> render(const metadata::graphSpec& g, const std::filesystem::path& root,
                                       const schedule& s) {
    auto options = support::exampleOptions(root, s.threads);
    options.minimumTaskCost = s.minimumTaskCost;
    options.shuffleSeed = s.shuffleSeed;
    primitives::engine e(std::move(options));
    const auto name = root.filename().string();
    const auto page = pageScript(name);
    std::size_t blocks = 0;
    const auto deliver = [&] {
        ++blocks;
        if (blocks == 1 && g.pagePort) {
            for (const auto& m : page) {
                e.io().deliverOsc(static_cast<std::uint16_t>(*g.pagePort),
                                  support::unwrap(primitives::oscPacket::of(protocol::encode(m))));
            }
        }
        for (const auto& m : keyboard) {
            if (m.block == blocks) e.io().deliverMidi("Keyboard", {.time = 0.0, .size = 3, .bytes = m.bytes});
        }
    };
    return server::renderOffline(e, e.build(g, 1), name == "straintest" ? loadPieceFrames : frames,
                                 outputsOf(g), 0.0, {}, deliver, noiseInputs());
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
        if (!silent(one)) ++sounding;
        // Only the web and serial examples, whose sources keep their
        // initial values offline, may be silent.
        if (readsInput(g)) CHECK_FALSE(silent(one));
        for (const auto& s : schedules) {
            INFO(s.threads << " threads, least task cost " << s.minimumTaskCost << " ns"
                           << (s.shuffleSeed ? ", shuffled" : ""));
            CHECK(support::bitDifference(render(g, path.parent_path(), s), one) == "");
        }
    }
    CHECK(sounding > 0);
}
