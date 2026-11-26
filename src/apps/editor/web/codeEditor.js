// The text editor: CodeMirror 6 with the editor surface of the design
// guide (brand/gui/design-guide.md §4, §5.5) in Carbon, and the CUA keys
// (patterns.md §14.3), the dialect's highlighting (tauri-editor.md §5.4),
// and the run's problems as diagnostics: a wavy underline, an icon in the
// marks margin, and a hover (design-guide.md §5.5); line marks (marks.js).

import { Compartment, EditorState, RangeSet, StateEffect, StateField } from "@codemirror/state";
import {
  Decoration, EditorView, GutterMarker, crosshairCursor, gutter, gutterLineClass, keymap, lineNumbers,
  highlightActiveLine, highlightActiveLineGutter, drawSelection, rectangularSelection,
} from "@codemirror/view";
import { nextDiagnostic, previousDiagnostic, setDiagnostics } from "@codemirror/lint";
import { history, indentWithTab, standardKeymap } from "@codemirror/commands";
import { highlightSelectionMatches } from "@codemirror/search";
import { closeBrackets, closeBracketsKeymap } from "@codemirror/autocomplete";
import { editingState } from "./editing.js";
import { marks } from "./marks.js";
import {
  bracketMatching, codeFolding, foldEffect, foldService, foldable, foldedRanges, indentUnit, unfoldEffect, HighlightStyle,
  syntaxHighlighting,
} from "@codemirror/language";
import { tags } from "@lezer/highlight";
import { foldRanges, lisp, tokenize } from "./lisp.js";
import { schemeKeys } from "./schemes.js";
import { find } from "./find.js";
import { h } from "./dom.js";
import { icon } from "./icons.js";

// Carbon's error icon (icons.js) for the marks margin, as an image.
const errorMark = "url(\"data:image/svg+xml," + encodeURIComponent(
  '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" width="16" height="16">' +
  '<circle cx="8" cy="8" r="6.5" fill="#ff6b6b"/>' +
  '<path d="M5.8 5.8l4.4 4.4M10.2 5.8l-4.4 4.4" stroke="#000" stroke-width="1.5"/></svg>') + "\")";

// Carbon's editor surface, from the stylesheet's custom properties.
const carbon = EditorView.theme({
  "&": { height: "100%", backgroundColor: "var(--window)", color: "var(--text)" },
  ".cm-scroller": { font: "14px/var(--line) var(--mono)", paddingTop: "var(--gap)" },
  ".cm-content": { padding: "0", caretColor: "var(--bright)" },
  ".cm-line": { padding: "0 0 0 1px" },
  ".cm-cursor, .cm-dropCursor": { borderLeft: "2px solid var(--bright)", marginLeft: "-1px" },
  ".cm-activeLine": { backgroundColor: "var(--lineCurrent)" },
  // Line numbers in 32 px, then the marks margin of 24 (marks and problems, right of the numbers as
  // GtkSourceView's marks are, patterns.md §21.7), then the fold margin of 24 (design-guide.md §4):
  // the layout's units, each icon centered in its own.
  ".cm-gutters": { backgroundColor: "var(--window)", color: "var(--lineNumberText)", border: "none" },
  // The fold margin: a chevron on each line that folds, seen while the pointer is over the gutter or the line is folded.
  ".cm-foldMargin": { width: "var(--control)" },
  ".cm-foldMargin .cm-gutterElement": { display: "grid", placeItems: "center", cursor: "default" },
  ".cm-foldMarker": { display: "grid", placeItems: "center", width: "var(--icon)", height: "var(--icon)", color: "var(--secondary)",
                      opacity: "0", transition: "opacity 0.1s" },
  ".cm-foldMarker:hover": { color: "var(--bright)" },
  ".cm-gutters:hover .cm-foldMarker, .cm-foldMarker.folded": { opacity: "1" },
  ".cm-foldPlaceholder": { background: "var(--tabActive)", color: "var(--bright)", border: "none", borderRadius: "0",
                           padding: "0 var(--gapTight)", margin: "0 var(--gapTight)", font: "12px var(--ui)" },
  ".cm-marksMargin": { width: "var(--control)" },
  ".cm-marksMargin .cm-gutterElement": { padding: "0", display: "grid", placeItems: "center", cursor: "default" },
  ".cm-markMarker": { display: "grid", placeItems: "center", color: "var(--accentText)" },
  ".cm-lint-marker": { width: "var(--icon)", height: "var(--icon)" },
  ".cm-lint-marker-error": { content: errorMark },
  // An error's range, and its hover (§5.5): the message, where it is, and View Problem.
  ".cm-lintRange-error": { backgroundImage: "none", textDecoration: "underline wavy var(--error)",
                           textDecorationSkipInk: "none", textUnderlineOffset: "3px" },
  ".cm-lintRange-active": { backgroundColor: "transparent" },
  ".cm-tooltip": { backgroundColor: "var(--box)", color: "var(--bright)", border: "none",
                   outline: "1px solid var(--boxRule)" },
  ".cm-tooltip.cm-tooltip-lint": { width: "420px", padding: "0", font: "13px/20px var(--ui)" },
  ".cm-diagnostic, .cm-diagnostic-error": { padding: "0", margin: "0", border: "none" },
  ".cm-diagnosticText": { display: "block", padding: "var(--gap) var(--inset)" },
  ".cm-problem": { display: "flex", gap: "var(--gap)" },
  ".cm-problem .icon": { color: "var(--error)", marginTop: "2px" },  // centered on the first 20 px line,
  ".cm-problem .src": { color: "var(--secondary)", font: "12px var(--mono)" },
  ".cm-diagnosticAction": { display: "flex", alignItems: "center", width: "100%", height: "var(--row)", margin: "0", padding: "0 var(--inset)", border: "none",
                            borderRadius: "0", background: "var(--side)", color: "var(--accentText)",
                            font: "13px var(--ui)", textAlign: "left", cursor: "pointer" },
  ".cm-diagnosticAction:hover": { background: "var(--tabHover)" },
  // The Vim and Emacs schemes' command lines; Vim's block cursor is editor.css's.
  ".cm-panels": { backgroundColor: "var(--side)", color: "var(--bright)" },
  ".cm-panels.cm-panels-bottom": { borderTop: "none" },
  ".cm-vim-panel": { height: "var(--row)", padding: "0 var(--inset)", font: "13px/var(--row) var(--mono)" },
  ".cm-vim-panel input": { color: "var(--bright)", font: "inherit" },
  // Completion (complete.js), on the box color with a hairline, in the code's face.
  ".cm-tooltip.cm-tooltip-autocomplete > ul": { font: "13px/var(--row) var(--mono)", maxHeight: "calc(10 * var(--row))", minWidth: "260px" },
  ".cm-tooltip.cm-tooltip-autocomplete > ul > li": { height: "var(--row)", lineHeight: "var(--row)", padding: "0 var(--inset)" },
  ".cm-tooltip.cm-tooltip-autocomplete > ul > li[aria-selected]": { background: "var(--tabActive)", color: "var(--bright)" },
  ".cm-completionMatchedText": { textDecoration: "none", color: "var(--accentText)", fontWeight: "700" },
  ".cm-completionDetail": { color: "var(--secondary)", fontStyle: "normal", font: "12px var(--ui)", marginLeft: "var(--insetLarge)",
                            float: "right" },
  // An evaluated form's mark is editor.css's .cm-evaluated, which reduced motion changes.
  ".cm-lineNumbers .cm-gutterElement": { minWidth: "32px", padding: "0", textAlign: "right" },
  ".cm-activeLineGutter": { backgroundColor: "var(--lineCurrent)", color: "var(--bright)" },
  "&.cm-focused": { outline: "none" },
  ".cm-selectionBackground, &.cm-focused .cm-selectionBackground, ::selection": {
    backgroundColor: "var(--selection) !important",
  },
  // Brackets around the cursor outlined, never filled (§5.5).
  ".cm-matchingBracket, &.cm-focused .cm-matchingBracket": {
    backgroundColor: "transparent", outline: "1px solid var(--secondary)", outlineOffset: "-1px",
  },
  ".cm-nonmatchingBracket": { backgroundColor: "transparent", color: "var(--error)" },
  // The selection's other matches: the selection's color, lighter, under find's tint.
  ".cm-selectionMatch": { backgroundColor: "#41414180" },
  ".cm-searchMatch .cm-selectionMatch": { backgroundColor: "transparent" },
}, { dark: true });

// Carbon's code colors by IDE role (design-guide.md §2.6), on the tags
// lisp.js gives its categories. Code draws without bold or slant; a lexical
// error is red and underlined, so it is never shown by color alone.
const carbonCode = HighlightStyle.define([
  { tag: [tags.keyword, tags.definitionKeyword], color: "var(--codeSpecialForm)" },
  { tag: tags.atom, color: "var(--codeKeyword)" },
  { tag: [tags.function(tags.variableName), tags.definition(tags.variableName)], color: "var(--codeCall)" },
  { tag: tags.string, color: "var(--codeString)" },
  { tag: [tags.number, tags.special(tags.number)], color: "var(--codeNumber)" },
  { tag: tags.special(tags.atom), color: "var(--codeQuotedSymbol)" },
  { tag: tags.variableName, color: "var(--codeSymbol)" },
  { tag: [tags.comment, tags.punctuation], color: "var(--codeComment)" },
  { tag: tags.invalid, color: "var(--error)", textDecoration: "underline wavy var(--error)",
    textUnderlineOffset: "3px" },
]);

// The key scheme's editing keys (schemes.js), first so that they come before CodeMirror's own.
const schemeCompartment = new Compartment();

/** A transaction that changes a document's key scheme. */
export function useScheme(scheme) {
  return { effects: schemeCompartment.reconfigure(schemeKeys(scheme)) };
}

/**
 * A document's editor state, with `onUpdate(state)` called after each
 * transaction. A .lisp file is highlighted as the dialect.
 * @param {string} text
 * @param {string} path
 * @param {string} scheme the key scheme (schemes.js)
 * @param {(state: EditorState) => void} onUpdate
 * @param {import("@codemirror/state").Extension} extras such as completion (complete.js), for .lisp files
 */
export function editorState(text, path, scheme, onUpdate, extras = [], savedMarks = []) {
  return EditorState.create({
    doc: text,
    extensions: [
      schemeCompartment.of(schemeKeys(scheme)),
      lineNumbers(),
      marks(savedMarks),
      folding,
      flashField,
      find,
      highlightActiveLine(),
      highlightActiveLineGutter(),
      drawSelection(),
      path.endsWith(".lisp") ? [lisp, syntaxHighlighting(carbonCode), extras] : [],
      bracketMatching(),
      history(),
      indentUnit.of("  "),
      EditorState.tabSize.of(2),
      // CodeMirror's motion and deletion keys only: its other defaults take GNOME's standard keys or
      // Alt with a letter, so the CUA scheme binds the editing commands itself (patterns.md §22.5,
      // schemes.js). Tab indents; after Escape, Tab leaves the editor, as CodeMirror's view does
      // (design-guide.md §8.3).
      keymap.of([...closeBracketsKeymap, ...standardKeymap, indentWithTab]),
      // Several cursors; a column selection by Shift+Alt+drag (VS Code); the selection's other matches marked.
      EditorState.allowMultipleSelections.of(true),
      rectangularSelection({ eventFilter: (e) => e.altKey && e.shiftKey }),
      crosshairCursor({ key: "Alt" }),
      highlightSelectionMatches(),
      closeBrackets(),
      editingState,
      carbon,
      EditorView.contentAttributes.of({ "aria-label": "Code" }),
      EditorView.updateListener.of((update) => {
        if (update.transactions.length) onUpdate(update.state);
      }),
    ],
  });
}

// ---- Folding (patterns.md §5): the dialect's regions (lisp.js, foldRanges), cached for each text.

const foldCache = new WeakMap();
function foldsOf(state) {
  let folds = foldCache.get(state.doc);
  if (!folds) {
    folds = foldRanges(state.doc.toString());
    foldCache.set(state.doc, folds);
  }
  return folds;
}

class FoldMarker extends GutterMarker {
  constructor(folded) {
    super();
    this.folded = folded;
  }
  eq(other) {
    return other.folded === this.folded;
  }
  toDOM() {
    return h("span", { class: `cm-foldMarker${this.folded ? " folded" : ""}`, title: this.folded ? "Unfold" : "Fold" },
      icon(this.folded ? "right" : "down"));
  }
}
const openMarker = new FoldMarker(false);
const foldedMarker = new FoldMarker(true);

// The fold folded on the line [from, to], or null.
function foldedOn(state, from, to) {
  let found = null;
  foldedRanges(state).between(from, to, (a, b) => {
    if (!found && a === to) found = { from: a, to: b };
  });
  return found;
}

// The fold margin keeps a cell on every line, so an evaluated form's mark covers it (design-guide.md §4).
const folding = [
  foldService.of((state, lineStart) => foldsOf(state).get(lineStart) ?? null),
  codeFolding({ placeholderText: "…" }),
  gutter({
    class: "cm-foldMargin",
    renderEmptyElements: true,
    markers: (view) => {
      const marks = [];
      for (const { from, to } of view.visibleRanges) {
        for (let pos = from; pos <= to;) {
          const line = view.state.doc.lineAt(pos);
          if (foldedOn(view.state, line.from, line.to)) marks.push(foldedMarker.range(line.from));
          else if (foldable(view.state, line.from, line.to)) marks.push(openMarker.range(line.from));
          pos = line.to + 1;
        }
      }
      return RangeSet.of(marks, true);
    },
    lineMarkerChange: (update) => foldedRanges(update.startState) !== foldedRanges(update.state),
    domEventHandlers: {
      click: (view, block) => {
        // A folded line's block runs to the end of its fold; the fold starts at its first line's end.
        const line = view.state.doc.lineAt(block.from);
        const folded = foldedOn(view.state, line.from, line.to);
        if (folded) {
          view.dispatch({ effects: unfoldEffect.of(folded) });
          return true;
        }
        const range = foldable(view.state, line.from, line.to);
        if (!range) return false;
        view.dispatch({ effects: foldEffect.of(range) });
        return true;
      },
    },
  }),
];

// The lines of an evaluated form, while it is marked.
const flashEffect = StateEffect.define();
const flashField = StateField.define({
  create: () => Decoration.none,
  update(lines, tr) {
    lines = lines.map(tr.changes);
    for (const effect of tr.effects) {
      if (!effect.is(flashEffect)) continue;
      lines = Decoration.none;
      if (!effect.value) continue;
      const mark = Decoration.line({ class: "cm-evaluated" });
      const first = tr.state.doc.lineAt(effect.value.from).number;
      const last = tr.state.doc.lineAt(effect.value.to).number;
      const ranges = [];
      for (let n = first; n <= last; ++n) ranges.push(mark.range(tr.state.doc.line(n).from));
      lines = Decoration.set(ranges);
    }
    return lines;
  },
  provide: (field) => [
    EditorView.decorations.from(field),
    // The gutter beside the lines is marked too.
    gutterLineClass.compute([field], (state) => {
      const marks = [];
      for (const cursor = state.field(field).iter(); cursor.value; cursor.next()) marks.push(evaluatedGutter.range(cursor.from));
      return RangeSet.of(marks);
    }),
  ],
});

const evaluatedGutter = new class extends GutterMarker {
  elementClass = "cm-evaluated";
}();

/** A transaction that marks the lines of [from, to), or, with null, clears the mark. */
export function markEvaluated(range) {
  return { effects: flashEffect.of(range) };
}

/** The offset in `state` of a line and a column from 1, the column in code points (`interpreter.md` §3). */
export function offsetOf(state, line, column) {
  const at = state.doc.line(Math.min(Math.max(line, 1), state.doc.lines));
  const units = [...at.text].slice(0, Math.max(column - 1, 0)).join("").length;
  return at.from + units;
}

/**
 * A transaction that shows `problems` ({path, line, column, message}, all
 * of this document's file) as diagnostics: each underlines the token it
 * starts at. `view(problem)` is its View Problem action.
 */
export function showProblems(state, problems, view) {
  const diagnostics = problems.map((problem) => {
    const from = offsetOf(state, problem.line, problem.column);
    const line = state.doc.lineAt(from);
    const token = tokenize(line.text).find((t) => t.from <= from - line.from && from - line.from < t.to);
    const to = token ? line.from + token.to : Math.min(from + 1, line.to);
    return {
      from, to, severity: "error", message: problem.message,
      renderMessage: () => h("span", { class: "cm-problem" }, icon("error"),
        h("span", {}, problem.message, h("div", { class: "src" }, `espresso · ${where(problem)}`))),
      actions: [{ name: "View Problem", apply: () => view(problem) }],
    };
  });
  return setDiagnostics(state, diagnostics);
}

/** A problem's place as the console writes it: "loops.lisp:82:44". */
export function where(problem) {
  return `${problem.path}:${problem.line}:${problem.column}`;
}

export { EditorView, nextDiagnostic, previousDiagnostic };
