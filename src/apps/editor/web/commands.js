// The command table (patterns.md §19.8, §22, §25): every command the
// window runs by name, with the label the command picker and Keyboard
// Shortcuts show, the menu it belongs to, and where its keys act. The key
// schemes bind the names (schemes.js); main.js runs them.
//
// - `menu`: File, Edit, Selection, View, Go, Play, or Help, in the menu
//   bar's order (§19.8).
// - `general`: listed under General in Keyboard Shortcuts, as the HIG
//   asks of general keys (§25.4).
// - `editor`: an editing command, whose keys act only while the code has
//   focus, so a field's own keys (Ctrl+A, Ctrl+Z) stay the field's.
// - `menuLabel`: the menu bar's label where it differs: a setting reads as
//   an adjective and a command as a verb (the HIG's Menus).

/**
 * @typedef {{name: string, label: string, menu: string, general?: boolean, editor?: boolean, menuLabel?: string}} Command
 */

/** @type {Command[]} */
export const commandTable = [
  { name: "newFile", label: "New File…", menu: "File" },
  { name: "newProject", label: "New Project…", menu: "File" },
  { name: "openFolder", label: "Open Folder…", menu: "File" },
  { name: "openRecent", label: "Open Recent…", menu: "File" },
  { name: "openExample", label: "Open Example…", menu: "File" },
  { name: "save", label: "Save", menu: "File" },
  { name: "saveAs", label: "Save As…", menu: "File" },
  { name: "saveAll", label: "Save All", menu: "File" },
  { name: "renameFile", label: "Rename…", menu: "File" },
  { name: "trashFile", label: "Move to Trash", menu: "File" },
  { name: "copyPath", label: "Copy Path", menu: "File" },
  { name: "copyRelativePath", label: "Copy Relative Path", menu: "File" },
  { name: "openContainingFolder", label: "Open Containing Folder", menu: "File" },
  { name: "revealInSideBar", label: "Reveal in Side Bar", menu: "File" },
  { name: "closeBuffer", label: "Close Editor", menu: "File" },
  { name: "closeOthers", label: "Close Other Editors", menu: "File" },
  { name: "closeSaved", label: "Close Saved Editors", menu: "File" },
  { name: "closeAll", label: "Close All Editors", menu: "File" },
  { name: "reopenClosed", label: "Reopen Closed Editor", menu: "File" },
  { name: "closeFolder", label: "Close Folder", menu: "File" },
  { name: "quit", label: "Quit", menu: "File", general: true },

  { name: "undo", label: "Undo", menu: "Edit", editor: true },
  { name: "redo", label: "Redo", menu: "Edit", editor: true },
  { name: "cut", label: "Cut", menu: "Edit", editor: true },
  { name: "copy", label: "Copy", menu: "Edit", editor: true },
  { name: "paste", label: "Paste", menu: "Edit", editor: true },
  { name: "selectAll", label: "Select All", menu: "Edit", editor: true },
  { name: "selectNone", label: "Deselect All", menu: "Edit", editor: true },
  { name: "searchForward", label: "Find…", menu: "Edit" },
  { name: "replace", label: "Find and Replace…", menu: "Edit" },
  { name: "findInFiles", label: "Find in Files…", menu: "Edit" },
  { name: "searchNext", label: "Find Next", menu: "Edit" },
  { name: "searchPrevious", label: "Find Previous", menu: "Edit" },
  { name: "toggleComment", label: "Toggle Comment", menu: "Edit", editor: true },
  { name: "toggleBlockComment", label: "Toggle Block Comment", menu: "Edit", editor: true },

  { name: "expandSelection", label: "Expand Selection", menu: "Selection", editor: true },
  { name: "shrinkSelection", label: "Shrink Selection", menu: "Selection", editor: true },
  { name: "addCursorAbove", label: "Add Cursor Above", menu: "Selection", editor: true },
  { name: "addCursorBelow", label: "Add Cursor Below", menu: "Selection", editor: true },
  { name: "addNextOccurrence", label: "Add Next Occurrence", menu: "Selection", editor: true },
  { name: "selectAllOccurrences", label: "Select All Occurrences", menu: "Selection", editor: true },
  { name: "duplicate", label: "Duplicate", menu: "Selection", editor: true },
  { name: "moveLineUp", label: "Move Line Up", menu: "Selection", editor: true },
  { name: "moveLineDown", label: "Move Line Down", menu: "Selection", editor: true },
  { name: "deleteLine", label: "Delete Line", menu: "Selection", editor: true },
  { name: "indentMore", label: "Indent", menu: "Selection", editor: true },
  { name: "indentLess", label: "Unindent", menu: "Selection", editor: true },
  { name: "joinLines", label: "Join Lines", menu: "Selection", editor: true },
  { name: "sortLines", label: "Sort Lines", menu: "Selection", editor: true },
  { name: "upperCase", label: "Upper Case", menu: "Selection", editor: true },
  { name: "lowerCase", label: "Lower Case", menu: "Selection", editor: true },
  { name: "titleCase", label: "Title Case", menu: "Selection", editor: true },

  { name: "toggleFileList", label: "Toggle Side Bar", menu: "View", menuLabel: "Side Bar" },
  { name: "toggleConsole", label: "Toggle Panel", menu: "View", menuLabel: "Panel" },
  { name: "showConsole", label: "Show Console", menu: "View" },
  { name: "showProblems", label: "Show Problems", menu: "View" },
  { name: "showSearch", label: "Show Search", menu: "View" },
  { name: "showPage", label: "Show Page", menu: "View" },
  { name: "clearConsole", label: "Clear Console", menu: "View" },
  { name: "fold", label: "Fold", menu: "View" },
  { name: "unfold", label: "Unfold", menu: "View" },
  { name: "foldAll", label: "Fold All", menu: "View" },
  { name: "unfoldAll", label: "Unfold All", menu: "View" },
  { name: "toggleWordWrap", label: "Toggle Word Wrap", menu: "View", menuLabel: "Word Wrap" },
  { name: "zoomIn", label: "Zoom In", menu: "View" },
  { name: "zoomOut", label: "Zoom Out", menu: "View" },
  { name: "resetZoom", label: "Reset Zoom", menu: "View" },
  { name: "toggleFullscreen", label: "Toggle Fullscreen", menu: "View", menuLabel: "Fullscreen" },

  { name: "back", label: "Back", menu: "Go" },
  { name: "forward", label: "Forward", menu: "Go" },
  { name: "switchBuffer", label: "Go to File…", menu: "Go" },
  { name: "showAllCommands", label: "Show All Commands…", menu: "Go", general: true },
  { name: "goToLine", label: "Go to Line…", menu: "Go" },
  { name: "goToDefinition", label: "Go to Definition", menu: "Go" },
  { name: "goToMatchingBracket", label: "Go to Matching Bracket", menu: "Go", editor: true },
  { name: "nextProblem", label: "Next Problem", menu: "Go" },
  { name: "previousProblem", label: "Previous Problem", menu: "Go" },
  { name: "toggleMark", label: "Toggle Mark", menu: "Go", editor: true },
  { name: "nextMark", label: "Next Mark", menu: "Go" },
  { name: "previousMark", label: "Previous Mark", menu: "Go" },
  { name: "showMarks", label: "Show Marks…", menu: "Go" },
  { name: "clearMarks", label: "Clear Marks", menu: "Go" },
  { name: "focusNextPart", label: "Focus Next Part", menu: "Go" },
  { name: "focusPreviousPart", label: "Focus Previous Part", menu: "Go" },
  { name: "nextBuffer", label: "Next Editor", menu: "Go" },
  { name: "previousBuffer", label: "Previous Editor", menu: "Go" },

  { name: "play", label: "Play", menu: "Play" },
  { name: "update", label: "Update", menu: "Play" },
  { name: "stop", label: "Stop", menu: "Play" },
  { name: "evalForm", label: "Evaluate Form", menu: "Play" },
  { name: "evalLastForm", label: "Evaluate Last Form", menu: "Play" },
  { name: "toggleUpdateOnSave", label: "Toggle Update on Save", menu: "Play", menuLabel: "Update on Save" },
  { name: "savePage", label: "Save Page as ui/index.html", menu: "Play" },

  { name: "showLanguageReference", label: "Language Reference", menu: "Help" },
  { name: "showLibraryReference", label: "Standard Library", menu: "Help" },
  { name: "showShortcuts", label: "Keyboard Shortcuts", menu: "Help", general: true },
  { name: "preferences", label: "Preferences", menu: "Edit", general: true },
  { name: "changeKeyScheme", label: "Change Key Scheme…", menu: "Help", general: true },
  { name: "showAbout", label: "About espresso", menu: "Help" },
];

/** The menus in the menu bar's order, then General, for Keyboard Shortcuts (§25.4). */
export const menuOrder = ["General", "File", "Edit", "Selection", "View", "Go", "Play", "Help"];

/** The command named `name`, or undefined. */
export const commandNamed = (name) => commandTable.find((c) => c.name === name);

// What a command acts on, where that limits where it runs: "file", a file of the project (a reference has no
// path); "code", the code's view (not a reference's prose, nor the page); "text", the code or a reference's prose
// (Copy, Find). The editor's own commands (`editor`) act on the code. searchBackward is nano's, in no menu.
const kinds = {
  save: "file", saveAs: "file", renameFile: "file", trashFile: "file", copyPath: "file", copyRelativePath: "file",
  openContainingFolder: "file", revealInSideBar: "file", evalForm: "file", evalLastForm: "file",
  replace: "code", fold: "code", unfold: "code", foldAll: "code", unfoldAll: "code", goToLine: "code",
  goToDefinition: "code", goToMatchingBracket: "code", nextMark: "code", previousMark: "code", clearMarks: "code",
  toggleMark: "code",
  copy: "text", searchForward: "text", searchBackward: "text", searchNext: "text", searchPrevious: "text",
};

/** What command `name` acts on: "file", "code", "text", or undefined for the window as a whole. */
export function actsOn(name) {
  return kinds[name] ?? (commandNamed(name)?.editor ? "code" : undefined);
}

