/// @file
/// A page's layout computed in units (`canvas.md` §2): each control's
/// footprint, each section's and pedal's placement of what it holds, and
/// the size each group needs. The interpreter lays a piece's page out with
/// it, so the IR carries every size and place; the validator checks a
/// layout with it; the renderer draws what it computed.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <espresso/metadata/graph.h>

namespace metadata {

/// A size in units of the page grid (`--es-u`, 24 px).
struct pageUnits {
    /// Columns.
    int w = 0;
    /// Rows.
    int h = 0;
    friend bool operator==(const pageUnits&, const pageUnits&) = default;
};

/// The footprint of the control or display @p element, in units, before
/// any `span` it gives: a knob 3 × 4 (big, 4 × 5), a toggle, button, or
/// footswitch 3 × 3, a lamp or value 3 × 2, a select 4 × 2, a slider or
/// meter 6 × 2 (2 × 6 vertical), a drawbar 2 × 6, a pad 8 × 8, a keyboard
/// one unit per white key by 5.
pageUnits footprintOf(const pageNode& element);

/// A layout that cannot be drawn: a group given a `size` smaller than what
/// it holds needs, or a child placed outside its group or over another.
struct layoutProblem {
    /// The element's path: `page > synth "Pad" > section "Filter"`.
    std::string where;
    /// What is wrong.
    std::string message;
    /// The element's definition, when known.
    std::optional<sourceSpan> src;
};

/// Lays out @p page, a tree whose root is a `page`, in place: each
/// section's children take an `at` and a `span` (those given are kept),
/// packed in the order written, in one row where a child that fits under
/// the one before stacks under it, or in rows of the section's `columns`;
/// each section and pedal takes the `size` it needs, unless it gives one;
/// panels, synth panels, and pedalboards are checked against a `size` they
/// give. The same tree lays out the same way every time.
std::vector<layoutProblem> layOut(pageNode& page);

}  // namespace metadata
