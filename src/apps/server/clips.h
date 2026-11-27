// The clips of a project (canvas.md §5.3): the MIDI files of each bank's
// folder, clips/<bank>/, read into the engine's clip banks, listed for
// pages, and saved from pages' uploads. Has no JUCE dependency, so
// server_tests and offline renders use the same code as a live server.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/primitives/clip.h>

namespace server {

/// The slot, from 0, of a file named @p name in a bank's folder: `3.mid`
/// or `3 name.mid` is slot 2, slots counting from 1 in file names (as on
/// the page); nothing for other names or slots past maxClipSlots.
std::optional<int> clipSlotOf(std::string_view name);

/// The files of bank @p bank's folder under @p root, by slot from 0: the
/// first in name order of a slot's names.
std::map<int, std::filesystem::path> clipFilesOf(const std::filesystem::path& root, std::string_view bank);

/// The name a page shows for the clip file @p file: its name without the
/// slot and `.mid`.
std::string clipTitle(const std::filesystem::path& file);

/// Saves the MIDI file @p bytes as slot @p slot, from 0, of bank @p bank
/// under @p root, as `clips/<bank>/<slot + 1> <title>.mid`, in place of the
/// slot's files before, written whole or not at all. @p title is kept to
/// letters, digits, spaces, hyphens, and underscores, at most 64 of them.
/// The error, when the bank's name or the slot is not one, the bytes are
/// not a MIDI file readMidiFile reads, or the file cannot be written.
std::optional<std::string> saveClip(const std::filesystem::path& root, std::string_view bank, int slot,
                                    std::string_view title, std::span<const std::uint8_t> bytes);

/// True for a bank name: lowercase letters, digits, and hyphens.
bool isClipBankName(std::string_view bank);

/// Gives the engine's clip banks the files of their folders under a
/// project root, reading only those that changed. Control thread.
class clipFiles {
public:
    /// Reads banks' folders under @p root.
    explicit clipFiles(std::filesystem::path root) : root_(std::move(root)) {}

    /// Gives each bank of @p banks the clips of its folder's files that
    /// changed since the last call, and empties the slots whose file is
    /// gone. Returns an error for each file that does not read; a file a
    /// bank cannot take yet (clipBank::offer) is read again next time.
    std::vector<std::string> sync(primitives::clipBanks& banks);

private:
    struct stamp {
        std::filesystem::path file;
        std::filesystem::file_time_type time;
        std::uintmax_t size = 0;
        friend bool operator==(const stamp&, const stamp&) = default;
    };
    std::filesystem::path root_;
    std::map<std::string, std::array<std::optional<stamp>, primitives::maxClipSlots>> known_;
};

}  // namespace server
