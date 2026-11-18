/// @file
/// Test support: an editor frame rendered to a fixed-size screen, and
/// text found on it.

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <espresso/tui/app.h>

namespace support {

struct frame {
    ftxui::Screen screen;
    explicit frame(tui::app& a, int width = 100, int height = 30)
        : screen(ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height))) {
        ftxui::Render(screen, a.render());
    }

    std::string at(int x, int y) const { return screen.CellAt(x, y).character; }

    bool contains(const std::string& needle) const {
        for (int y = 0; y < screen.dimy(); ++y) {
            std::string row;
            for (int x = 0; x < screen.dimx(); ++x) row += at(x, y).empty() ? " " : at(x, y);
            if (row.find(needle) != std::string::npos) return true;
        }
        return false;
    }

    // Position of the first occurrence of `needle` at or right of column
    // `minX`, on row `minY` or below.
    std::pair<int, int> find(const std::string& needle, int minX = 0, int minY = 0) const {
        for (int y = minY; y < screen.dimy(); ++y) {
            std::string row;
            std::vector<int> xs;
            for (int x = 0; x < screen.dimx(); ++x) {
                xs.push_back(static_cast<int>(row.size()));
                const std::string& c = screen.CellAt(x, y).character;
                row += c.empty() ? " " : c;
            }
            for (std::size_t at = row.find(needle); at != std::string::npos; at = row.find(needle, at + 1)) {
                const auto x = static_cast<int>(std::ranges::find(xs, static_cast<int>(at)) - xs.begin());
                if (x >= minX) return {x, y};
            }
        }
        FAIL("not on screen: " << needle);
        return {-1, -1};
    }
};

}  // namespace support
