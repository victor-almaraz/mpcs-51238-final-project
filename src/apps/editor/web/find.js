// Find and replace (design-guide.md §5.6, patterns.md §10.2): VS Code's
// overlay at the editor's top right, over CodeMirror's search. The replace
// toggle, the field with match case, whole word, and regular expression,
// the count, previous and next, and close; with replace open, a second row
// with the replacement, Replace, and Replace All. Every match is tinted and
// the current one filled (editor.css).

import {
  SearchQuery, closeSearchPanel, findNext, findPrevious, getSearchQuery, openSearchPanel, replaceAll, replaceNext,
  search, setSearchQuery,
} from "@codemirror/search";
import { EditorView } from "@codemirror/view";
import { h } from "./dom.js";
import { icon } from "./icons.js";

// Matches counted before the count reads "1000+".
const countLimit = 1000;

// The open panel of each view, for Replace, which opens the panel with its second row.
const panels = new WeakMap();

/** The matches of the panel's query in `state`, up to the limit, and which one the selection is. */
export function matchCount(state) {
  const query = getSearchQuery(state);
  if (!query.valid) return { total: 0, current: 0 };
  const { from, to } = state.selection.main;
  const cursor = query.getCursor(state);
  let total = 0;
  let current = 0;
  for (let next = cursor.next(); !next.done && total < countLimit; next = cursor.next()) {
    ++total;
    if (next.value.from === from && next.value.to === to) current = total;
  }
  return { total, current };
}

/** A count as the find bar reads it: "3 of 12", "12 found" without a current match, or "No results". */
export function countText(current, total, more = "") {
  return total === 0 ? "No results" : current ? `${current} of ${total}${more}` : `${total}${more} found`;
}

/**
 * The find bar, the code's and a reference's: the field with `toggles` after it, the count, Previous, Next, and
 * Close, with `before` (Replace's toggle) and `rows` (its row) when given. Enter and Shift+Enter in the field call
 * `step(1)` and `step(-1)`; Escape from any of its controls calls `close`. `setCount(text, none)` writes the count.
 */
export function findBar({ value = "", label = "Find", toggles = [], before = null, rows = [], oninput, step, close,
  mainField = false }) {
  const iconButton = (name, title, onclick) =>
    h("button", { class: "btn flat icon-only", title, "aria-label": title, onclick }, icon(name));
  const field = h("input", { type: "text", class: "field", value, placeholder: "Find", "aria-label": label,
    "main-field": mainField ? "true" : null, spellcheck: "false", oninput,
    onkeydown: (e) => {
      if (e.key !== "Enter") return;
      e.preventDefault();
      step(e.shiftKey ? -1 : 1);
    } });
  const count = h("span", { class: "count", "aria-live": "polite" });
  const dom = h("div", { class: "find", role: "search", "aria-label": "Find" }, before,
    h("div", { class: "rows" },
      h("div", { class: "row" },
        h("div", { class: "input" }, field, ...toggles),
        count,
        iconButton("up", "Previous Match (Shift+Enter)", () => step(-1)),
        iconButton("down", "Next Match (Enter)", () => step(1)),
        iconButton("close", "Close (Esc)", close)),
      ...rows));
  // Escape closes the bar from any of its controls, not only its fields (the APG).
  dom.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && !e.defaultPrevented) {
      e.preventDefault();
      close();
    }
  });
  const setCount = (text, none = false) => {
    count.textContent = text;
    count.classList.toggle("none", none);
  };
  return { dom, field, setCount };
}

class FindPanel {
  constructor(view) {
    this.view = view;
    panels.set(view, this);
    const query = getSearchQuery(view.state);
    const toggle = (name, label, on) => h("button", {
      class: "opt", title: label, "aria-label": label, "aria-pressed": String(on),
      onclick: (e) => {
        const button = e.currentTarget;
        button.setAttribute("aria-pressed", String(button.getAttribute("aria-pressed") !== "true"));
        this.commit();
      },
    }, name);
    // No Alt keys: Alt with a letter is an access key in GNOME (the HIG's Keyboard).
    this.caseToggle = toggle("Aa", "Match Case", query.caseSensitive);
    this.wordToggle = toggle("ab", "Match Whole Word", query.wholeWord);
    this.regexToggle = toggle(".*", "Regular Expression", query.regexp);
    this.replaceField = h("input", {
      type: "text", class: "field", value: query.replace, placeholder: "Replace", "aria-label": "Replace",
      spellcheck: "false",
      oninput: () => this.commit(),
      onkeydown: (e) => {
        if (e.key !== "Enter") return;
        e.preventDefault();
        replaceNext(view);
      },
    });
    this.replaceToggle = h("button", {
      class: "btn flat icon-only expand", title: "Show Replace", "aria-label": "Show Replace", "aria-expanded": "false",
      onclick: () => this.showReplace(this.replaceRow.hidden),
    }, icon("right"));
    this.replaceRow = h("div", { class: "row", hidden: true },
      h("div", { class: "input" }, this.replaceField),
      h("button", { class: "btn flat text", onclick: () => replaceNext(view) }, "Replace"),
      h("button", { class: "btn flat text", onclick: () => replaceAll(view) }, "Replace All"));

    const bar = findBar({
      value: query.search, mainField: true, oninput: () => this.commit(),
      toggles: [this.caseToggle, this.wordToggle, this.regexToggle], before: this.replaceToggle, rows: [this.replaceRow],
      step: (by) => (by < 0 ? findPrevious : findNext)(view),
      close: () => {
        closeSearchPanel(view);
        view.focus();
      },
    });
    this.dom = bar.dom;
    this.field = bar.field;
    this.setCount = bar.setCount;
    this.top = true;
  }

  showReplace(on) {
    this.replaceRow.hidden = !on;
    this.replaceToggle.setAttribute("aria-expanded", String(on));
    this.replaceToggle.replaceChildren(icon(on ? "down" : "right"));
    (on ? this.replaceField : this.field).focus();
  }

  commit() {
    const pressed = (button) => button.getAttribute("aria-pressed") === "true";
    const query = new SearchQuery({
      search: this.field.value, replace: this.replaceField.value, caseSensitive: pressed(this.caseToggle),
      wholeWord: pressed(this.wordToggle), regexp: pressed(this.regexToggle),
    });
    if (!query.eq(getSearchQuery(this.view.state))) this.view.dispatch({ effects: setSearchQuery.of(query) });
  }

  mount() {
    this.field.select();
    this.renderCount();
  }

  update(update) {
    for (const tr of update.transactions) {
      for (const effect of tr.effects) {
        if (effect.is(setSearchQuery) && !effect.value.eq(this.query)) this.#fill(effect.value);
      }
    }
    if (update.docChanged || update.selectionSet || update.transactions.some((tr) => tr.effects.length)) {
      this.renderCount();
    }
  }

  #fill(query) {
    this.query = query;
    this.field.value = query.search;
    this.replaceField.value = query.replace;
    this.caseToggle.setAttribute("aria-pressed", String(query.caseSensitive));
    this.wordToggle.setAttribute("aria-pressed", String(query.wholeWord));
    this.regexToggle.setAttribute("aria-pressed", String(query.regexp));
  }

  renderCount() {
    const query = getSearchQuery(this.view.state);
    this.query = query;
    if (!query.search) {
      this.setCount("");
      return;
    }
    const { total, current } = matchCount(this.view.state);
    this.setCount(countText(current, total, total >= countLimit ? "+" : ""), total === 0);
  }

  destroy() {
    panels.delete(this.view);
  }
}

/** CodeMirror's search with the find overlay as its panel, at the editor's top. */
export const find = [
  search({ top: true, createPanel: (view) => new FindPanel(view) }),
  EditorView.theme({
    ".cm-panels.cm-panels-top": { border: "none", background: "transparent", height: "0", overflow: "visible" },
    // The tint, and an edge at 3:1 against the editor, so a match is seen without the color alone.
    ".cm-searchMatch": { backgroundColor: "#f5f05a40", outline: "none", boxShadow: "inset 0 -1px 0 #b8b44a" },
    ".cm-searchMatch.cm-searchMatch-selected, .cm-searchMatch-selected": {
      backgroundColor: "var(--accentText)", color: "var(--window)" },
    ".cm-searchMatch-selected span": { color: "var(--window) !important" },
  }),
];

/** Opens find in `view`, with its replace row when `replace`; the field takes the selection. */
export function openFind(view, { replace = false } = {}) {
  openSearchPanel(view);
  panels.get(view)?.showReplace(replace);
}

export { findNext, findPrevious };
