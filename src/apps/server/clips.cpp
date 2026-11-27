#include "clips.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <espresso/primitives/clip.h>

#include "samples.h"

namespace server {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view clipsFolder = "clips";
constexpr std::string_view extension = ".mid";

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// The digits that start @p name, and where they end.
std::pair<std::string_view, std::size_t> leadingDigits(std::string_view name) {
    std::size_t n = 0;
    while (n < name.size() && isDigit(name[n])) ++n;
    return {name.substr(0, n), n};
}

}  // namespace

bool isClipBankName(std::string_view bank) {
    return !bank.empty() &&
           std::ranges::all_of(bank, [](char c) { return (c >= 'a' && c <= 'z') || isDigit(c) || c == '-'; });
}

std::optional<int> clipSlotOf(std::string_view name) {
    if (name.size() <= extension.size() || !name.ends_with(extension)) return std::nullopt;
    const auto stem = name.substr(0, name.size() - extension.size());
    const auto [digits, end] = leadingDigits(stem);
    if (digits.empty() || digits.size() > 2) return std::nullopt;
    // The digits stand alone or before a separator.
    if (end < stem.size() && stem[end] != ' ' && stem[end] != '-' && stem[end] != '_') return std::nullopt;
    const int slot = std::stoi(std::string(digits));
    if (slot < 1 || slot > primitives::maxClipSlots) return std::nullopt;
    return slot - 1;
}

std::map<int, fs::path> clipFilesOf(const fs::path& root, std::string_view bank) {
    std::map<int, fs::path> out;
    if (!isClipBankName(bank)) return out;
    std::error_code error;
    const fs::path folder = root / clipsFolder / bank;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error)) files.push_back(it->path());
    }
    std::ranges::sort(files);
    for (const auto& f : files) {
        if (const auto slot = clipSlotOf(f.filename().string())) out.try_emplace(*slot, f);
    }
    return out;
}

std::string clipTitle(const fs::path& file) {
    const std::string stem = file.stem().string();
    auto [digits, end] = leadingDigits(stem);
    while (end < stem.size() && (stem[end] == ' ' || stem[end] == '-' || stem[end] == '_')) ++end;
    return stem.substr(end);
}

std::optional<std::string> saveClip(const fs::path& root, std::string_view bank, int slot,
                                    std::string_view title, std::span<const std::uint8_t> bytes) {
    if (!isClipBankName(bank)) return "a clip bank's name is lowercase letters, digits, and hyphens";
    if (slot < 0 || slot >= primitives::maxClipSlots)
        return "a slot is 1 to " + std::to_string(primitives::maxClipSlots);
    std::string error;
    if (!primitives::readMidiFile(bytes, error)) return "not a MIDI file the server reads: " + error;
    // A title from a file name drops its extension.
    for (const std::string_view ext : {".mid", ".midi", ".MID", ".MIDI"}) {
        if (title.ends_with(ext)) title.remove_suffix(ext.size());
    }
    std::string kept;
    for (const char c : title) {
        if (kept.size() == 64) break;
        if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == ' ' || c == '-' || c == '_') kept += c;
    }
    // Errors name files from the project's root, not the server's paths.
    const fs::path shown = fs::path(clipsFolder) / bank;
    const fs::path folder = root / shown;
    std::error_code fsError;
    fs::create_directories(folder, fsError);
    if (fsError) return "cannot make " + shown.string() + ": " + fsError.message();
    const std::string name =
        std::to_string(slot + 1) + (kept.empty() ? "" : " " + kept) + std::string(extension);
    const fs::path file = folder / name;
    const fs::path part = folder / (name + ".part");
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        for (const auto b : bytes) out.put(static_cast<char>(b));
        if (!out.flush()) {
            fs::remove(part, fsError);
            return "cannot write " + (shown / name).string();
        }
    }
    // In place at once, over a file of the same name; then the slot's other
    // files go, so the slot is never without a file.
    fs::rename(part, file, fsError);
    if (fsError) {
        fs::remove(part, fsError);
        return "cannot write " + (shown / name).string() + ": " + fsError.message();
    }
    std::vector<fs::path> before;
    for (fs::directory_iterator it(folder, fsError), end; !fsError && it != end; it.increment(fsError)) {
        if (it->path() != file && clipSlotOf(it->path().filename().string()) == slot)
            before.push_back(it->path());
    }
    for (const auto& f : before) fs::remove(f, fsError);
    return std::nullopt;
}

std::vector<std::string> clipFiles::sync(primitives::clipBanks& banks) {
    std::vector<std::string> errors;
    pending_ = false;
    for (const auto& name : banks.names()) {
        auto& known = known_[name];
        const auto files = clipFilesOf(root_, name);
        const auto bank = banks.bank(name);
        for (int slot = 0; slot < primitives::maxClipSlots; ++slot) {
            auto& [was, failed] = known[static_cast<std::size_t>(slot)];
            const auto found = files.find(slot);
            std::optional<stamp> now;
            if (found != files.end()) {
                std::error_code error;
                const auto time = fs::last_write_time(found->second, error);
                const auto size = fs::file_size(found->second, error);
                if (!error) now = stamp{.file = found->second, .time = time, .size = size};
            }
            // A file that did not read is read again each time, as a change
            // of its permissions leaves its stamp as it was.
            if (now == was && !failed) continue;
            std::unique_ptr<const primitives::clip> c;
            if (now) {
                std::string error;
                const auto bytes = readFile(now->file, error);
                auto read = bytes ? primitives::readMidiFile(*bytes, error) : std::nullopt;
                if (!read) {
                    // Reported once, until the file changes.
                    if (!failed || now != was)
                        errors.push_back(fs::relative(now->file, root_).string() + ": " + error);
                    was = now;
                    failed = true;
                    continue;
                }
                c = std::make_unique<const primitives::clip>(std::move(*read));
            }
            if (bank->offer(slot, std::move(c))) {
                was = now;
                failed = false;
            } else {
                pending_ = true;
            }
        }
    }
    return errors;
}

}  // namespace server
