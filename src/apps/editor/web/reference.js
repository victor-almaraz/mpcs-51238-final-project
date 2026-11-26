// Help's references (patterns.md §19.8): docs/language.md and
// docs/library.md, drawn as prose in a tab of their own, as GNOME's Help
// and the IDEs' documentation views draw them, not as read-only Markdown.
// Each is the article, its Lisp examples highlighted as the code is, beside
// the contents of its sections; a link goes to its section, to the other
// reference, or to the browser.

import { marked } from "marked";
import { h } from "./dom.js";
import { icon } from "./icons.js";
import { tokenize } from "./lisp.js";

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

/** Lisp source as spans of its token categories (lisp.js), which editor.css colors as the code's. */
export function highlighted(code) {
  const parts = [];
  let at = 0;
  for (const token of tokenize(code)) {
    if (token.from > at) parts.push(code.slice(at, token.from));
    parts.push(h("span", { class: `tok-${token.category}` }, code.slice(token.from, token.to)));
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

    this.outline = [...this.article.querySelectorAll("h2")].map((heading) => ({ id: heading.id, text: heading.textContent }));
    this.contents = h("nav", { class: "contents", "aria-label": "Contents" },
      h("div", { class: "group" }, "Contents"),
      this.outline.map(({ id, text }) => h("a", { class: "row", href: `#${id}`, "data-id": id,
        onclick: (e) => {
          e.preventDefault();
          this.show(id);
        } }, text)));
    // The prose scrolls, one Tab stop, by the arrows and Page Up and Down as a page does.
    this.scroller = h("div", { class: "scroller", tabindex: "0", role: "document", "aria-label": title }, this.article);
    this.scroller.addEventListener("scroll", () => this.#markCurrent(), { passive: true });
    this.element = h("div", { class: "reference" }, this.scroller, this.contents);
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

  // The contents mark the section at the top of the prose.
  #markCurrent() {
    if (!this.scroller.isConnected) return;  // laid out once it shows
    const top = this.scroller.scrollTop + headingRoom + 1;
    let current = this.outline[0]?.id;
    for (const { id } of this.outline) {
      if (this.article.querySelector(`#${CSS.escape(id)}`).offsetTop <= top) current = id;
    }
    for (const row of this.contents.querySelectorAll(".row")) {
      if (row.dataset.id === current) row.setAttribute("aria-current", "location");
      else row.removeAttribute("aria-current");
    }
  }
}
