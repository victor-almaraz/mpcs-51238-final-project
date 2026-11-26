// Completion from the code's structure (the user's request, 2026-11-25).
// The tokenizer's tree of lists (lisp.js, listTree) and its table of forms
// (lisp.js, forms) give:
// - the names bound around the cursor: a `let`'s bindings, the parameters
//   of a `fn`, a `define`, a `defpiece`, or a `defmethod`;
// - the names the open files and the standard library define;
// - the language's forms, its builtins, and its primitives (the sidecar's
//   `language`, tauri-editor.md §4.5);
// - after a `:`, the keywords of the call around the cursor: a primitive's
//   parameters and ports, the `:initarg`s of the class a `make` names, or a
//   function's `&key` parameters, less those the call already gives; and
//   after a choice parameter's keyword, its choices.
// Strings and comments complete nothing.

import { autocompletion } from "@codemirror/autocomplete";
import {
  baseCategory, formNames, forms, isLambdaListMarker, listRule, listTree, tokenize, wordChar,
} from "./lisp.js";

const wordBefore = new RegExp(`${wordChar}+$`);
const anyWord = new RegExp(`^${wordChar}*$`);
const keywordWord = new RegExp(`^:${wordChar}*$`);

const isList = (node) => node?.children !== undefined;
const isName = (node) => node && !isList(node) && ["symbol", "specialForm", "objectForm"].includes(baseCategory(node.category));
const head = (list) => (isList(list) && isName(list.children[0]) ? list.children[0].text : undefined);
const typeOf = (kind) => (kind === "class" || kind === "variable" ? kind : "function");

// The list arguments of `list` that bind or define names, by its form's rules: [{rule, node}].
function argumentsOf(list) {
  const found = [];
  let lists = 0;
  list.children.forEach((node, i) => {
    if (!isList(node)) return;
    const rule = listRule(head(list), i, lists++);
    if (rule?.role) found.push({ rule, node });
  });
  return found;
}

// A lambda list's names and its &key names; a `(name class)` element names
// its first symbol, and a named list's first element is the definition's name.
function parameters(node, named) {
  const names = [];
  const keys = [];
  let keyed = false;
  for (const element of node.children.slice(named ? 1 : 0)) {
    const name = isList(element) ? element.children[0] : element;
    if (!isName(name)) continue;
    if (isLambdaListMarker(name.text)) {
      keyed = name.text === "&key";
      continue;
    }
    names.push(name.text);
    if (keyed) keys.push(name.text);
  }
  return { names, keys };
}

// The first names of the lists in `node`: a let's bindings, a class's slots.
const firstNames = (node) => node.children.filter(isList).map((l) => l.children[0]).filter(isName).map((n) => n.text);

// A class's `:initarg`s, from its slots.
function initargs(slots) {
  return slots.children.filter(isList).flatMap((slot) => {
    const at = slot.children.findIndex((n) => n.text === ":initarg");
    return at >= 0 && slot.children[at + 1] ? [slot.children[at + 1].text.replace(/^:/, "")] : [];
  });
}

// What `tree` defines at its top level.
function definitionsOf(tree) {
  const found = [];
  for (const form of tree.children) {
    const spec = isList(form) && forms[head(form)];
    if (!spec || spec.defines === undefined) continue;
    const target = form.children[spec.defines];
    const args = argumentsOf(form);
    const params = args.find((a) => a.rule.role === "params");
    const slots = args.find((a) => a.rule.role === "slots");
    if (isList(target)) {  // (define (name params...) ...)
      if (head(target)) found.push({ name: head(target), kind: "function", keys: parameters(target, true).keys });
    } else if (isName(target)) {
      const value = form.children[spec.defines + 1];
      const fn = spec.kind === "variable" && head(value) === "fn";  // (define name (fn (params...) ...))
      found.push({
        name: target.text,
        kind: fn ? "function" : spec.kind,
        keys: fn ? parameters(value.children[1] ?? { children: [] }, false).keys
          : slots ? initargs(slots.node) : params ? parameters(params.node, false).keys : [],
      });
    }
  }
  return found;
}

/** What `text` defines at its top level: [{name, kind, keys}], with each class's `:initarg`s as its keys. */
export function definitions(text) {
  return definitionsOf(listTree(text));
}

// The lists from the root to the innermost that holds `pos`.
function pathTo(tree, pos) {
  const path = [tree];
  for (let node = tree; ;) {
    // Inside a list is after its bracket and before its closing one.
    const inner = node.children.find((c) => isList(c) && c.from < pos && (c.closed ? pos < c.to : pos <= c.to));
    if (!inner) return path;
    path.push(inner);
    node = inner;
  }
}

// The names bound around `pos` on `path`.
function scopeNames(path) {
  return path.flatMap((list) => argumentsOf(list).flatMap(({ rule, node }) => {
    if (rule.role === "params") return parameters(node, rule.named).names;
    if (rule.role === "bindings") return firstNames(node);
    return [];
  }));
}

// The suggestions that do not depend on the text: the forms, the builtins, and the primitives, once per language.
const fixedOptions = new WeakMap();
const noLanguage = {};
function fixed(language) {
  const key = language ?? noLanguage;
  let options = fixedOptions.get(key);
  if (!options) {
    options = [
      ...formNames().map((name) => ({ label: name, type: "keyword", detail: "form", boost: 0 })),
      ...(language?.builtins ?? []).map((name) => ({ label: name, type: "function", detail: "builtin", boost: 0 })),
      ...(language?.primitives ?? []).map((p) => ({ label: p.name, type: "function", detail: "primitive", boost: 0 })),
    ];
    fixedOptions.set(key, options);
  }
  return options;
}

// The keywords of `call`'s head at `from`, less those it gives; or a choice parameter's choices.
function keywordOptions(call, from, language, all) {
  const name = head(call);
  const given = call.children.filter((n) => !isList(n) && n.from < from);
  const used = new Set(call.children.filter((n) => !isList(n) && n.text.startsWith(":") && n.from !== from).map((n) => n.text));
  const primitive = language?.primitives.find((p) => p.name === name);
  const previous = given.at(-1);
  const choices = primitive?.params.find((p) => `:${p.name}` === previous?.text)?.choices;
  if (choices) return choices.map((c) => ({ label: `:${c}`, type: "enum", detail: previous.text.slice(1) }));
  const options = [];
  const add = (label, detail) => {
    if (!used.has(label)) options.push({ label, type: "property", detail });
  };
  if (primitive) {
    for (const p of primitive.params) add(`:${p.name}`, p.default === undefined ? p.dimension : `${p.dimension} · ${p.default}`);
    for (const p of primitive.inputs) add(`:${p.name}`, "input");
    for (const p of primitive.outputs) add(`:${p.name}`, "output");
  } else if (forms[name]?.keysOf !== undefined) {
    const target = call.children[forms[name].keysOf];
    const cls = isName(target) && all().find((d) => d.kind === "class" && d.name === target.text);
    for (const k of cls?.keys ?? []) add(`:${k}`, "initarg");
  } else {
    for (const k of all().find((d) => d.name === name)?.keys ?? []) add(`:${k}`, "key");
  }
  return options;
}

/**
 * The completions at `pos` in `text`: {from, options, validFor} or null.
 * `language` is the sidecar's (or null before it answers); `others()` the
 * definitions of the other open files and of the library, [{name, kind, keys, path}].
 */
export function completionsAt(text, pos, language, others = () => [], explicit = false) {
  const lineStart = text.lastIndexOf("\n", pos - 1) + 1;
  const typed = wordBefore.exec(text.slice(lineStart, pos))?.[0] ?? "";
  if (!typed && !explicit) return null;
  if (/^-?\d/.test(typed)) return null;  // a number
  const from = pos - typed.length;
  // Nothing in a comment, to its end, or in a string, closed or not.
  const at = tokenize(text).find((t) => t.from < pos && pos <= t.to);
  const quoted = at && text[at.from] === '"' && (at.category === "string" || at.category === "error");
  if (at?.category === "comment" || (quoted && (pos < at.to || at.unterminated))) return null;

  const tree = listTree(text);
  const path = pathTo(tree, pos);
  const own = definitionsOf(tree);
  const all = () => [...own, ...others()];

  if (typed.startsWith(":")) {
    const options = keywordOptions(path.at(-1), from, language, all);
    return options.length ? { from, options, validFor: keywordWord } : null;
  }

  // Names: the nearest first.
  const options = new Map(fixed(language).map((o) => [o.label, o]));
  const add = (label, type, detail, boost) => {
    if (!(options.get(label)?.boost >= boost)) options.set(label, { label, type, detail, boost });
  };
  for (const d of others()) add(d.name, typeOf(d.kind), d.path ?? d.kind, 1);
  for (const d of own) add(d.name, typeOf(d.kind), d.kind, 2);
  for (const name of scopeNames(path)) add(name, "variable", "local", 3);
  options.delete(typed);  // the word as typed is not a suggestion of itself
  return { from, options: [...options.values()], validFor: anyWord };
}

/**
 * The completion extension: `language()` answers the sidecar's language,
 * and `others(state)` the definitions of the other open files and the library.
 */
export function completion(language, others) {
  return autocompletion({
    icons: false,
    override: [(context) => completionsAt(context.state.doc.toString(), context.pos, language(),
      () => others(context.state), context.explicit)],
  });
}
