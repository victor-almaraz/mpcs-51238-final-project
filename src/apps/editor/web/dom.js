// Small helpers for building the editor's views with the DOM.

/**
 * An element: `h("button", {class: "btn", onclick}, "Save")`. Attributes
 * whose name starts with "on" are listeners; false and null ones are left
 * out; children are nodes or strings.
 */
export function h(tag, attributes = {}, ...children) {
  const element = document.createElement(tag);
  for (const [name, value] of Object.entries(attributes)) {
    if (value === false || value === null || value === undefined) continue;
    if (name.startsWith("on")) element.addEventListener(name.slice(2), value);
    else element.setAttribute(name, value === true ? "" : value);
  }
  for (const child of children.flat()) {
    if (child !== null && child !== undefined && child !== false) element.append(child);
  }
  return element;
}

/** True on macOS, where the command key takes the role of Ctrl. */
export const isMac = /Mac/.test(navigator.platform);

// Key names as GNOME's shortcuts dialog and macOS's menus write them.
const keyNames = {
  ArrowLeft: ["Left", "←"], ArrowRight: ["Right", "→"], ArrowUp: ["Up", "↑"], ArrowDown: ["Down", "↓"],
  PageUp: ["Page\u00a0Up", "⇞"], PageDown: ["Page\u00a0Down", "⇟"], Enter: ["Enter", "↩"], Delete: ["Delete", "⌦"],
  Backspace: ["Backspace", "⌫"], Escape: ["Esc", "⎋"],
};

/**
 * A shortcut as the platform writes it: "Mod+S" is "Ctrl+S", or "⌘S" on
 * macOS; "Mod+Shift+/" is "Ctrl+?", as the HIG writes Keyboard Shortcuts.
 */
export function keyLabel(keys) {
  const parts = keys.split(/\+(?!$)/);
  let key = parts.pop();
  const modifiers = new Set(parts);
  if (key === "/" && modifiers.has("Shift")) {
    key = "?";
    modifiers.delete("Shift");
  }
  key = keyNames[key]?.[isMac ? 1 : 0] ?? key;
  if (!isMac) return [...[...modifiers].map((m) => (m === "Mod" ? "Ctrl" : m)), key].join("+");
  const symbols = { Ctrl: "⌃", Alt: "⌥", Shift: "⇧", Mod: "⌘" };
  return ["Ctrl", "Alt", "Shift", "Mod"].filter((m) => modifiers.has(m)).map((m) => symbols[m]).join("") + key;
}

/** A key label as key caps: "Ctrl+Shift+P" is three, "C-x C-s" two, "⌘⇧P" and ":w" one. */
export function keyCaps(label) {
  if (!label) return [];
  return label.split(" ").flatMap((part) => (isMac ? [part] : part.split(/\+(?!$)/)));
}
