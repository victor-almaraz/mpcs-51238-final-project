#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/metadata/pageLayout.h>

namespace metadata {

namespace {

// The number parameter @p name of @p node, or @p otherwise.
double number(const pageNode& node, std::string_view name, double otherwise) {
    const auto it = node.params.find(name);
    if (it == node.params.end() || !std::holds_alternative<double>(it->second)) return otherwise;
    return std::get<double>(it->second);
}

std::string text(const pageNode& node, std::string_view name) {
    const auto it = node.params.find(name);
    if (it == node.params.end() || !std::holds_alternative<std::string>(it->second)) return {};
    return std::get<std::string>(it->second);
}

// A pair parameter (`span`, `at`, `size`) of whole numbers, when given.
std::optional<pageUnits> pair(const pageNode& node, std::string_view name) {
    const auto it = node.params.find(name);
    if (it == node.params.end() || !std::holds_alternative<std::vector<double>>(it->second))
        return std::nullopt;
    const auto& l = std::get<std::vector<double>>(it->second);
    if (l.size() != 2) return std::nullopt;
    return pageUnits{.w = static_cast<int>(std::lround(l[0])), .h = static_cast<int>(std::lround(l[1]))};
}

void setPair(pageNode& node, const std::string& name, pageUnits u) {
    node.params[name] = std::vector<double>{static_cast<double>(u.w), static_cast<double>(u.h)};
}

int whiteKeys(int low, int high) {
    int count = 0;
    for (int n = low; n <= high; ++n) {
        const int pc = ((n % 12) + 12) % 12;
        if (pc == 0 || pc == 2 || pc == 4 || pc == 5 || pc == 7 || pc == 9 || pc == 11) ++count;
    }
    return count;
}

std::string describe(const pageNode& node) {
    std::string out = node.kind;
    if (!node.label.empty()) out += " \"" + node.label + "\"";
    return out;
}

std::string units(pageUnits u) { return std::to_string(u.w) + " × " + std::to_string(u.h) + " units"; }

// Which cells of a section are taken, as it grows.
class occupancy {
public:
    bool free(int x, int y, pageUnits s) const {
        for (int cy = y; cy < y + s.h; ++cy) {
            for (int cx = x; cx < x + s.w; ++cx) {
                if (taken(cx, cy)) return false;
            }
        }
        return true;
    }
    void take(int x, int y, pageUnits s) {
        for (int cy = y; cy < y + s.h; ++cy) {
            for (int cx = x; cx < x + s.w; ++cx) cells_.emplace_back(cx, cy);
        }
    }

private:
    bool taken(int x, int y) const { return std::ranges::find(cells_, std::pair{x, y}) != cells_.end(); }
    std::vector<std::pair<int, int>> cells_;
};

class layout {
public:
    std::vector<layoutProblem> problems;

    // Lays out @p node and returns the size it needs, in units: what it
    // takes on its parent's grid.
    pageUnits run(pageNode& node, const std::string& where) {
        const std::string here = where.empty() ? describe(node) : where + " > " + describe(node);
        if (node.kind == "section") return section(node, here);
        if (node.kind == "pedal") return pedal(node, here);
        std::vector<pageUnits> sizes;
        sizes.reserve(node.children.size());
        for (auto& child : node.children) sizes.push_back(run(child, here));
        pageUnits need{};
        if (node.kind == "panel") need = panel(node, sizes);
        if (node.kind == "synth") need = synth(node, sizes);
        if (node.kind == "pedalboard") need = pedalboard(sizes);
        check(node, here, need);
        return pair(node, "size").value_or(need);
    }

private:
    void problem(const pageNode& node, const std::string& where, std::string message) {
        problems.push_back({.where = where, .message = std::move(message), .src = node.src});
    }

    // A size given smaller than the one needed.
    void check(const pageNode& node, const std::string& where, pageUnits need) {
        const auto given = pair(node, "size");
        if (given && (given->w < need.w || given->h < need.h))
            problem(node, where, "size is " + units(*given) + "; what it holds needs " + units(need));
    }

    // A section: its children placed in the order written, each in its
    // footprint (or span), around those placed by hand.
    pageUnits section(pageNode& node, const std::string& where) {
        int columns = static_cast<int>(number(node, "columns", 0));
        // Rows of clips stack, one a line: a loop matrix (canvas.md §6).
        if (columns == 0 && !node.children.empty() &&
            std::ranges::all_of(node.children, [](const pageNode& c) { return c.kind == "clips"; })) {
            for (const auto& c : node.children)
                columns = std::max(columns, pair(c, "span").value_or(footprintOf(c)).w);
        }
        occupancy grid;
        pageUnits extent{.w = columns, .h = 0};
        // Those placed by hand first; the rest flow around them.
        for (auto& child : node.children) {
            const auto at = pair(child, "at");
            if (!at) continue;
            const pageUnits span = pair(child, "span").value_or(footprintOf(child));
            const int x = at->w - 1;
            const int y = at->h - 1;
            if (!grid.free(x, y, span))
                problem(child, where + " > " + describe(child),
                        "overlaps another element at " + std::to_string(at->w) + " " + std::to_string(at->h));
            grid.take(x, y, span);
            setPair(child, "span", span);
            extent.w = std::max(extent.w, x + span.w);
            extent.h = std::max(extent.h, y + span.h);
        }
        // Without columns, one row as tall as the tallest child: each child
        // stacks under the one before when it fits, else starts a column.
        int height = extent.h;
        for (const auto& child : node.children) {
            if (!pair(child, "at"))
                height = std::max(height, pair(child, "span").value_or(footprintOf(child)).h);
        }
        int cx = 0;
        int cy = 0;
        for (auto& child : node.children) {
            if (pair(child, "at")) continue;
            const pageUnits span = pair(child, "span").value_or(footprintOf(child));
            if (columns > 0) {
                // Rows of columns units, filled in turn.
                while (cx + span.w > std::max(columns, span.w) || !grid.free(cx, cy, span)) {
                    if (++cx + span.w > std::max(columns, span.w)) {
                        cx = 0;
                        ++cy;
                    }
                }
            } else {
                // Down the current column, then on to the next.
                while (cy + span.h > height || !grid.free(cx, cy, span)) {
                    if (++cy + span.h > height) {
                        cy = 0;
                        ++cx;
                    }
                }
            }
            grid.take(cx, cy, span);
            setPair(child, "at", {.w = cx + 1, .h = cy + 1});
            setPair(child, "span", span);
            extent.w = std::max(extent.w, cx + span.w);
            extent.h = std::max(extent.h, cy + span.h);
            if (columns > 0) {
                cx += span.w;
            } else {
                cy += span.h;
            }
        }
        check(node, where, extent);
        const auto given = pair(node, "size");
        if (!given) setPair(node, "size", extent);
        const pageUnits size = given.value_or(extent);
        return {.w = size.w, .h = size.h + (node.label.empty() ? 0 : 1)};
    }

    // A pedal: its controls in rows a unit apart, each row centred, a
    // control of another height starting a new row, at the width whose
    // pedal is nearest a portrait enclosure's shape (a 125B is about 1.8
    // times as high as it is wide); then its name, LED, and footswitch.
    // Each control takes an `at` in the pedal's control area: its row's top
    // and its column, from 1.
    pageUnits pedal(pageNode& node, const std::string& where) {
        std::vector<std::reference_wrapper<pageNode>> items;
        bool footswitch = false;
        for (auto& child : node.children) {
            if (child.kind == "footswitch") {
                footswitch = true;
            } else if (child.kind != "lamp") {
                items.emplace_back(child);
            }
        }
        std::vector<pageUnits> spans;
        spans.reserve(items.size());
        int widest = 0;
        int total = 0;
        for (const pageNode& child : items) {
            spans.push_back(pair(child, "span").value_or(footprintOf(child)));
            widest = std::max(widest, spans.back().w);
            total += spans.back().w + 1;
        }
        const int name = static_cast<int>(std::ceil(static_cast<double>(node.label.size()) * 0.9));
        // At least a first row of two controls side by side (three when it
        // holds more than four), as pedals hold their knobs.
        const std::size_t perRow = spans.size() > 4 ? 3 : 2;
        int firstRow = -1;
        for (std::size_t i = 0; i < std::min(perRow, spans.size()); ++i) firstRow += spans[i].w + 1;
        const int least = std::max({widest, name, 6, firstRow});
        const auto height = [&](int rows) {
            return 1 + rows + (rows > 0 ? 1 : 0) + 2 + 1 + (footswitch ? 4 : 0) + 1;
        };
        const auto given = pair(node, "size");
        int inner = given ? given->w - 2 : least;
        if (!given) {
            double best = 1e9;
            for (int w = least; w <= std::max(least, total); ++w) {
                const packing rows = rowsAt(spans, w);
                const double shape = static_cast<double>(height(rows.height)) / (w + 2);
                // A row that cannot sit centred on the grid counts against a width.
                const double off = std::abs(shape - 1.8) + 0.15 * rows.uneven;
                if (off < best - 1e-9) {
                    best = off;
                    inner = w;
                }
            }
        }
        const packing rows = rowsAt(spans, std::max(inner, widest));
        for (std::size_t i = 0; i < items.size(); ++i) {
            setPair(items[i], "span", spans[i]);
            setPair(items[i], "at", rows.at[i]);
        }
        const pageUnits need{.w = std::max(inner, least) + 2, .h = height(rows.height)};
        check(node, where, need);
        if (!given) setPair(node, "size", need);
        return given.value_or(need);
    }

    // Controls in rows at @p width: each row's controls a unit apart and
    // centred, a new row when the next would pass the width or is of
    // another height; each control's place, and the rows' height.
    struct packing {
        std::vector<pageUnits> at;
        int height = 0;
        int uneven = 0;  // rows whose spare width is odd
    };
    static packing rowsAt(const std::vector<pageUnits>& spans, int width) {
        packing out;
        out.at.resize(spans.size());
        std::size_t first = 0;
        int top = 0;
        while (first < spans.size()) {
            std::size_t last = first;
            int used = spans[first].w;
            int tall = spans[first].h;
            while (last + 1 < spans.size() && spans[last + 1].h == spans[first].h &&
                   used + 1 + spans[last + 1].w <= width) {
                ++last;
                used += 1 + spans[last].w;
                tall = std::max(tall, spans[last].h);
            }
            if ((width - used) % 2 != 0) ++out.uneven;
            int x = (width - used) / 2;
            for (std::size_t i = first; i <= last; ++i) {
                out.at[i] = {.w = x + 1, .h = top + 1};
                x += spans[i].w + 1;
            }
            top += tall;
            first = last + 1;
        }
        out.height = top;
        return out;
    }

    // A panel: padded a unit; a title two units high and a unit apart when
    // labelled; sections side by side two units apart (a rule between), or
    // stacked.
    static pageUnits panel(const pageNode& node, const std::vector<pageUnits>& sizes) {
        const bool column = text(node, "flow") == "column";
        pageUnits inner{};
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            const int gap = i == 0 ? 0 : 2;
            if (column) {
                inner.w = std::max(inner.w, sizes[i].w);
                inner.h += sizes[i].h + gap;
            } else {
                inner.w += sizes[i].w + gap;
                inner.h = std::max(inner.h, sizes[i].h);
            }
        }
        const int title = node.label.empty() ? 0 : 3;
        return {.w = inner.w + 2, .h = inner.h + title + 2};
    }

    // A synth panel: its cheeks, a unit each; its face padded a unit, its
    // model's name two units high and a unit apart; sections side by side
    // two units apart; a keybed under them a unit apart.
    static pageUnits synth(const pageNode& node, const std::vector<pageUnits>& sizes) {
        pageUnits row{};
        int keybed = 0;
        bool first = true;
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            const auto& child = node.children[i];
            const bool keys =
                std::ranges::any_of(child.children, [](const pageNode& c) { return c.kind == "keys"; });
            if (keys) {
                row.w = std::max(row.w, sizes[i].w);
                keybed = std::max(keybed, sizes[i].h + 1);
                continue;
            }
            row.w += sizes[i].w + (first ? 0 : 2);
            row.h = std::max(row.h, sizes[i].h);
            first = false;
        }
        return {.w = row.w + 4, .h = row.h + keybed + 3 + 2};
    }

    // A pedalboard: pedals in a row a unit apart, padded two units at the
    // sides and one above and below.
    static pageUnits pedalboard(const std::vector<pageUnits>& sizes) {
        pageUnits need{};
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            need.w += sizes[i].w + (i == 0 ? 0 : 1);
            need.h = std::max(need.h, sizes[i].h);
        }
        return {.w = need.w + 4, .h = need.h + 2};
    }
};

}  // namespace

pageUnits footprintOf(const pageNode& element) {
    const std::string& kind = element.kind;
    const bool vertical = text(element, "orientation") == "vertical";
    if (kind == "knob")
        return text(element, "size") == "big" ? pageUnits{.w = 4, .h = 5} : pageUnits{.w = 3, .h = 4};
    if (kind == "toggle" || kind == "button" || kind == "footswitch") return {.w = 3, .h = 3};
    if (kind == "lamp" || kind == "value") return {.w = 3, .h = 2};
    if (kind == "select") return {.w = 4, .h = 2};
    if (kind == "slider" || kind == "meter")
        return vertical ? pageUnits{.w = 2, .h = 6} : pageUnits{.w = 6, .h = 2};
    if (kind == "drawbar") return {.w = 2, .h = 6};
    if (kind == "xy") return {.w = 8, .h = 8};
    if (kind == "screen") return {.w = 3, .h = 2};
    // Its label, a cell four units wide for each slot, and its stop button.
    if (kind == "clips") return {.w = 6 + (4 * static_cast<int>(number(element, "slots", 8))), .h = 2};
    if (kind == "keys") {
        const int low = static_cast<int>(number(element, "low", 48));
        const int high = static_cast<int>(number(element, "high", 71));
        return {.w = std::max(1, whiteKeys(low, std::max(low, high))), .h = 5};
    }
    return {.w = 1, .h = 1};
}

std::vector<layoutProblem> layOut(pageNode& page) {
    layout l;
    l.run(page, "");
    return std::move(l.problems);
}

}  // namespace metadata
