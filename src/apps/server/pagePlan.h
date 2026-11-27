// What the active plan's page port serves besides its page (io.md §12.4,
// canvas.md §5.3): kept beside the plan by the engine host and read by the
// page bridge, which links neither to the other.
#pragma once

#include <set>
#include <string>

namespace server {

/// The names a plan gives its page port.
struct pagePlan {
    /// The addresses its `oscOut` nodes send to the page port: those whose
    /// last message a page that opens later is sent first.
    std::set<std::string> addresses;
    /// The clip banks its `clipPlayer` nodes play: those a page may list
    /// and save clips to.
    std::set<std::string> banks;
};

}  // namespace server
