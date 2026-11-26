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

/**
 * Every scheme's keys: the TUI's F keys (keymap.cpp, common) where GNOME's
 * standard keys leave them free (patterns.md §25.3), as F1 is Help, F5
 * Refresh, F9 the side pane, and F10 the menu bar's (GTK's and the HIG's
 * key for it); Play and Keyboard Shortcuts, which those F keys held; Help
 * (F1, the language reference until there is a user guide) and Preferences.
 */
export const common = {
  F6: "stop", F8: "nextProblem", "Shift-F8": "previousProblem", F9: "toggleFileList", "Mod-F9": "toggleConsole",
  F12: "goToDefinition", "Mod-Shift-Space": "play", "Mod-Shift-/": "showShortcuts",
  F1: "showLanguageReference", "Mod-,": "preferences", "Mod-Shift-k": "showAllCommands",
  "Mod-F6": "focusNextPart", "Mod-Shift-F6": "focusPreviousPart",
};

/** The schemes, by the name settings use. */
export const schemes = {
  cua: {
    label: "CUA",
    // The TUI's CUA keys, then the GUI's after VS Code (patterns.md §14.3), with GNOME's standard
    // keys where the two conflict (§25.2). Alt with a letter is never a key: it is an access key.
    keys: {
      "Mod-n": "newFile", "Mod-o": "openFolder", "Mod-s": "save", "Mod-w": "closeBuffer",
      "Mod-Shift-w": "closeAll", "F2": "renameFile", "Mod-q": "quit", "Mod-Shift-s": "saveAs", "Mod-Alt-s": "saveAll",
      "Mod-Alt-o": "openRecent", "Mod-Shift-t": "reopenClosed", "Mod-Shift-f": "findInFiles",
      "Mod-PageDown": "nextBuffer", "Mod-PageUp": "previousBuffer", "Ctrl-Tab": "nextBuffer",
      "Ctrl-Shift-Tab": "previousBuffer",
      "Mod-k": "switchBuffer", "Mod-Shift-k": "showAllCommands", "Mod-i": "goToLine",
      "Mod-f": "searchForward", "Mod-h": "replace", "Mod-g": "searchNext", "Mod-Shift-g": "searchPrevious",
      "F3": "searchNext", "Shift-F3": "searchPrevious",
      "Mod-.": "stop", "Mod-Enter": "evalForm", "Shift-Enter": "evalLastForm",
      "Mod-Shift-m": "showProblems", "Mod-Alt-[": "fold", "Mod-Alt-]": "unfold",
      "Mod-=": "zoomIn", "Mod-Shift-=": "zoomIn", "Mod--": "zoomOut", "Mod-0": "resetZoom", F11: "toggleFullscreen",
      "Alt-ArrowLeft": "back", "Alt-ArrowRight": "forward",
      // Marks (§21.7): the HIG's Add Bookmark and Edit Bookmarks; gedit's next and previous.
      "Mod-d": "toggleMark", "Mod-Alt-d": "showMarks", "Mod-b": "nextMark", "Mod-Shift-b": "previousMark",
      // Editing (§22): these act only from the code.
      "Mod-z": "undo", "Mod-Shift-z": "redo", "Mod-a": "selectAll", "Mod-Shift-a": "selectNone",
      "Mod-/": "toggleComment", "Mod-Alt-/": "toggleBlockComment",
      "Shift-Alt-ArrowRight": "expandSelection", "Shift-Alt-ArrowLeft": "shrinkSelection",
      "Mod-Alt-ArrowUp": "addCursorAbove", "Mod-Alt-ArrowDown": "addCursorBelow",
      "Mod-e": "addNextOccurrence", "Mod-Shift-l": "selectAllOccurrences",
      "Mod-u": "duplicate", "Alt-ArrowUp": "moveLineUp", "Alt-ArrowDown": "moveLineDown",
      "Mod-Shift-Delete": "deleteLine", "Mod-]": "indentMore", "Mod-[": "indentLess",
      "Mod-j": "joinLines", "Mod-Shift-j": "sortLines", "Mod-Shift-\\": "goToMatchingBracket",
    },
    // macOS's own where Alt with an arrow moves by word or paragraph there (VS Code's macOS keys).
    macKeys: {
      "Ctrl--": "back", "Ctrl-Shift--": "forward", "Ctrl-Mod-f": "toggleFullscreen",
      "Ctrl-Shift-ArrowRight": "expandSelection", "Ctrl-Shift-ArrowLeft": "shrinkSelection",
    },
    // Keys the webview handles itself, so the clipboard and its events stay the system's; named
    // here for menus and Keyboard Shortcuts only.
    shown: { "Mod-x": "cut", "Mod-c": "copy", "Mod-v": "paste" },
  },
  emacs: {
    label: "Emacs",
    chords: {
      "C-x C-f": "openFolder", "C-x C-s": "save", "C-x k": "closeBuffer", "C-x C-c": "quit",
      "C-x Right": "nextBuffer", "C-x Left": "previousBuffer", "C-x C-e": "evalLastForm", "C-M-x": "evalForm",
      "C-c C-c": "play", "C-c C-s": "stop", "C-c f": "toggleFileList", "C-c o": "toggleConsole",
      "C-x b": "switchBuffer", "M-x": "showAllCommands", "M-g": "goToLine",
      "C-x r m": "toggleMark", "C-x r l": "showMarks",
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

// A scheme's single chords for this platform: on macOS its `macKeys` take the place of its keys for the same commands.
function keysOf(scheme) {
  const s = schemes[scheme];
  if (!isMac || !s.macKeys) return s.keys ?? {};
  const mac = new Set(Object.values(s.macKeys));
  return { ...Object.fromEntries(Object.entries(s.keys).filter(([, c]) => !mac.has(c))), ...s.macKeys };
}

/** The window's single chords in `scheme`, by chord name: its own, then the F keys. */
export function chordTable(scheme) {
  const table = new Map();
  for (const [binding, command] of Object.entries({ ...common, ...keysOf(scheme) })) {
    table.set(bindingChord(binding), command);
  }
  return table;
}

/**
 * The single chord that runs `command` in `scheme`, in CodeMirror's
 * notation ("Mod-Shift-k"), for a menu's accelerator; null when the scheme
 * runs it by a key sequence, an ex command, or not at all. Keys that type
 * (no Ctrl, Alt, or Cmd, and not an F key) are never accelerators: they act
 * only from the code.
 */
export function bindingOf(scheme, command) {
  const s = schemes[scheme];
  const find = (table) => Object.entries(table ?? {}).find(([, c]) => c === command)?.[0];
  if (s.ex?.some(([, , c]) => c === command) || find(s.chords)) return null;
  const binding = find(keysOf(scheme)) ?? find(s.shown) ?? find(common);
  if (!binding) return null;
  return /(^|-)(Mod|Ctrl|Alt|Meta)-|^(Shift-)?F\d+$/.test(binding) ? binding : null;
}

/** How `scheme` writes every binding of `command`, for Keyboard Shortcuts: ["⌘S"], ["F3", "Ctrl+G"]. */
export function bindingLabels(scheme, command) {
  const s = schemes[scheme];
  const all = (table) => Object.entries(table ?? {}).filter(([, c]) => c === command).map(([b]) => b);
  const labels = [
    ...(s.ex ?? []).filter(([, , c]) => c === command).map(([, abbreviation]) => `:${abbreviation}`),
    ...all(s.chords),
    ...[...all(keysOf(scheme)), ...all(s.shown)].map((key) => (scheme === "nano"
      ? key.replace(/^Ctrl-/, "^").replace(/^Alt-/, "M-").toUpperCase()
      : keyLabel(key.replace(/-(?!$)/g, "+").replace(/\+([a-z])$/, (_, c) => `+${c.toUpperCase()}`)))),
    ...all(common).map((key) => keyLabel(key.replace(/-(?!$)/g, "+").replace(/\+([a-z])$/, (_, c) => `+${c.toUpperCase()}`))),
  ];
  return [...new Set(labels)];
}

/** How `scheme` writes the first binding of `command`, for menus and tooltips; "" when it has none. */
export function bindingLabel(scheme, command) {
  const s = schemes[scheme];
  const find = (table) => Object.entries(table ?? {}).find(([, c]) => c === command)?.[0];
  const ex = s.ex?.find(([, , c]) => c === command);
  if (ex) return `:${ex[1]}`;
  const chord = find(s.chords);
  if (chord) return chord;
  const key = find(keysOf(scheme)) ?? find(s.shown);
  if (key && scheme === "nano") return key.replace(/^Ctrl-/, "^").replace(/^Alt-/, "M-").toUpperCase();
  const binding = key ?? find(common);
  if (binding) return keyLabel(binding.replace(/-(?!$)/g, "+").replace(/\+([a-z])$/, (_, c) => `+${c.toUpperCase()}`));
  return "";
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
