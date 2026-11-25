// The pickers (design-guide.md §5.9, patterns.md §9.2): one widget for Go to
// File and Show All Commands, after VS Code's quick open. A field with a
// teal underline over rows of 24 px; the selected row on `tabActive`; key
// bindings as key caps. The window gives it a source, which turns the
// field's text into rows, and the picker does the rest: the keys, the
// pointer, and the roles of a combobox over a listbox.

import { h } from "./dom.js";
import { icon } from "./icons.js";

/**
 * A fuzzy match of `query` in `text`, case-insensitive: each character of
 * the query in order. Answers {score, hits}, the indices matched, or null.
 * Runs of consecutive characters and starts of words score higher.
 */
export function fuzzy(query, text) {
  const q = query.toLowerCase();
  const t = text.toLowerCase();
  const hits = [];
  let score = 0;
  let at = 0;
  for (const c of q) {
    const i = t.indexOf(c, at);
    if (i < 0) return null;
    if (hits.length && i === hits.at(-1) + 1) score += 3;
    if (i === 0 || "/-_. ".includes(t[i - 1])) score += 2;
    score -= (i - at) * 0.01;
    hits.push(i);
    at = i + 1;
  }
  return { score, hits };
}

/** `text` with the characters at `hits` marked. */
export function marked(text, hits = []) {
  if (hits.length === 0) return [text];
  const parts = [];
  let from = 0;
  for (const i of hits) {
    if (i > from) parts.push(text.slice(from, i));
    parts.push(h("span", { class: "hit" }, text[i]));
    from = i + 1;
  }
  parts.push(text.slice(from));
  return parts;
}

let nextId = 1;

export class Picker {
  /**
   * @param {HTMLElement} host where the picker is placed
   * @param {() => void} onClose called when it closes, whatever closed it
   */
  constructor(host, onClose) {
    this.host = host;
    this.onClose = onClose;
    this.element = null;
  }

  get isOpen() {
    return this.element !== null;
  }

  /**
   * Opens the picker. `source(query)` answers {rows, empty?}: each row
   * {label, detail?, keys?, icon?, hits?, group?, run}, where a group
   * heads the rows from the first that names it; and the text shown when
   * there are none.
   */
  open({ label, placeholder, value = "", source }) {
    this.close(false);
    this.source = source;
    this.selected = 0;
    const id = `picker-${nextId++}`;
    this.input = h("input", {
      type: "text", class: "field", placeholder, value, spellcheck: "false", autocomplete: "off",
      role: "combobox", "aria-expanded": "true", "aria-controls": `${id}-list`, "aria-autocomplete": "list",
      "aria-label": label,
      oninput: () => {
        this.selected = 0;
        this.render();
      },
      onkeydown: (e) => this.#key(e),
    });
    this.list = h("div", { class: "list", role: "listbox", id: `${id}-list`, "aria-label": label });
    this.element = h("div", { class: "picker", role: "dialog", "aria-label": label },
      h("div", { class: "input" }, this.input), this.list);
    this.listId = id;
    // A press outside the picker closes it.
    this.outside = (e) => {
      if (!this.element.contains(e.target)) this.close(true);
    };
    document.addEventListener("pointerdown", this.outside, true);
    this.host.append(this.element);
    this.render();
    this.input.focus();
    this.input.setSelectionRange(value.length, value.length);
  }

  /** Closes the picker; `cancelled` when nothing was chosen. */
  close(cancelled = true) {
    if (!this.element) return;
    this.element.remove();
    this.element = null;
    document.removeEventListener("pointerdown", this.outside, true);
    this.onClose(cancelled);
  }

  /** Puts `value` in the field, as if typed. */
  setValue(value) {
    this.input.value = value;
    this.selected = 0;
    this.render();
  }

  render() {
    const { rows, empty } = this.source(this.input.value);
    this.rows = rows;
    this.selected = Math.min(this.selected, Math.max(rows.length - 1, 0));
    const children = [];
    rows.forEach((row, i) => {
      if (row.group && row.group !== rows[i - 1]?.group) {
        children.push(h("div", { class: "group", role: "presentation" }, row.group));
      }
      children.push(h("div", {
        class: `row${i === this.selected ? " on" : ""}`, role: "option", id: `${this.listId}-${i}`,
        "aria-selected": String(i === this.selected),
        onpointermove: () => {
          if (this.selected !== i) this.#select(i);
        },
        onclick: () => this.#choose(i),
      },
      row.icon ? icon(row.icon) : null,
      h("span", { class: "label" }, marked(row.label, row.hits)),
      row.detail ? h("span", { class: "detail" }, row.detail) : null,
      row.keys?.length ? h("span", { class: "keys" }, row.keys.map((k) => h("kbd", {}, k))) : null));
    });
    if (rows.length === 0) children.push(h("div", { class: "none" }, empty ?? "No results"));
    this.list.replaceChildren(...children);
    this.#mark();
  }

  #select(i) {
    this.selected = i;
    for (const [j, row] of [...this.list.querySelectorAll(".row")].entries()) {
      row.classList.toggle("on", j === i);
      row.setAttribute("aria-selected", String(j === i));
    }
    this.#mark();
  }

  #mark() {
    const row = this.list.querySelectorAll(".row")[this.selected];
    if (row) {
      this.input.setAttribute("aria-activedescendant", row.id);
      row.scrollIntoView({ block: "nearest" });
    } else {
      this.input.removeAttribute("aria-activedescendant");
    }
  }

  // Closes the picker, then runs the row, which may open another.
  #choose(i) {
    const row = this.rows[i];
    if (!row) return;
    this.close(false);
    row.run();
  }

  #key(e) {
    const last = this.rows.length - 1;
    const page = 10;
    const to = {
      ArrowDown: this.selected + 1, ArrowUp: this.selected - 1, PageDown: this.selected + page,
      PageUp: this.selected - page,
    }[e.key];
    if (to !== undefined) {
      e.preventDefault();
      if (last >= 0) this.#select(e.key.startsWith("Arrow") ? (to + last + 1) % (last + 1) : Math.max(0, Math.min(to, last)));
      return;
    }
    if (e.key === "Enter") {
      e.preventDefault();
      this.#choose(this.selected);
      return;
    }
    if (e.key === "Escape") {
      e.preventDefault();
      e.stopPropagation();
      this.close(true);
      return;
    }
    if (e.key === "Tab") e.preventDefault();  // focus stays in the picker
  }
}
