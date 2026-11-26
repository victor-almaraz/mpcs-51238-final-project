// Find in Files (patterns.md §23.5): a Search view in the bottom panel,
// beside Console and Problems, as GNOME Builder's and Sublime Text's sit at
// the window's foot. A field with Match Case and Regular Expression, then
// the matches by file, each line with its matches marked; Enter or a click
// opens one where it is.

import { h, rove, roving } from "./dom.js";

const resultLimit = 1000;

/** The matches of `query` in `text`: [{line, column, from, to, text}], at most `limit`. */
export function matchesIn(text, query, { caseSensitive = false, regex = false } = {}, limit = resultLimit) {
  if (!query) return [];
  let pattern;
  try {
    pattern = new RegExp(regex ? query : query.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), caseSensitive ? "g" : "gi");
  } catch {
    return null;  // not a regular expression
  }
  const found = [];
  const lines = text.split("\n");
  for (let n = 0; n < lines.length && found.length < limit; ++n) {
    pattern.lastIndex = 0;
    for (let m = pattern.exec(lines[n]); m && found.length < limit; m = pattern.exec(lines[n])) {
      if (m[0] === "") {
        pattern.lastIndex += 1;  // an empty match moves on
        continue;
      }
      found.push({ line: n + 1, column: m.index + 1, from: m.index, to: m.index + m[0].length, text: lines[n] });
    }
  }
  return found;
}

/**
 * The view: `texts()` answers the project's files as [{path, text}];
 * `goTo({path, line, column})` opens a match.
 */
export class SearchView {
  #options = { caseSensitive: false, regex: false };
  #runs = 0;

  constructor({ texts, goTo }) {
    this.texts = texts;
    this.goTo = goTo;
    this.field = h("input", { class: "field", type: "search", placeholder: "Find in files", "aria-label": "Find in Files",
      spellcheck: "false",
      onkeydown: (e) => {
        if (e.key === "Enter") {
          e.preventDefault();
          this.run();
        }
      } });
    const option = (key, label, text) => h("button", { class: "opt", title: label, "aria-label": label,
      "aria-pressed": "false",
      onclick: (e) => {
        this.#options[key] = !this.#options[key];
        e.currentTarget.setAttribute("aria-pressed", String(this.#options[key]));
        this.run();
      } }, text);
    this.count = h("span", { class: "count", role: "status" });
    // The matches as a listbox in groups by file, one Tab stop, the arrows between them (the APG's listbox).
    this.results = h("div", { class: "results", role: "listbox", "aria-label": "Matches" });
    roving(this.results, ".hit", (hit) => hit.click());
    this.element = h("div", { class: "search", hidden: true },
      h("div", { class: "query" }, h("div", { class: "input" }, this.field,
        option("caseSensitive", "Match Case", "Aa"), option("regex", "Regular Expression", ".*")), this.count),
      this.results);
  }

  /** Focuses the field, with `text` in it when given. */
  focus(text) {
    if (text) this.field.value = text;
    this.field.focus();
    this.field.select();
  }

  /** Searches the project for the field's text and lists the matches. */
  async run() {
    const query = this.field.value;
    const run = ++this.#runs;
    if (!query) {
      this.results.replaceChildren();
      this.count.textContent = "";
      return;
    }
    const files = await this.texts();
    if (run !== this.#runs) return;  // a newer search took its place
    const groups = [];
    let total = 0;
    for (const { path, text } of files) {
      const found = matchesIn(text, query, this.#options, resultLimit - total);
      if (found === null) {
        this.count.textContent = "Not a regular expression";
        this.results.replaceChildren();
        return;
      }
      if (found.length) groups.push({ path, found });
      total += found.length;
      if (total >= resultLimit) break;
    }
    this.count.textContent = total === 0 ? "No results"
      : `${total}${total >= resultLimit ? "+" : ""} result${total === 1 ? "" : "s"} in ${groups.length} file${groups.length === 1 ? "" : "s"}`;
    this.count.classList.toggle("none", total === 0);
    this.results.replaceChildren(...groups.map(({ path, found }) => h("div", { role: "group", "aria-label": path },
      h("div", { class: "file", role: "presentation" }, path, h("span", { class: "dim" }, ` ${found.length}`)),
      ...found.map((m) => h("div", { class: "hit", role: "option", "aria-selected": "false",
        "aria-label": `${path}, line ${m.line}: ${m.text.trim()}`,
        onclick: () => this.goTo({ path, line: m.line, column: m.column }) },
      h("span", { class: "where" }, `${m.line}`),
      h("span", { class: "text" }, m.text.slice(0, m.from).trimStart(), h("mark", {}, m.text.slice(m.from, m.to)),
        m.text.slice(m.to)))))));
    rove([...this.results.querySelectorAll(".hit")]);
  }
}
