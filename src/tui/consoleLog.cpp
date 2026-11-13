#include <proj/tui/consoleLog.h>

namespace tui {

void consoleLog::add(consoleKind kind, std::string text, std::optional<sourceLocation> location) {
    entries_.push_back({kind, std::move(text), std::move(location)});
    while (entries_.size() > capacity_) entries_.pop_front();
}

} // namespace tui
