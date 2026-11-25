// The text editor: CodeMirror 6 with the editor surface of the design
// guide (brand/gui/design-guide.md §4, §5.5) in Carbon, and the CUA keys
// (patterns.md §14.3), and the dialect's highlighting (tauri-editor.md §5.4).

import { EditorState } from "@codemirror/state";
import {
  EditorView, keymap, lineNumbers, highlightActiveLine, highlightActiveLineGutter, drawSelection,
} from "@codemirror/view";
import { defaultKeymap, history, historyKeymap, indentWithTab } from "@codemirror/commands";
import { bracketMatching, indentUnit, HighlightStyle, syntaxHighlighting } from "@codemirror/language";
import { tags } from "@lezer/highlight";
import { lisp } from "./lisp.js";

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
  ".cm-gutters": { backgroundColor: "var(--window)", color: "var(--lineNumberText)", border: "none",
                   paddingLeft: "22px", paddingRight: "22px" },
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

/**
 * A document's editor state, with `onUpdate(state)` called after each
 * change of text or selection. A .lisp file is highlighted as the dialect.
 * @param {string} text
 * @param {string} path
 * @param {(state: EditorState) => void} onUpdate
 */
export function editorState(text, path, onUpdate) {
  return EditorState.create({
    doc: text,
    extensions: [
      lineNumbers(),
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
        if (update.docChanged || update.selectionSet) onUpdate(update.state);
      }),
    ],
  });
}

export { EditorView };
