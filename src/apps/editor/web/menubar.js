// The native menu bar (patterns.md §19.8, §24.7): the menus as data, the
// model they resolve to for the window's state and key scheme, and the bar
// Tauri builds from it — the system bar on macOS, a GTK bar in the window
// on Linux. Every item is a command of the command table (commands.js), so
// the bar, the command picker, and Keyboard Shortcuts read alike.
//
// GNOME's rules for menus govern it (§19.8): header capitalization, an
// access key on every item on Linux, an ellipsis only where the item asks
// for more, items disabled and never hidden, 3 to 12 items a menu, 3 to 6 a
// submenu, no nested submenus.

import { commandNamed } from "./commands.js";
import { isMac } from "./dom.js";

const separator = "|";

/**
 * The menus, by command name; `separator` between groups, and
 * `{submenu, items}` for a submenu. On macOS the app menu holds About and
 * Quit, and Window the system's window items (the Apple HIG's menu bar).
 */
export function menuLayout(mac = isMac) {
  return [
    ...(mac ? [{ label: "espresso", app: true, items: ["showAbout", separator, "preferences", separator,
      { predefined: "Services" }, separator,
      { predefined: "Hide" }, { predefined: "HideOthers" }, { predefined: "ShowAll" }, separator, "quit"] }] : []),
    { label: "File", items: ["newFile", "openFolder", "openRecent", separator, "save", "saveAs", "saveAll", separator,
      "renameFile", "trashFile", separator, "closeBuffer", "closeAll", "closeFolder",
      ...(mac ? [] : [separator, "quit"])] },
    // macOS: the system's own editing items, so fields and the code keep the system's undo and clipboard.
    { label: "Edit", items: [...(mac
      ? [{ predefined: "Undo" }, { predefined: "Redo" }, separator, { predefined: "Cut" }, { predefined: "Copy" },
        { predefined: "Paste" }, { predefined: "SelectAll" }]
      : ["undo", "redo", separator, "cut", "copy", "paste", "selectAll"]), "selectNone", separator,
    "searchForward", "replace", "findInFiles", separator, "toggleComment", ...(mac ? [] : [separator, "preferences"])] },
    { label: "Selection", items: ["expandSelection", "shrinkSelection", separator, "addCursorAbove", "addCursorBelow",
      "addNextOccurrence", "selectAllOccurrences", separator, "duplicate", "moveLineUp", "moveLineDown", "deleteLine", "toggleBlockComment",
      separator, { submenu: "Transform", items: ["joinLines", "sortLines", "upperCase", "lowerCase", "titleCase"] }] },
    { label: "View", items: ["toggleFileList", "toggleConsole", separator, "showConsole", "showProblems", "showSearch",
      "showPage", separator, "toggleWordWrap", { submenu: "Folding", items: ["fold", "unfold", "foldAll", "unfoldAll"] }, separator,
      "zoomIn", "zoomOut", "resetZoom", separator, "toggleFullscreen"] },
    { label: "Go", items: ["back", "forward", "nextBuffer", "previousBuffer", separator, "switchBuffer", "showAllCommands", "goToLine", "goToDefinition",
      "goToMatchingBracket", separator, "nextProblem", "previousProblem", separator,
      { submenu: "Marks", items: ["toggleMark", "nextMark", "previousMark", "showMarks", "clearMarks"] }] },
    { label: "Play", items: ["play", "stop", separator, "evalForm", "evalLastForm", separator, "toggleUpdateOnSave"] },
    ...(mac ? [{ label: "Window", window: true, items: [{ predefined: "Minimize" }, { predefined: "Maximize" }, separator,
      { predefined: "BringAllToFront" }] }] : []),
    { label: "Help", help: true, items: ["showLanguageReference", "showLibraryReference", separator, "showShortcuts",
      ...(mac ? [] : [separator, "showAbout"])] },
  ];
}

// Access keys (Linux): each item's first letter not taken in its menu, else its next; "&" marks it, as muda reads.
// The drawn menus (main.js) take theirs from here too.
export function withAccessKeys(labels) {
  const taken = new Set();
  return labels.map((label) => {
    if (label === null) return null;
    const letters = [...label].map((c, i) => [c, i]).filter(([c]) => /\p{L}|\p{N}/u.test(c));
    const words = letters.filter(([, i]) => i === 0 || /\s/.test(label[i - 1]));
    const pick = [...words, ...letters].find(([c]) => !taken.has(c.toLowerCase()));
    if (!pick) return label;
    taken.add(pick[0].toLowerCase());
    return label.slice(0, pick[1]) + "&" + label.slice(pick[1]);
  });
}

/**
 * The menu bar for the window's state: [{label, items}], each item
 * {kind: "command", name, label, accelerator, enabled, checked} (checked
 * undefined for a command, true or false for a setting), {kind:
 * "separator"}, {kind: "predefined", item}, or {kind: "submenu", label,
 * items}. `state` gives each command's label, accelerator, enabled, and
 * checked: {label(name), accelerator(name), enabled(name), checked(name)}.
 */
export function menuModel(state, mac = isMac) {
  const resolve = (items) => {
    const resolved = items.map((item) => {
      if (item === separator) return { kind: "separator" };
      if (item.predefined) return { kind: "predefined", item: item.predefined };
      if (item.submenu) return { kind: "submenu", label: item.submenu, items: resolve(item.items) };
      const command = commandNamed(item);
      return { kind: "command", name: item, label: state.label(item) ?? command.menuLabel ?? command.label,
        accelerator: state.accelerator(item), enabled: state.enabled(item), checked: state.checked(item) };
    });
    if (mac) return resolved;
    const keyed = withAccessKeys(resolved.map((i) => (i.kind === "command" || i.kind === "submenu" ? i.label : null)));
    return resolved.map((i, n) => (keyed[n] === null ? i : { ...i, label: keyed[n] }));
  };
  const menus = menuLayout(mac).map((menu) => ({ ...menu, items: resolve(menu.items) }));
  if (mac) return menus;
  const keyed = withAccessKeys(menus.map((m) => m.label));
  return menus.map((m, n) => ({ ...m, label: keyed[n] }));
}

/** A binding in CodeMirror's notation ("Mod-Shift-k") as muda writes an accelerator ("CmdOrCtrl+Shift+K"). */
export function acceleratorOf(binding) {
  if (!binding) return null;
  const parts = binding.split(/-(?!$)/);
  const key = parts.pop();
  const names = { Mod: "CmdOrCtrl", Ctrl: "Ctrl", Alt: "Alt", Shift: "Shift", Meta: "Super" };
  const keys = {
    "/": "Slash", ".": "Period", ",": "Comma", "-": "Minus", "=": "Equal", "[": "BracketLeft", "]": "BracketRight",
    "\\": "Backslash", Enter: "Enter", Space: "Space", Delete: "Delete",
  };
  return [...parts.map((m) => names[m]), keys[key] ?? (key.length === 1 ? key.toUpperCase() : key)].join("+");
}

/**
 * The native bar: built once from a model, then kept current, each change
 * of label, enabled, or checked sent only when it differs. `run(name)`
 * runs an item's command. Without Tauri's menu (a browser), it does nothing.
 */
export class MenuBar {
  #items = new Map();  // command name → {item, label, enabled, checked}
  #ready = null;

  constructor(run) {
    this.run = run;
  }

  /** Builds the bar from `model`, or brings the built bar up to it. */
  async show(model) {
    const api = window.__TAURI__?.menu;
    if (!api) return;
    if (!this.#ready) this.#ready = this.#build(api, model);
    await this.#ready;
    await this.#update(model);
  }

  async #build(api, model) {
    const { Menu, Submenu, MenuItem, CheckMenuItem, PredefinedMenuItem } = api;
    const make = async (entry) => {
      if (entry.kind === "separator") return PredefinedMenuItem.new({ item: "Separator" });
      if (entry.kind === "predefined") return PredefinedMenuItem.new({ item: entry.item });
      if (entry.kind === "submenu") return Submenu.new({ text: entry.label, items: await Promise.all(entry.items.map(make)) });
      const options = { id: entry.name, text: entry.label, enabled: entry.enabled, action: () => this.run(entry.name) };
      const accelerated = entry.accelerator ? { ...options, accelerator: entry.accelerator } : options;
      const create = (o) => (entry.checked === undefined ? MenuItem.new(o) : CheckMenuItem.new({ ...o, checked: entry.checked }));
      // An accelerator muda cannot read leaves the item without one, not the bar without the item.
      const item = await create(accelerated).catch(() => create(options));
      this.#items.set(entry.name, { item, label: entry.label, enabled: entry.enabled, checked: entry.checked,
        accelerator: entry.accelerator });
      return item;
    };
    const submenus = [];
    for (const menu of model) {
      const submenu = await Submenu.new({ text: menu.label, items: await Promise.all(menu.items.map(make)) });
      if (menu.window) await submenu.setAsWindowsMenuForNSApp?.();
      if (menu.help) await submenu.setAsHelpMenuForNSApp?.();
      submenus.push(submenu);
    }
    const bar = await Menu.new({ items: submenus });
    // macOS: the app's bar. Linux: the main window's, so the page's window has none.
    if (isMac) await bar.setAsAppMenu();
    else await bar.setAsWindowMenu(window.__TAURI__.window?.getCurrentWindow());
  }

  async #update(model) {
    const changes = [];
    const visit = (entries) => {
      for (const entry of entries) {
        if (entry.kind === "submenu") visit(entry.items);
        if (entry.kind !== "command") continue;
        const known = this.#items.get(entry.name);
        if (!known) continue;
        if (known.label !== entry.label) changes.push(known.item.setText(entry.label));
        if (known.enabled !== entry.enabled) changes.push(known.item.setEnabled(entry.enabled));
        if (entry.checked !== undefined && known.checked !== entry.checked) changes.push(known.item.setChecked(entry.checked));
        if (known.accelerator !== entry.accelerator) changes.push(known.item.setAccelerator(entry.accelerator).catch(() => {}));
        Object.assign(known, { label: entry.label, enabled: entry.enabled, checked: entry.checked,
          accelerator: entry.accelerator });
      }
    };
    for (const menu of model) visit(menu.items);
    await Promise.all(changes);
  }
}
