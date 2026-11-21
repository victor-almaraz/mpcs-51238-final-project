// The dependencies of every example's plan, and of crossfades between
// plans, order every pair of conflicting nodes, and their tasks keep the
// dependencies (threads.md §4.1, §5, §9.2).

#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>
#include <espresso/primitives/buffer.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/plan.h>
#include <espresso/primitives/primitive.h>

#include "../../src/primitives/transition.h"
#include "samples.h"
#include "support/dependencies.h"
#include "support/tasks.h"
#include "support/unwrap.h"

namespace {

using infoList = std::vector<std::reference_wrapper<const primitives::nodeInfo>>;

// Every example's IR file, sorted.
std::vector<std::filesystem::path> examples() {
    std::vector<std::filesystem::path> out;
    for (const auto& e : std::filesystem::recursive_directory_iterator(
             std::filesystem::path(ESPRESSO_SOURCE_DIR) / "examples")) {
        if (e.path().string().ends_with(".ir.json")) out.push_back(e.path());
    }
    std::ranges::sort(out);
    return out;
}

// A plan and the engine that built it, which outlives it: nodes refer to
// their engine's I/O hub.
struct built {
    std::unique_ptr<primitives::engine> engine;
    metadata::graphSpec graph;
    std::shared_ptr<primitives::plan> plan;
};

// The plan of the IR file @p path, built by a new engine that loads
// samples from the file's folder.
built build(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    const auto ir = ir::read(text.str());
    INFO(ir.text());
    REQUIRE(ir.ok());
    built b{.engine = std::make_unique<primitives::engine>(primitives::engineOptions{
                // The server's video pipes, at a small frame size.
                .audio = {.sampleRate = 48000.0, .blockSize = 256, .frameWidth = 64, .frameHeight = 36},
                .pipes = {.video = 16},
                .loadImage = nullptr,
                .loadSample =
                    server::sampleLoaderFor(path.parent_path(), std::make_shared<server::sampleCache>()),
                .bufferMemory = primitives::bufferPool::defaultLimit}),
            .graph = {},
            .plan = nullptr};
    b.graph = support::unwrap(ir.graph);
    b.plan = b.engine->build(b.graph, 1);
    return b;
}

// The node information of @p t's merged order.
infoList fadeOrder(const primitives::transition& t) {
    std::map<std::shared_ptr<primitives::primitive>, std::reference_wrapper<const primitives::nodeInfo>,
             std::owner_less<>>
        info;
    for (const auto& p : {t.incoming, t.outgoing}) {
        if (!p) continue;
        for (std::size_t i = 0; i < p->nodes.size(); ++i) info.emplace(p->nodes[i], p->info[i]);
    }
    infoList out;
    for (const auto& e : t.order) out.push_back(info.at(e.node));
    return out;
}

// The feedback pipes during @p t's fade, as the engine configures them.
std::vector<metadata::pipeRef> fadeFeedback(const primitives::transition& t) {
    std::map<metadata::pipeRef, bool> declared;
    for (const auto& p : {t.outgoing, t.incoming}) {
        if (!p) continue;
        for (const auto& d : p->pipes) declared[d.pipe] = d.feedback;
    }
    std::vector<metadata::pipeRef> out;
    for (const auto& [ref, delayed] : declared) {
        if (delayed) out.push_back(ref);
    }
    return out;
}

// The estimated costs of @p nodes.
std::vector<double> costsOf(const infoList& nodes) {
    std::vector<double> out;
    out.reserve(nodes.size());
    for (const primitives::nodeInfo& n : nodes) out.push_back(n.cost);
    return out;
}

// Checks the dependencies and tasks of @p nodes, with feedback pipes
// @p feedback, and returns the count of conflicting pairs.
std::size_t check(const infoList& nodes, std::span<const metadata::pipeRef> feedback,
                  const primitives::dependencyList& deps, const primitives::taskList& tasks) {
    std::mt19937 random(Catch::getSeed());
    const auto conflicts = support::checkDependencies(nodes, feedback, deps);
    support::checkTasks(deps, tasks, costsOf(nodes), random);
    return conflicts;
}

// Checks the dependencies and tasks of the crossfade from @p from to @p to.
std::size_t checkFade(const std::shared_ptr<primitives::plan>& to,
                      const std::shared_ptr<primitives::plan>& from) {
    const auto t = primitives::makeTransition(to, from, 0.05, primitives::quantization::none, 48000.0);
    return check(fadeOrder(*t), fadeFeedback(*t), t->dependencies, t->tasks);
}

}  // namespace

TEST_CASE("every example's dependencies order its conflicting nodes and its tasks keep them, as do "
          "crossfades into it") {
    const auto paths = examples();
    REQUIRE(paths.size() >= 20);
    std::optional<built> previous;
    std::size_t conflicts = 0;
    for (const auto& path : paths) {
        INFO(path.filename().string());
        built current = build(path);
        const infoList order(current.plan->info.begin(), current.plan->info.end());
        conflicts += check(order, current.plan->feedback, current.plan->dependencies, current.plan->tasks);
        // A swap to the same piece rebuilt by its engine, as after an edit,
        // which adopts its buffers and ports; and one from the example before.
        conflicts += checkFade(current.engine->build(current.graph, 2), current.plan);
        if (previous) conflicts += checkFade(current.plan, previous->plan);
        previous.reset();  // the plan before its engine
        previous.emplace(std::move(current));
    }
    CHECK(conflicts > 10000);
}
