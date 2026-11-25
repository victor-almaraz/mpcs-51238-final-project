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
import { h, keyLabel } from "./dom.js";
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

class FindPanel {
  constructor(view) {
    this.view = view;
    panels.set(view, this);
    const query = getSearchQuery(view.state);
    const toggle = (name, label, keys, on) => h("button", {
      class: "opt", title: `${label} (${keyLabel(keys)})`, "aria-label": label, "aria-pressed": String(on),
      onclick: (e) => {
        const button = e.currentTarget;
        button.setAttribute("aria-pressed", String(button.getAttribute("aria-pressed") !== "true"));
        this.commit();
      },
    }, name);
    const iconButton = (name, label, onclick) =>
      h("button", { class: "btn flat", title: label, "aria-label": label, onclick }, icon(name));

    this.field = h("input", {
      type: "text", class: "field", value: query.search, placeholder: "Find", "aria-label": "Find",
      "main-field": "true", spellcheck: "false",
      oninput: () => this.commit(),
      onkeydown: (e) => this.#key(e, () => (e.shiftKey ? findPrevious : findNext)(view)),
    });
    this.caseToggle = toggle("Aa", "Match case", "Alt+C", query.caseSensitive);
    this.wordToggle = toggle("ab", "Match whole word", "Alt+W", query.wholeWord);
    this.regexToggle = toggle(".*", "Use regular expression", "Alt+R", query.regexp);
    this.count = h("span", { class: "count", "aria-live": "polite" });
    this.replaceField = h("input", {
      type: "text", class: "field", value: query.replace, placeholder: "Replace", "aria-label": "Replace",
      spellcheck: "false",
      oninput: () => this.commit(),
      onkeydown: (e) => this.#key(e, () => replaceNext(view)),
    });
    this.replaceToggle = h("button", {
      class: "btn flat expand", title: "Toggle replace", "aria-label": "Toggle replace", "aria-expanded": "false",
      onclick: () => this.showReplace(this.replaceRow.hidden),
    }, icon("right"));
    this.replaceRow = h("div", { class: "row", hidden: true },
      h("div", { class: "input" }, this.replaceField),
      h("button", { class: "btn flat text", onclick: () => replaceNext(view) }, "Replace"),
      h("button", { class: "btn flat text", onclick: () => replaceAll(view) }, "Replace All"));

    this.dom = h("div", { class: "find", role: "search", "aria-label": "Find" },
      this.replaceToggle,
      h("div", { class: "rows" },
        h("div", { class: "row" },
          h("div", { class: "input" }, this.field, this.caseToggle, this.wordToggle, this.regexToggle),
          this.count,
          iconButton("up", "Previous match (Shift+Enter)", () => findPrevious(view)),
          iconButton("down", "Next match (Enter)", () => findNext(view)),
          iconButton("close", "Close (Escape)", () => closeSearchPanel(view))),
        this.replaceRow));
    this.dom.addEventListener("keydown", (e) => {
      const option = { c: this.caseToggle, w: this.wordToggle, r: this.regexToggle }[e.code?.slice(3).toLowerCase()];
      if (e.altKey && !e.ctrlKey && !e.metaKey && option) {
        e.preventDefault();
        option.click();
      }
    });
    this.top = true;
  }

  // Enter runs `action`, and Escape closes the panel and returns to the text.
  #key(e, action) {
    if (e.key === "Enter") {
      e.preventDefault();
      action();
    } else if (e.key === "Escape") {
      e.preventDefault();
      closeSearchPanel(this.view);
      this.view.focus();
    }
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
      this.count.textContent = "";
      this.count.classList.remove("none");
      return;
    }
    const { total, current } = matchCount(this.view.state);
    const more = total >= countLimit ? "+" : "";
    this.count.textContent = total === 0 ? "No results" : current ? `${current} of ${total}${more}` : `${total}${more} found`;
    this.count.classList.toggle("none", total === 0);
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
    ".cm-searchMatch": { backgroundColor: "#f5f05a40", outline: "none" },
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
