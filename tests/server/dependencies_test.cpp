// The dependencies of every example's plan, and of crossfades between
// plans, order every pair of conflicting nodes, and their tasks keep the
// dependencies (threads.md §4.1, §5, §9.2).

#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/primitives/engine.h>
#include <espresso/primitives/plan.h>
#include <espresso/primitives/primitive.h>

#include "../../src/primitives/transition.h"
#include "support/dependencies.h"
#include "support/examples.h"
#include "support/tasks.h"
#include "support/unwrap.h"

namespace {

using infoList = std::vector<std::reference_wrapper<const primitives::nodeInfo>>;

// A plan, the engine that built it, which outlives it (nodes refer to
// their engine's I/O hub), and the graph it was built from.
struct built {
    std::unique_ptr<primitives::engine> engine;
    std::reference_wrapper<const metadata::graphSpec> graph;
    std::shared_ptr<primitives::plan> plan;
};

// The plan of the IR file @p path, built by a new engine.
built build(const std::filesystem::path& path) {
    built b{.engine = std::make_unique<primitives::engine>(support::exampleOptions(path.parent_path())),
            .graph = support::irGraph(path),
            .plan = nullptr};
    b.plan = b.engine->build(b.graph.get(), 1);
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

// Checks the schedule of @p nodes, with feedback pipes @p feedback, and
// returns the count of conflicting pairs.
std::size_t check(const infoList& nodes, std::span<const metadata::pipeRef> feedback,
                  const primitives::nodeSchedule& schedule) {
    std::mt19937 random(Catch::getSeed());
    const auto conflicts = support::checkDependencies(nodes, feedback, schedule.dependencies);
    support::checkTasks(schedule.dependencies, schedule.tasks, random);
    return conflicts;
}

// Checks the schedule of the crossfade from @p from to @p to, with the
// feedback pipes it runs with.
std::size_t checkFade(const std::shared_ptr<primitives::plan>& to,
                      const std::shared_ptr<primitives::plan>& from) {
    const auto t = primitives::makeTransition(to, from, 0.05, primitives::quantization::none, 48000.0);
    return check(fadeOrder(*t), t->feedback, support::unwrap(t->schedule));
}

}  // namespace

TEST_CASE("every example's dependencies order its conflicting nodes and its tasks keep them, as do "
          "crossfades into it") {
    const auto paths = support::exampleIrFiles();
    REQUIRE(paths.size() >= 14);
    std::optional<built> previous;
    std::size_t conflicts = 0;
    for (const auto& path : paths) {
        INFO(path.filename().string());
        built current = build(path);
        const infoList order(current.plan->info.begin(), current.plan->info.end());
        conflicts += check(order, current.plan->feedback, current.plan->schedule);
        // A swap to the same piece rebuilt by its engine, as after an edit,
        // which adopts its buffers and ports; and one from the example before.
        conflicts += checkFade(current.engine->build(current.graph.get(), 2), current.plan);
        if (previous) conflicts += checkFade(current.plan, previous->plan);
        previous.reset();  // the plan before its engine
        previous.emplace(std::move(current));
    }
    CHECK(conflicts > 10000);
}
