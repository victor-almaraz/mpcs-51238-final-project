// Help's references (patterns.md §19.8): docs/language.md and
// docs/library.md, drawn as prose in a tab of their own, as GNOME's Help
// and the IDEs' documentation views draw them, not as read-only Markdown.
// Each is the article, its Lisp examples highlighted as the code is, beside
// the contents of its sections; a link goes to its section, to the other
// reference, or to the browser.

import { marked } from "marked";
import { h, selectedText } from "./dom.js";
import { icon } from "./icons.js";
import { countText, findBar } from "./find.js";
import { StyleModule } from "style-mod";
import { carbonCode } from "./codeEditor.js";
import { lispParser, tokenize } from "./lisp.js";

// The code's own colors (codeEditor.js), mounted for a window that shows no code yet.
if (carbonCode.module) StyleModule.mount(document, carbonCode.module);

// The room left above a heading scrolled to: editor.css's --inset. Offsets are the scroller's, which is positioned.
const headingRoom = 12;

/** The references, by name: the file in docs/ and the tab's title. */
export const references = {
  language: { file: "language.md", title: "Language Reference" },
  library: { file: "library.md", title: "Standard Library" },
};

/** A heading's id, as GitHub's: lower case, punctuation dropped, spaces as hyphens. */
export function slug(text) {
  return text.trim().toLowerCase().replace(/[^\p{L}\p{N}\s_-]/gu, "").replace(/\s/g, "-");
}

/** Lisp source as spans of its token categories (lisp.js), in the code's highlighting classes. */
export function highlighted(code) {
  const parts = [];
  let at = 0;
  for (const token of tokenize(code)) {
    if (token.from > at) parts.push(code.slice(at, token.from));
    const style = carbonCode.style([lispParser.tokenTable[token.category]]);
    parts.push(style ? h("span", { class: style }, code.slice(token.from, token.to)) : code.slice(token.from, token.to));
    at = token.to;
  }
  if (at < code.length) parts.push(code.slice(at));
  return parts;
}

/**
 * Where a link goes: {section} in this reference, {reference, section} in
 * another, {url} outside, or null for a link to nothing the editor opens.
 */
export function linkTarget(href) {
  if (href.startsWith("#")) return { section: decodeURIComponent(href.slice(1)) };
  if (/^(https?|mailto):/i.test(href)) return { url: href };
  const [file, section = null] = href.split("#");
  const name = Object.keys(references).find((n) => file === references[n].file || file.endsWith(`/${references[n].file}`));
  return name ? { reference: name, section: section && decodeURIComponent(section) } : null;
}

/**
 * The view of a reference's `markdown`: its `element` for the editor's
 * area, its `outline` ([{id, text}], one a section), and `show(id)`, which
 * scrolls to a section. `follow(target)` takes a link to another reference
 * or outside (linkTarget); `copy(text)` takes an example's text.
 */
export class ReferenceView {
  constructor(markdown, { title, follow, copy }) {
    this.article = h("article", { class: "prose" });
    this.article.innerHTML = marked.parse(markdown, { gfm: true, async: false });
    // Every heading an anchor, numbered when its text repeats.
    const seen = new Map();
    for (const heading of this.article.querySelectorAll("h1, h2, h3, h4")) {
      const base = slug(heading.textContent);
      const n = seen.get(base) ?? 0;
      seen.set(base, n + 1);
      heading.id = n ? `${base}-${n}` : base;
    }
    // Examples: Lisp highlighted, each with Copy.
    for (const code of this.article.querySelectorAll("pre > code")) {
      const text = code.textContent.replace(/\n$/, "");
      if (code.classList.contains("language-lisp")) code.replaceChildren(...highlighted(text));
      code.parentElement.classList.add("example");
      code.parentElement.prepend(h("button", { class: "btn flat icon-only copy", title: "Copy", "aria-label": "Copy Example",
        onclick: () => copy(text) }, icon("copy")));
    }
    // A wide table scrolls across in a narrow window, not the article.
    for (const table of this.article.querySelectorAll("table")) {
      const wrap = h("div", { class: "table" });
      table.replaceWith(wrap);
      wrap.append(table);
    }
    this.article.addEventListener("click", (e) => {
      const link = e.target.closest("a[href]");
      if (!link) return;
      e.preventDefault();
      const target = linkTarget(link.getAttribute("href"));
      if (target?.section !== undefined && !target.reference) this.show(target.section);
      else if (target) follow(target);
    });

    this.outline = [...this.article.querySelectorAll("h2")].map((heading) => ({ id: heading.id, text: heading.textContent,
      heading }));
    this.contents = h("nav", { class: "contents", "aria-label": "Contents" },
      h("div", { class: "group" }, "Contents"),
      this.outline.map(({ id, text }) => h("a", { class: "row", href: `#${id}`, "data-id": id,
        onclick: (e) => {
          e.preventDefault();
          this.show(id);
        } }, text)));
    // The prose scrolls, one Tab stop, by the arrows and Page Up and Down as a page does.
    this.scroller = h("div", { class: "scroller", tabindex: "0", role: "document", "aria-label": title }, this.article);
    // The section at the top, once a frame however fast the prose scrolls.
    let frame = 0;
    this.scroller.addEventListener("scroll", () => {
      if (frame) return;
      frame = requestAnimationFrame(() => {
        frame = 0;
        this.#markCurrent();
      });
    }, { passive: true });
    this.element = h("div", { class: "reference" }, this.scroller, this.contents);
    this.#makeFind();
  }

  /** Scrolls to the section `id`, focusing the prose; false when there is none. */
  show(id) {
    const heading = id ? this.article.querySelector(`#${CSS.escape(id)}`) : null;
    if (!heading) return false;
    this.scroller.scrollTop = heading.offsetTop - headingRoom;
    this.#markCurrent();
    this.scroller.focus({ preventScroll: true });
    return true;
  }

  /** Focuses the prose. */
  focus() {
    this.#markCurrent();
    this.scroller.focus({ preventScroll: true });
  }

  // ---- Find (Ctrl+F on a reference): its matches marked in the prose, as the code's find bar marks them, one at a
  // time the current; Enter and Shift+Enter, or F3, go between them; Escape closes it.

  #hits = [];
  #current = null;  // the section the contents mark
  #at = -1;

  #makeFind() {
    let waiting = 0;
    const bar = findBar({
      label: "Find in Reference",
      oninput: () => {  // a moment after the typing stops: each search marks the whole prose again
        clearTimeout(waiting);
        waiting = setTimeout(() => this.#search(), 80);
      },
      step: (by) => this.findStep(by),
      close: () => this.closeFind(),
    });
    this.findField = bar.field;
    this.setCount = bar.setCount;
    this.findBar = bar.dom;
    this.findBar.hidden = true;
    this.element.append(this.findBar);
  }

  /** Opens Find, with the prose's selection in its field when there is one. */
  openFind() {
    const selected = selectedText().trim();
    if (selected && !selected.includes("\n") && this.article.contains(getSelection().anchorNode)) {
      this.findField.value = selected;
    }
    this.findBar.hidden = false;
    this.findField.focus();
    this.findField.select();
    this.#search();
  }

  /** Closes Find, its marks gone, the prose focused. */
  closeFind() {
    this.findBar.hidden = true;
    this.#unmark();
    this.setCount("");
    this.focus();
  }

  /** The next match (`by` 1) or the previous one (-1), round the prose; Find opens when it is closed. */
  findStep(by) {
    if (this.findBar.hidden) {
      this.openFind();
      return;
    }
    if (!this.#hits.length) return;
    this.#show((this.#at + by + this.#hits.length) % this.#hits.length);
  }

  #search() {
    this.#unmark();
    const query = this.findField.value.toLowerCase();
    if (!query) {
      this.setCount("");
      return;
    }
    // Each text node's matches, wrapped in a mark; a match across two nodes (code and prose) is not one.
    const walker = document.createTreeWalker(this.article, NodeFilter.SHOW_TEXT);
    const nodes = [];
    for (let node = walker.nextNode(); node; node = walker.nextNode()) nodes.push(node);
    for (const node of nodes) {
      const text = node.data.toLowerCase();
      let from = text.indexOf(query);
      let rest = node;
      let offset = 0;
      while (from >= 0) {
        const match = rest.splitText(from - offset);
        rest = match.splitText(query.length);
        const mark = h("mark", { class: "hit" });
        match.replaceWith(mark);
        mark.append(match);
        this.#hits.push(mark);
        offset = from + query.length;
        from = text.indexOf(query, offset);
      }
    }
    if (this.#hits.length === 0) {
      this.setCount(countText(0, 0), true);
      return;
    }
    // The first match at or below the top of the prose, as a browser's find begins where the page is.
    const top = this.scroller.scrollTop;
    const first = this.#hits.findIndex((m) => m.offsetTop >= top);
    this.#show(Math.max(first, 0));
  }

  #show(i) {
    this.#hits[this.#at]?.classList.remove("current");
    this.#at = i;
    const mark = this.#hits[i];
    mark.classList.add("current");
    mark.scrollIntoView({ block: "center" });
    this.setCount(countText(i + 1, this.#hits.length));
    this.#markCurrent();
  }

  #unmark() {
    const parents = new Set(this.#hits.map((mark) => mark.parentNode));
    for (const mark of this.#hits) mark.replaceWith(...mark.childNodes);
    for (const parent of parents) parent.normalize();  // the text split for the marks, whole again
    this.#hits = [];
    this.#at = -1;
  }

  // The contents mark the section at the top of the prose.
  #markCurrent() {
    if (!this.scroller.isConnected) return;  // laid out once it shows
    const top = this.scroller.scrollTop + headingRoom + 1;
    let current = this.outline[0]?.id;
    for (const { id, heading } of this.outline) {
      if (heading.offsetTop <= top) current = id;
    }
    if (current === this.#current) return;  // only a change of section touches the contents
    this.#current = current;
    for (const row of this.contents.querySelectorAll(".row")) {
      if (row.dataset.id === current) row.setAttribute("aria-current", "location");
      else row.removeAttribute("aria-current");
    }
  }
}
