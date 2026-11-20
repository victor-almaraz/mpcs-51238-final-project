// Serial lines (io.md §4.3, §4.4): splitting a byte stream into lines,
// reading numbers from a line, and writing a frame as a line. No device
// and no JUCE, so the rules are tested without hardware.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <espresso/primitives/io.h>

namespace server {

/// Longest line read, in bytes, without its line feed (io.md §4.3).
inline constexpr std::size_t maxLineBytes = 256;

/// Splits bytes into lines at line feeds, dropping a carriage return
/// before one. A line longer than maxLineBytes, not counting that carriage
/// return, is dropped up to the next line feed and counted.
class lineSplitter {
public:
    /// Passes each line that @p bytes completes to @p line.
    void feed(std::span<const char> bytes, const std::function<void(std::string_view)>& line);
    /// Lines dropped for their length so far.
    std::uint64_t tooLong() const noexcept { return tooLong_; }

private:
    std::string pending_;
    bool skipping_ = false;  // inside a line already counted as too long
    std::uint64_t tooLong_ = 0;
};

/// What a line holds.
enum class lineKind : std::uint8_t {
    frame,      ///< Numbers: a frame
    empty,      ///< Nothing but separators: ignored
    malformed,  ///< A field that is not a finite number, or more than maxChannels fields
};

/// A line read.
struct parsedLine {
    /// What it holds.
    lineKind kind = lineKind::empty;
    /// Its numbers, when it is a frame; the time is left at 0.
    primitives::dataFrame frame;
};

/// Reads @p line: numbers separated by runs of spaces, tabs, or commas,
/// in decimal, with an optional sign, point, and exponent.
parsedLine parseLine(std::string_view line);

/// The number @p field reads as, in the C locale; nothing when it is not
/// a decimal number or is not finite as a float.
std::optional<float> parseNumber(std::string_view field);

/// @p frame as a line: its values separated by single spaces, each with
/// the fewest significant digits that read back to the same float, ending
/// with a line feed (io.md §4.4).
std::string formatLine(const primitives::dataFrame& frame);

}  // namespace server
