// Line marks (patterns.md §21.7): a mark on a line to come back to, set by
// Toggle Mark (Ctrl+D, the HIG's Add Bookmark) or a click in the marks
// margin, followed by Next and Previous Mark (Ctrl+B, Shift+Ctrl+B, gedit's),
// and kept per project as a line and its text.
//
// The marks margin stands right of the line numbers, as GtkSourceView's
// does, and holds the run's problems too: a line's error or warning icon
// wins over its mark, which its tooltip then names.

import { MapMode, RangeSet, RangeSetBuilder, StateEffect, StateField } from "@codemirror/state";
import { GutterMarker, gutter } from "@codemirror/view";
import { forEachDiagnostic } from "@codemirror/lint";
import { h } from "./dom.js";
import { icon } from "./icons.js";

class MarkMarker extends GutterMarker {
  eq(other) {
    return other instanceof MarkMarker;
  }
  toDOM() {
    return h("span", { class: "cm-markMarker", title: "Marked Line" }, icon("mark"));
  }
}
// A mark at a line's start that the line's deletion leaves at the start of the line in its place
// (GtkSourceView: "moved to the beginning of the line when the user deletes the line they were in").
MarkMarker.prototype.mapMode = MapMode.Simple;
const markMarker = new MarkMarker();

class ProblemMarker extends GutterMarker {
  constructor(severity, message, marked) {
    super();
    this.severity = severity;
    this.message = message;
    this.marked = marked;
  }
  eq(other) {
    return other instanceof ProblemMarker && other.severity === this.severity && other.message === this.message
      && other.marked === this.marked;
  }
  toDOM() {
    const title = this.marked ? `${this.message}\nMarked Line` : this.message;
    return h("span", { class: `cm-lint-marker cm-lint-marker-${this.severity}`, title });
  }
}

const toggleAt = StateEffect.define();  // a position: the mark of its line, set or cleared
/** Replaces the document's marks with marks at these positions' lines. */
export const setMarks = StateEffect.define();

// Marks at the starts of these positions' lines, one a line.
function marksAt(doc, positions) {
  const starts = [...new Set(positions.map((p) => doc.lineAt(Math.min(Math.max(p, 0), doc.length)).from))];
  return RangeSet.of(starts.sort((a, b) => a - b).map((p) => markMarker.range(p)));
}

/** The document's marks, at the starts of their lines. */
export const marksField = StateField.define({
  create: () => RangeSet.empty,
  update(marks, tr) {
    let next = marks.map(tr.changes);
    for (const e of tr.effects) {
      if (e.is(setMarks)) next = marksAt(tr.state.doc, e.value);
      if (e.is(toggleAt)) {
        const line = tr.state.doc.lineAt(e.value);
        const had = positions(next).some((p) => p >= line.from && p <= line.to);
        next = had ? next.update({ filter: (from) => from < line.from || from > line.to })
          : next.update({ add: [markMarker.range(line.from)] });
      }
    }
    // A change can bring two marks onto one line, or one off its line's start.
    if (tr.docChanged) next = marksAt(tr.state.doc, positions(next));
    return next;
  },
});

function positions(marks) {
  const found = [];
  for (let iter = marks.iter(); iter.value; iter.next()) found.push(iter.from);
  return found;
}

/** The marked lines of `state`, by number, in order. */
export function markedLines(state) {
  const marks = state.field(marksField, false);
  return marks ? positions(marks).map((p) => state.doc.lineAt(p).number) : [];
}

/** The marks to keep: each marked line's number and text. */
export function savedMarks(state) {
  return markedLines(state).map((n) => ({ line: n, text: state.doc.line(n).text }));
}

/**
 * The positions of `saved` marks in `doc`: each at its line when that line
 * has its text still, else at the nearest line that has it; a mark whose
 * text is on no line is dropped.
 */
export function restoredMarks(doc, saved) {
  const found = [];
  for (const { line, text } of saved ?? []) {
    for (let d = 0; d < doc.lines; ++d) {
      const at = [line - d, line + d].find((n) => n >= 1 && n <= doc.lines && doc.line(n).text === text);
      if (at !== undefined) {
        found.push(doc.line(at).from);
        break;
      }
    }
  }
  return found;
}

/** Sets or clears the mark of the cursor's line. */
export function toggleMark(view) {
  view.dispatch({ effects: toggleAt.of(view.state.selection.main.head) });
  return true;
}

// The cursor to the next marked line after the cursor's (or before it), wrapping round the document.
function travel(direction) {
  return (view) => {
    const lines = markedLines(view.state);
    if (lines.length === 0) return false;
    const here = view.state.doc.lineAt(view.state.selection.main.head).number;
    const next = direction > 0 ? lines.find((n) => n > here) ?? lines[0] : lines.findLast((n) => n < here) ?? lines.at(-1);
    view.dispatch({ selection: { anchor: view.state.doc.line(next).from }, scrollIntoView: true });
    return true;
  };
}

export const nextMark = travel(1);
export const previousMark = travel(-1);

// The margin's markers: a line's worst problem, else its mark.
function marginMarkers(view) {
  const { state } = view;
  const problems = new Map();
  const rank = { error: 3, warning: 2, info: 1, hint: 0 };
  forEachDiagnostic(state, (d, from) => {
    const start = state.doc.lineAt(from).from;
    const known = problems.get(start);
    if (!known || rank[d.severity] > rank[known.severity]) problems.set(start, { severity: d.severity, message: d.message });
  });
  const marked = new Set(positions(state.field(marksField, false) ?? RangeSet.empty));
  const builder = new RangeSetBuilder();
  for (const start of [...new Set([...problems.keys(), ...marked])].sort((a, b) => a - b)) {
    const problem = problems.get(start);
    builder.add(start, start, problem ? new ProblemMarker(problem.severity, problem.message, marked.has(start)) : markMarker);
  }
  return builder.finish();
}

/** The marks: their field and the margin, whose click sets or clears its line's mark (Kate's icon border). */
export function marks(saved = []) {
  return [
    marksField.init((state) => marksAt(state.doc, restoredMarks(state.doc, saved))),
    gutter({
      class: "cm-marksMargin",
      markers: marginMarkers,
      initialSpacer: () => markMarker,
      domEventHandlers: {
        mousedown(view, line, event) {
          if (event.button !== 0) return false;
          view.dispatch({ effects: toggleAt.of(line.from) });
          return true;
        },
      },
    }),
  ];
}
