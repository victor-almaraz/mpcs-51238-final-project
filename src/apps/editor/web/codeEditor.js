// The text editor: CodeMirror 6 with the editor surface of the design
// guide (brand/gui/design-guide.md §4, §5.5) in Carbon, and the CUA keys
// (patterns.md §14.3), the dialect's highlighting (tauri-editor.md §5.4),
// and the run's problems as diagnostics: a wavy underline, an icon in the
// glyph margin, and a hover (design-guide.md §5.5).

import { Compartment, EditorState, RangeSet, StateEffect, StateField } from "@codemirror/state";
import {
  Decoration, EditorView, GutterMarker, gutter, gutterLineClass, keymap, lineNumbers, highlightActiveLine,
  highlightActiveLineGutter, drawSelection,
} from "@codemirror/view";
import { lintGutter, nextDiagnostic, setDiagnostics } from "@codemirror/lint";
import { defaultKeymap, history, historyKeymap, indentWithTab } from "@codemirror/commands";
import { bracketMatching, indentUnit, HighlightStyle, syntaxHighlighting } from "@codemirror/language";
import { tags } from "@lezer/highlight";
import { lisp, tokenize } from "./lisp.js";
import { schemeKeys } from "./schemes.js";
import { h } from "./dom.js";
import { icon } from "./icons.js";

// Carbon's error icon (icons.js) for the glyph margin, as an image.
const errorMark = "url(\"data:image/svg+xml," + encodeURIComponent(
  '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" width="14" height="14">' +
  '<circle cx="8" cy="8" r="6.5" fill="#ff6b6b"/>' +
  '<path d="M5.8 5.8l4.4 4.4M10.2 5.8l-4.4 4.4" stroke="#000" stroke-width="1.5"/></svg>') + "\")";

// Carbon's editor surface, from the stylesheet's custom properties.
const carbon = EditorView.theme({
  "&": { height: "100%", backgroundColor: "var(--window)", color: "var(--text)" },
  ".cm-scroller": { font: "14px/22px var(--mono)", paddingTop: "6px" },
  ".cm-content": { padding: "0", caretColor: "var(--bright)" },
  ".cm-line": { padding: "0 0 0 1px" },
  ".cm-cursor, .cm-dropCursor": { borderLeft: "2px solid var(--bright)", marginLeft: "-1px" },
  ".cm-activeLine": { backgroundColor: "var(--lineCurrent)" },
  // A glyph margin of 22 px, line numbers in 34, and a fold margin of 22
  // (design-guide.md §4).
  ".cm-gutters": { backgroundColor: "var(--window)", color: "var(--lineNumberText)", border: "none" },
  ".cm-foldMargin": { width: "22px" },
  ".cm-gutter-lint": { width: "22px" },
  ".cm-gutter-lint .cm-gutterElement": { padding: "0", display: "grid", placeItems: "center" },
  ".cm-lint-marker": { width: "14px", height: "14px" },
  ".cm-lint-marker-error": { content: errorMark },
  // An error's range, and its hover (§5.5): the message, where it is, and View Problem.
  ".cm-lintRange-error": { backgroundImage: "none", textDecoration: "underline wavy var(--error)",
                           textDecorationSkipInk: "none", textUnderlineOffset: "3px" },
  ".cm-lintRange-active": { backgroundColor: "transparent" },
  ".cm-tooltip": { backgroundColor: "var(--box)", color: "var(--bright)", border: "none",
                   outline: "1px solid var(--boxRule)" },
  ".cm-tooltip.cm-tooltip-lint": { width: "420px", padding: "0", font: "13px/1.5 var(--ui)" },
  ".cm-diagnostic, .cm-diagnostic-error": { padding: "0", margin: "0", border: "none" },
  ".cm-diagnosticText": { display: "block", padding: "8px 10px" },
  ".cm-problem": { display: "flex", gap: "8px" },
  ".cm-problem .icon": { color: "var(--error)", marginTop: "2px" },
  ".cm-problem .src": { color: "var(--secondary)", font: "12px var(--mono)" },
  ".cm-diagnosticAction": { display: "block", width: "100%", margin: "0", padding: "4px 10px", border: "none",
                            borderRadius: "0", background: "var(--side)", color: "var(--accentText)",
                            font: "13px var(--ui)", textAlign: "left", cursor: "pointer" },
  ".cm-diagnosticAction:hover": { background: "var(--tabHover)" },
  // The Vim and Emacs schemes' command lines; Vim's block cursor is editor.css's.
  ".cm-panels": { backgroundColor: "var(--side)", color: "var(--bright)" },
  ".cm-panels.cm-panels-bottom": { borderTop: "none" },
  ".cm-vim-panel": { padding: "2px 12px", font: "13px/20px var(--mono)" },
  ".cm-vim-panel input": { color: "var(--bright)", font: "inherit" },
  // An evaluated form's mark is editor.css's .cm-evaluated, which reduced motion changes.
  ".cm-lineNumbers .cm-gutterElement": { minWidth: "34px", padding: "0", textAlign: "right" },
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
 */
export function editorState(text, path, scheme, onUpdate) {
  return EditorState.create({
    doc: text,
    extensions: [
      schemeCompartment.of(schemeKeys(scheme)),
      lintGutter(),
      lineNumbers(),
      gutter({ class: "cm-foldMargin", renderEmptyElements: true }),  // folding comes later; the margin is kept
      flashField,
      highlightActiveLine(),
      highlightActiveLineGutter(),
      drawSelection(),
      path.endsWith(".lisp") ? [lisp, syntaxHighlighting(carbonCode)] : [],
      bracketMatching(),
      history(),
      indentUnit.of("  "),
      EditorState.tabSize.of(2),
      keymap.of([...defaultKeymap, ...historyKeymap, indentWithTab]),
      carbon,
      EditorView.contentAttributes.of({ "aria-label": "Code" }),
      EditorView.updateListener.of((update) => {
        if (update.transactions.length) onUpdate(update.state);
      }),
    ],
  });
}

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

export { EditorView, nextDiagnostic };
