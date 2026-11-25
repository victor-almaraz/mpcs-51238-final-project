#include <optional>
#include <string>
#include <utility>

#include <espresso/workspace/consoleLog.h>

namespace workspace {

void consoleLog::add(consoleKind kind, std::string text, std::optional<sourceLocation> location) {
    entries_.push_back({.kind = kind, .text = std::move(text), .location = std::move(location)});
    while (entries_.size() > capacity_) entries_.pop_front();
}

}  // namespace workspace
