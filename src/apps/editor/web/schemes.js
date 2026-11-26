// The key schemes (tauri-editor.md §5.3, tui.md §8.2): CUA, Emacs, Vim, and
// nano. Each binds the editor's commands, named as the TUI names them
// (tui.md §8.1), from the TUI's tables (src/tui/keymap.cpp). Editing keys
// come from CodeMirror's own keymap (CUA), @replit/codemirror-emacs,
// @replit/codemirror-vim, and the nano table below; the commands that act
// on the window go to the runner the window sets.
//
// Bindings are data: `keys` are single chords in CodeMirror's notation,
// matched by the window for every focus; Emacs's `chords` are key
// sequences in the Emacs package's notation and Vim's `ex` and `normal`
// are its commands and mappings, all three acting in the editor.

import { EditorSelection, Prec, StateEffect, StateField } from "@codemirror/state";
import { keymap } from "@codemirror/view";
import {
  cursorCharLeft, cursorCharRight, cursorDocEnd, cursorDocStart, cursorGroupBackward, cursorGroupForward,
  cursorLineBoundaryBackward, cursorLineDown, cursorLineEnd, cursorLineUp, cursorPageDown, cursorPageUp,
  deleteCharForward, redo, selectCharLeft, selectCharRight, selectDocEnd, selectDocStart, selectGroupBackward,
  selectGroupForward, selectLineBoundaryBackward, selectLineDown, selectLineEnd, selectLineUp, selectPageDown,
  selectPageUp, undo,
} from "@codemirror/commands";
import { emacs, EmacsHandler } from "@replit/codemirror-emacs";
import { CodeMirror, Vim, getCM, vim } from "@replit/codemirror-vim";
import { isMac, keyLabel } from "./dom.js";

/** Every scheme's F keys (keymap.cpp, common). */
export const common = {
  F1: "showShortcuts", F5: "play", F6: "stop", F7: "toggleFileList", F8: "evalLastForm", F9: "toggleConsole",
  F10: "mainMenu",
};

/** The schemes, by the name settings use. */
export const schemes = {
  cua: {
    label: "CUA",
    keys: {
      "Mod-o": "openFolder", "Mod-s": "save", "Mod-w": "closeBuffer", "Mod-q": "quit",
      "Mod-PageDown": "nextBuffer", "Mod-PageUp": "previousBuffer", "Mod-e": "evalLastForm",
      "Mod-p": "switchBuffer", "Mod-f": "searchForward", "Mod-r": "searchBackward", "F3": "searchNext",
      "Shift-F3": "searchPrevious",
      // The GUI's own, after VS Code (patterns.md §14.3).
      "Mod-Enter": "evalForm", "Shift-Enter": "evalLastForm", "Mod-.": "stop", "Mod-b": "toggleFileList",
      "Mod-j": "toggleConsole", "Mod-Shift-m": "showProblems", "Mod-Shift-p": "showAllCommands", "Mod-h": "replace",
      "Ctrl-g": "goToLine", "Mod-Alt-[": "fold", "Mod-Alt-]": "unfold",
    },
  },
  emacs: {
    label: "Emacs",
    chords: {
      "C-x C-f": "openFolder", "C-x C-s": "save", "C-x k": "closeBuffer", "C-x C-c": "quit",
      "C-x Right": "nextBuffer", "C-x Left": "previousBuffer", "C-x C-e": "evalLastForm", "C-M-x": "evalForm",
      "C-c C-c": "play", "C-c C-s": "stop", "C-c f": "toggleFileList", "C-c o": "toggleConsole",
      "C-x b": "switchBuffer", "M-x": "showAllCommands", "M-g": "goToLine",
      // The Emacs package's own C-s and C-r open find (searchForward, searchBackward).
    },
  },
  vim: {
    label: "Vim",
    // [name, abbreviation, command]
    ex: [
      ["write", "w", "save"], ["quit", "q", "quit"], ["wq", "wq", "saveAndQuit"], ["xit", "x", "saveAndQuit"],
      ["edit", "e", "openFolder"], ["bdelete", "bd", "closeBuffer"], ["bnext", "bn", "nextBuffer"],
      ["bprevious", "bp", "previousBuffer"], ["play", "play", "play"], ["stop", "stop", "stop"],
      ["eval", "eval", "evalLastForm"], ["filelist", "filelist", "toggleFileList"],
      ["console", "console", "toggleConsole"], ["buffer", "b", "switchBuffer"],
    ],
    normal: { "<C-x><C-e>": ":eval<CR>", gt: ":bnext<CR>", gT: ":bprevious<CR>" },
  },
  nano: {
    label: "nano",
    keys: {
      "Ctrl-o": "save", "Ctrl-r": "openFolder", "Alt-k": "closeBuffer", "Ctrl-x": "quit",
      "Alt-.": "nextBuffer", "Alt-,": "previousBuffer", "Alt-r": "evalLastForm", "Alt-b": "switchBuffer",
      "Ctrl-w": "searchForward", "Ctrl-q": "searchBackward", "Alt-w": "searchNext", "Alt-q": "searchPrevious",
      "Alt-g": "goToLine",
    },
  },
};

export const schemeNames = Object.keys(schemes);

// ---- The window's commands, run by name.

let runCommand = () => false;

/** Sets the function that runs a command by name; it answers false when the command does not apply. */
export function setCommandRunner(run) {
  runCommand = run;
}

for (const [keys, command] of Object.entries(schemes.emacs.chords)) {
  EmacsHandler.bindKey(keys, { exec: () => runCommand(command) });
}
for (const [name, abbreviation, command] of schemes.vim.ex) {
  // `:q!` and `:x!` read "!" as the command's argument.
  Vim.defineEx(name, abbreviation, (cm, params) =>
    runCommand(command === "quit" && params.argString?.trim() === "!" ? "quitWithoutSaving" : command));
}
CodeMirror.commands.save = () => runCommand("save");  // Vim's own :w calls it
for (const [keys, ex] of Object.entries(schemes.vim.normal)) Vim.map(keys, ex, "normal");

// ---- Chords: a binding's and a key event's names, the same for the same keys.

const modifierOrder = ["Alt", "Ctrl", "Meta", "Shift"];
const codeKeys = {
  Comma: ",", Period: ".", Slash: "/", Backslash: "\\", Minus: "-", Equal: "=", Space: "Space", BracketLeft: "[",
  BracketRight: "]",
};

function chordOf(modifiers, key) {
  const name = key.length === 1 ? key.toLowerCase() : key;
  return [...modifierOrder.filter((m) => modifiers.has(m)), name].join("-");
}

/** A binding's chord name: "Mod-Shift-m" is "Meta-Shift-m" on macOS and "Ctrl-Shift-m" elsewhere. */
export function bindingChord(binding) {
  const parts = binding.split(/-(?!$)/);
  const key = parts.pop();
  return chordOf(new Set(parts.map((m) => (m === "Mod" ? (isMac ? "Meta" : "Ctrl") : m))), key);
}

/**
 * A key event's chord name. With a modifier the key is read from its
 * position, so Option+K on macOS is Alt-k and not "˚".
 */
export function eventChord(e) {
  let key = e.key === " " ? "Space" : e.key;
  if ((e.altKey || e.ctrlKey || e.metaKey) && e.code) {
    if (/^Key[A-Z]$/.test(e.code)) key = e.code.slice(3);
    else if (/^Digit\d$/.test(e.code)) key = e.code.slice(5);
    else if (codeKeys[e.code]) key = codeKeys[e.code];
  }
  const modifiers = new Set([e.altKey && "Alt", e.ctrlKey && "Ctrl", e.metaKey && "Meta", e.shiftKey && "Shift"]);
  return chordOf(modifiers, key);
}

/** The window's single chords in `scheme`, by chord name: its own, then the F keys. */
export function chordTable(scheme) {
  const table = new Map();
  for (const [binding, command] of Object.entries({ ...common, ...schemes[scheme].keys })) {
    table.set(bindingChord(binding), command);
  }
  return table;
}

/** How `scheme` writes the first binding of `command`, for menus and tooltips; "" when it has none. */
export function bindingLabel(scheme, command) {
  const s = schemes[scheme];
  const find = (table) => Object.entries(table ?? {}).find(([, c]) => c === command)?.[0];
  const ex = s.ex?.find(([, , c]) => c === command);
  if (ex) return `:${ex[1]}`;
  const chord = find(s.chords);
  if (chord) return chord;
  const key = find(s.keys);
  if (key && scheme === "nano") return key.replace(/^Ctrl-/, "^").replace(/^Alt-/, "M-").toUpperCase();
  if (key) return keyLabel(key.replace(/-(?!$)/g, "+").replace(/\+([a-z])$/, (_, c) => `+${c.toUpperCase()}`));
  return find(common) ?? "";
}

// ---- nano's editing keys (keymap.cpp, nano), with its mark and cutbuffer.

const setMark = StateEffect.define();
const markField = StateField.define({
  create: () => false,
  update(on, tr) {
    for (const e of tr.effects) if (e.is(setMark)) on = e.value;
    return tr.docChanged ? false : on;
  },
});
const marked = (view) => view.state.field(markField, false) ?? false;
// A motion that extends the selection while the mark is set.
const motion = (go, extend) => (view) => (marked(view) ? extend(view) : go(view));

let cutbuffer = "";

// The selection, or else the cursor's whole line, for cut and copy.
function cutRange(view) {
  const { from, to, empty } = view.state.selection.main;
  if (!empty) return { from, to };
  const line = view.state.doc.lineAt(from);
  return { from: line.from, to: Math.min(line.to + 1, view.state.doc.length) };
}

const nanoKeymap = [
  { key: "Ctrl-f", run: motion(cursorCharRight, selectCharRight) },
  { key: "Ctrl-b", run: motion(cursorCharLeft, selectCharLeft) },
  { key: "Ctrl-n", run: motion(cursorLineDown, selectLineDown) },
  { key: "Ctrl-p", run: motion(cursorLineUp, selectLineUp) },
  { key: "Ctrl-Space", run: motion(cursorGroupForward, selectGroupForward) },
  { key: "Alt-Space", run: motion(cursorGroupBackward, selectGroupBackward) },
  { key: "Ctrl-a", run: motion(cursorLineBoundaryBackward, selectLineBoundaryBackward) },
  { key: "Ctrl-e", run: motion(cursorLineEnd, selectLineEnd) },
  { key: "Ctrl-v", run: motion(cursorPageDown, selectPageDown) },
  { key: "Ctrl-y", run: motion(cursorPageUp, selectPageUp) },
  { key: "Alt-\\", run: motion(cursorDocStart, selectDocStart) },
  { key: "Alt-/", run: motion(cursorDocEnd, selectDocEnd) },
  { key: "Ctrl-d", run: deleteCharForward },
  { key: "Alt-u", run: undo },
  { key: "Alt-e", run: redo },
  { key: "Alt-a", run: (view) => {
    view.dispatch({ effects: setMark.of(!marked(view)), selection: EditorSelection.cursor(view.state.selection.main.head) });
    return true;
  } },
  { key: "Ctrl-k", run: (view) => {
    const range = cutRange(view);
    cutbuffer = view.state.sliceDoc(range.from, range.to);
    view.dispatch({ changes: range, selection: EditorSelection.cursor(range.from) });
    return true;
  } },
  { key: "Alt-6", run: (view) => {
    const range = cutRange(view);
    cutbuffer = view.state.sliceDoc(range.from, range.to);
    view.dispatch({ effects: setMark.of(false), selection: EditorSelection.cursor(view.state.selection.main.head) });
    return true;
  } },
  { key: "Ctrl-u", run: (view) => {
    const at = view.state.selection.main.head;
    view.dispatch({ changes: { from: at, insert: cutbuffer }, selection: EditorSelection.cursor(at + cutbuffer.length) });
    return true;
  } },
];

/** The editing keys of `scheme`, before CodeMirror's own keymap. */
export function schemeKeys(scheme) {
  if (scheme === "vim") return vim();
  if (scheme === "emacs") return emacs();
  if (scheme === "nano") return [markField, Prec.high(keymap.of(nanoKeymap))];
  return [];
}

/** Calls `onMode(mode)` with Vim's mode ("normal", "insert", "visual", "replace") as it changes in `view`. */
export function watchVimMode(view, onMode) {
  getCM(view)?.on("vim-mode-change", (e) => onMode(e.mode));
}
