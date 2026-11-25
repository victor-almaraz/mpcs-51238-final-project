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

/** A shortcut as the platform writes it: "Mod+S" is "Ctrl+S", or "⌘S" on macOS. */
export function keyLabel(keys) {
  if (!isMac) return keys.replace("Mod", "Ctrl");
  return keys.replace("Mod+", "⌘").replace("Shift+", "⇧").replace("Alt+", "⌥");
}
