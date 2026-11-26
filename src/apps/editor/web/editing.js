// The editing commands of the command table (patterns.md §22, §25): those
// CodeMirror provides, and those the editor adds — deselect, join and sort
// lines, case changes, and selection by form, which walks the tokenizer's
// tree of lists (lisp.js, listTree). Each is a CodeMirror command: a
// function of the view that answers whether it applied.

import { EditorSelection, StateEffect, StateField } from "@codemirror/state";
import { listTree } from "./lisp.js";

export {
  addCursorAbove, addCursorBelow, copyLineDown as duplicate, cursorMatchingBracket, deleteLine, indentLess,
  indentMore, moveLineDown, moveLineUp, redo, selectAll, toggleBlockComment, toggleComment, undo,
} from "@codemirror/commands";
export { selectNextOccurrence, selectSelectionMatches } from "@codemirror/search";

/** Collapses the selection to its main cursor (the HIG's Deselect All). */
export function selectNone(view) {
  const { main, ranges } = view.state.selection;
  if (ranges.length === 1 && main.empty) return false;
  view.dispatch({ selection: EditorSelection.cursor(main.head) });
  return true;
}

// The lines a range covers, whole.
function linesOf(state, range) {
  const first = state.doc.lineAt(range.from);
  let last = state.doc.lineAt(range.to);
  // A selection that ends at a line's start leaves that line out.
  if (!range.empty && range.to === last.from && last.number > first.number) last = state.doc.line(last.number - 1);
  return { first, last };
}

/**
 * Joins each selection's lines, or the cursor's line and the next: the
 * break and the next line's indentation become one space, or nothing
 * after an opening or before a closing bracket.
 */
export function joinLines(view) {
  const { state } = view;
  const changes = [];
  for (const range of state.selection.ranges) {
    const { first, last } = linesOf(state, range);
    const end = last.number > first.number ? last.number : Math.min(first.number + 1, state.doc.lines);
    for (let n = first.number; n < end; ++n) {
      const line = state.doc.line(n);
      const next = state.doc.line(n + 1);
      const indent = /^\s*/.exec(next.text)[0].length;
      const before = line.text.trimEnd();
      const after = next.text.slice(indent);
      const tight = before === "" || /[([{]$/.test(before) || /^[)\]}]/.test(after) || after === "";
      changes.push({ from: line.from + before.length, to: next.from + indent, insert: tight ? "" : " " });
    }
  }
  if (changes.length === 0) return false;
  view.dispatch({ changes, userEvent: "input.join" });
  return true;
}

/** Sorts the lines each selection covers, in the locale's order. */
export function sortLines(view) {
  const { state } = view;
  const changes = [];
  for (const range of state.selection.ranges) {
    if (range.empty) continue;
    const { first, last } = linesOf(state, range);
    if (last.number === first.number) continue;
    const lines = state.sliceDoc(first.from, last.to).split("\n");
    const sorted = [...lines].sort((a, b) => a.localeCompare(b));
    if (sorted.join("\n") !== lines.join("\n")) changes.push({ from: first.from, to: last.to, insert: sorted.join("\n") });
  }
  if (changes.length === 0) return false;
  view.dispatch({ changes, userEvent: "input.sort" });
  return true;
}

// A command that rewrites each selection's text, or the word at a cursor.
function transform(rewrite) {
  return (view) => {
    const { state } = view;
    const changes = [];
    const ranges = state.selection.ranges.map((range) => {
      const target = range.empty ? state.wordAt(range.head) : range;
      if (!target) return range;
      const text = state.sliceDoc(target.from, target.to);
      const insert = rewrite(text);
      if (insert !== text) changes.push({ from: target.from, to: target.to, insert });
      return range;
    });
    if (changes.length === 0) return false;
    view.dispatch({ changes, selection: EditorSelection.create(ranges, state.selection.mainIndex), userEvent: "input.case" });
    return true;
  };
}

export const upperCase = transform((text) => text.toLocaleUpperCase());
export const lowerCase = transform((text) => text.toLocaleLowerCase());
export const titleCase = transform((text) =>
  text.toLocaleLowerCase().replace(/(^|[^\p{L}\p{N}'])(\p{L})/gu, (_, before, letter) => before + letter.toLocaleUpperCase()));

// ---- Selection by form: a token, a list's contents, the list, its parent's contents, and so on to the whole text.

// Every range that selection by form can stop at in `text`, smallest first for a given start.
function formRanges(text) {
  const ranges = [{ from: 0, to: text.length }];
  const visit = (node) => {
    for (const child of node.children ?? []) {
      if (child.children) {
        if (child.closed && child.to - child.from > 2) ranges.push({ from: child.from + 1, to: child.to - 1 });
        ranges.push({ from: child.from, to: child.to });
        visit(child);
      } else {
        ranges.push({ from: child.from, to: child.to });
      }
    }
  };
  visit(listTree(text));
  return ranges;
}

// The smallest form range that holds `range` and is larger than it.
function enclosing(ranges, range) {
  let best = null;
  for (const r of ranges) {
    if (r.from > range.from || r.to < range.to || (r.from === range.from && r.to === range.to)) continue;
    if (!best || r.to - r.from < best.to - best.from) best = r;
  }
  return best;
}

const pushExpansion = StateEffect.define();
const popExpansion = StateEffect.define();

// The selections that Expand Selection grew from, so that Shrink Selection returns through them.
const expansions = StateField.define({
  create: () => [],
  update(stack, tr) {
    for (const e of tr.effects) {
      if (e.is(pushExpansion)) return [...stack, e.value];
      if (e.is(popExpansion)) return stack.slice(0, -1);
    }
    return tr.selection || tr.docChanged ? [] : stack;
  },
});

/** Grows each selection to the smallest form that holds it (VSC's Expand Selection, by the dialect's lists). */
export function expandSelection(view) {
  const { state } = view;
  const ranges = formRanges(state.doc.toString());
  let grown = false;
  const next = state.selection.ranges.map((range) => {
    const r = enclosing(ranges, range);
    if (!r) return range;
    grown = true;
    return EditorSelection.range(r.from, r.to);
  });
  if (!grown) return false;
  view.dispatch({
    selection: EditorSelection.create(next, state.selection.mainIndex),
    effects: pushExpansion.of(state.selection),
  });
  return true;
}

/** Returns each selection to what it was before the last Expand Selection. */
export function shrinkSelection(view) {
  const stack = view.state.field(expansions, false) ?? [];
  if (stack.length === 0) return false;
  view.dispatch({ selection: stack.at(-1), effects: popExpansion.of(null) });
  return true;
}

/** The state the commands above keep. */
export const editingState = [expansions];
