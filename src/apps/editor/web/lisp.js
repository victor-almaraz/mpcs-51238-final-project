// The Lisp dialect's highlighting (tauri-editor.md §5.4): a CodeMirror
// stream tokenizer that reads each line as the interpreter's tolerant lexer
// reads the file (src/interpreter/lexer.cpp) and gives each token the C++
// highlighter's category (src/workspace/highlighter.cpp, tui.md §6.2).
//
// Three categories refine a symbol, as the design guide colors code
// (brand/gui/design-guide.md §2.6, §2.7): the name a definition defines, a
// quoted symbol, and a call, a plain symbol at the head of a form. Each
// draws as a symbol under the C++ highlighter; `baseCategory` maps it back.
//
// Read a line at a time, the tokenizer cannot see past the line it is on.
// It differs from the C++ highlighter in two cases only:
// - An unterminated string or block comment draws as a string or a comment
//   to the end of the file, where the C++ highlighter marks it an error.
// - A string over several lines with an invalid escape is an error from the
//   line of the escape on; the lines before it draw as a string.

import { StreamLanguage } from "@codemirror/language";
import { tags } from "@lezer/highlight";

// The lexer's character classes (lexer.cpp, grammar.md §1).
const delimiters = new Set([" ", "\t", "\r", "\n", "(", ")", "[", "]", "'", '"', ";"]);
const isWhitespace = (c) => c === " " || c === "\t" || c === "\r";
const isDigit = (c) => c >= "0" && c <= "9";
const isLetter = (c) => (c >= "a" && c <= "z") || (c >= "A" && c <= "Z");
const symbolStarts = new Set("+-*/<>=!?_%");
const isSymbolStart = (c) => symbolStarts.has(c) || isLetter(c);
const isSymbolChar = (c) => isSymbolStart(c) || isDigit(c) || c === ".";
const allSymbolChars = (text) => [...text].every(isSymbolChar);

const unitSuffixes = new Set(["s", "ms", "us", "smp", "beat", "bar", "hz", "khz", "bpm", "db", "st", "ct"]);
const lambdaListMarkers = new Set(["&optional", "&rest", "&key"]);

// classifySymbol's tables (highlighter.cpp).
const specialForms = new Set([
  "quote", "if", "cond", "let", "fn", "do", "set!", "define",
  "module", "import", "export", "defpiece", "project", "mapper", "play",
]);
const objectForms = new Set([
  "defclass", "defgeneric", "defmethod", "make", "slot-value", "call-next-method", "next-method-p",
]);

/** A symbol's category by name: "specialForm", "objectForm", or "symbol". */
export function classifySymbol(name) {
  if (specialForms.has(name)) return "specialForm";
  if (objectForms.has(name)) return "objectForm";
  return "symbol";
}

// The length of the numeric prefix of `word`, and whether it is a float;
// 0 when the prefix is not a well-formed number (lexer.cpp, scanNumber).
function scanNumber(word) {
  let i = 0;
  if (word[i] === "-") ++i;
  const intStart = i;
  while (i < word.length && isDigit(word[i])) ++i;
  if (i === intStart) return [0, false];
  let isFloat = false;
  if (word[i] === ".") {
    const fracStart = ++i;
    while (i < word.length && isDigit(word[i])) ++i;
    if (i === fracStart) return [0, false];
    isFloat = true;
  }
  if (word[i] === "e" || word[i] === "E") {
    let j = i + 1;
    if (word[j] === "+" || word[j] === "-") ++j;
    const expStart = j;
    while (j < word.length && isDigit(word[j])) ++j;
    if (j > expStart) {
      i = j;
      isFloat = true;
    }
  }
  return [i, isFloat];
}

// The category of a word: what lexer.cpp's word() makes of it, as
// highlighter.cpp's categoryOf() draws it.
function wordCategory(text) {
  const first = text[0];
  if (isDigit(first) || (first === "-" && text.length > 1 && isDigit(text[1]))) {
    const [numeric] = scanNumber(text);
    if (numeric === 0) return "error";
    if (numeric === text.length) return "number";
    return unitSuffixes.has(text.slice(numeric)) ? "unitLiteral" : "error";
  }
  if (first === ":") return text.length > 1 && allSymbolChars(text.slice(1)) ? "keyword" : "error";
  if (first === "#") return text === "#t" || text === "#f" ? "keyword" : "error";
  if (text === "nil") return "keyword";
  if (lambdaListMarkers.has(text)) return "symbol";
  if (!isSymbolStart(first) || !allSymbolChars(text)) return "error";
  return classifySymbol(text);
}

// The forms that define a name: the symbol after one is its definition name.
const definers = new Set(["define", "defclass", "defmethod", "defgeneric", "defpiece"]);

// The lists of a form that are not calls (design-guide.md §2.7), by the
// form's operator: the list at element `at`, or the first list at element
// `from` or later. A "data" list holds no calls at any depth (parameters,
// superclasses); a "bindings" list holds lists whose heads are names
// (bindings, slot specs) and whose other elements may be calls.
const listArguments = {
  fn: { at: 1, kind: "data" },           // (fn (x) ...)
  define: { at: 1, kind: "data" },       // (define (name x) ...)
  defpiece: { at: 2, kind: "data" },     // (defpiece name () ...)
  let: { at: 1, kind: "bindings" },      // (let ((a 1)) ...)
  defclass: [{ at: 2, kind: "data" }, { at: 3, kind: "bindings" }],
  defgeneric: { at: 2, kind: "data" },   // (defgeneric name (a b))
  defmethod: { from: 2, kind: "data" },  // (defmethod name [:before] ((a class)) ...)
  quote: { from: 1, kind: "data" },
};

// The kind of a list opened by `bracket` inside `parent`, the innermost
// open list (or undefined at the top level), right after a quote or not.
function listKind(parent, bracket, quoted) {
  if (quoted || parent?.kind === "data") return "data";
  if (bracket === "[") return "list";  // a vector: its head is not a call
  if (!parent) return "call";
  if (parent.kind === "bindings") return "list";
  if (parent.kind !== "call") return "call";
  for (const rule of [listArguments[parent.op] ?? []].flat()) {
    if (rule.at === parent.count || (rule.from !== undefined && parent.lists === 0 && parent.count >= rule.from)) {
      return rule.kind;
    }
  }
  return "call";
}

/**
 * The tokenizer's state between lines:
 * - `open`: the string ({kind: "string", bad}) or block comment
 *   ({kind: "block", depth}) the line starts inside, or null.
 * - `datum`: the datum a `#;` comments out, or null; its `depth` is 0
 *   until its first token and then counts its open lists (highlighter.cpp,
 *   skipDatum).
 * - `stack`: the open lists, innermost last, each {kind, op, count, lists}:
 *   its kind (listKind), its first element, how many elements and how many
 *   lists it has so far.
 * - `naming`: the last token was a definer, so the next symbol is a name.
 * - `quoted`: the last token was a quote.
 */
function startState() {
  return { open: null, datum: null, stack: [], naming: false, quoted: false };
}

function copyState(s) {
  return {
    open: s.open && { ...s.open },
    datum: s.datum && { ...s.datum },
    stack: s.stack.map((f) => ({ ...f })),
    naming: s.naming,
    quoted: s.quoted,
  };
}

// The category a token of `base` draws in, where the state stands.
function categoryIn(state, base) {
  if (state.datum) return base === "error" ? "error" : "comment";
  if (base !== "symbol") return base;
  if (state.naming) return "definitionName";
  if (state.quoted) return "quotedSymbol";
  const top = state.stack.at(-1);
  return top?.kind === "call" && top.count === 0 ? "call" : "symbol";
}

// Moves the datum comment past a token of `kind`; true when the datum ends.
function datumEnds(datum, kind) {
  if (datum.depth === 0) {
    if (kind === "trivia" || kind === "quote" || kind === "datum") return false;
    if (kind !== "open") return true;  // an atom, a closing bracket, or an error
  }
  if (kind === "open") ++datum.depth;
  else if (kind === "close") --datum.depth;
  return datum.depth === 0;
}

// Moves the state past a token of `kind` ("open", "close", "quote",
// "datum", "trivia", or "atom"), with its text.
function advance(state, kind, text) {
  if (state.datum) {
    if (datumEnds(state.datum, kind)) state.datum = null;
    return;
  }
  const top = state.stack.at(-1);
  switch (kind) {
    case "datum":
      state.datum = { depth: 0 };
      return;
    case "trivia":
      return;
    case "quote":
      state.quoted = true;
      return;
    case "open":
      state.stack.push({ kind: listKind(top, text, state.quoted), op: null, count: 0, lists: 0 });
      if (top) {
        ++top.count;
        ++top.lists;
      }
      break;
    case "close":
      state.stack.pop();
      break;
    default:  // an atom
      if (top) {
        if (top.count === 0) top.op = text;
        ++top.count;
      }
      state.naming = definers.has(text);
  }
  state.quoted = false;
}

// A whole token: its category, then the state past it.
function finish(state, kind, base, text) {
  const category = categoryIn(state, base);
  advance(state, kind, text);
  return category;
}

// The rest of a string on this line (lexer.cpp, stringToken).
function stringPart(stream, state) {
  const open = state.open;
  while (!stream.eol()) {
    const c = stream.next();
    if (c === '"') {
      state.open = null;
      return finish(state, "atom", open.bad ? "error" : "string", "");
    }
    if (c === "\\") {
      // An escape of the line break is invalid; the C++ lexer reads past it.
      const e = stream.next();
      if (e === undefined || !"ntr\"\\".includes(e)) open.bad = true;
    }
  }
  return categoryIn(state, open.bad ? "error" : "string");
}

// The rest of a block comment on this line (lexer.cpp, blockComment).
function blockPart(stream, state) {
  const open = state.open;
  while (!stream.eol()) {
    if (stream.match("#|")) {
      ++open.depth;
    } else if (stream.match("|#")) {
      if (--open.depth === 0) {
        state.open = null;
        return finish(state, "trivia", "comment");
      }
    } else {
      stream.next();
    }
  }
  return categoryIn(state, "comment");
}

function token(stream, state) {
  if (state.open?.kind === "string") return stringPart(stream, state);
  if (state.open?.kind === "block") return blockPart(stream, state);
  if (stream.eatWhile(isWhitespace)) return null;
  const c = stream.next();
  switch (c) {
    case "(":
    case "[":
      return finish(state, "open", "delimiter", c);
    case ")":
    case "]":
      return finish(state, "close", "delimiter", c);
    case "'":
      return finish(state, "quote", "delimiter", c);
    case '"':
      state.open = { kind: "string", bad: false };
      return stringPart(stream, state);
    case ";":
      stream.skipToEnd();
      return finish(state, "trivia", "comment");
    case "#":
      if (stream.eat("|")) {
        state.open = { kind: "block", depth: 1 };
        return blockPart(stream, state);
      }
      if (stream.eat(";")) return finish(state, "datum", "comment");
  }
  stream.eatWhile((ch) => !delimiters.has(ch));
  const text = stream.current();
  return finish(state, "atom", wordCategory(text), text);
}

/** The categories that refine a symbol, and the category each draws as in the C++ highlighter. */
export function baseCategory(category) {
  return category === "call" || category === "definitionName" || category === "quotedSymbol" ? "symbol" : category;
}

// Each category's highlighting tag, so a theme colors it by its IDE role
// (patterns.md §15.5).
const tokenTable = {
  comment: tags.comment,
  string: tags.string,
  number: tags.number,
  unitLiteral: tags.special(tags.number),
  keyword: tags.atom,
  specialForm: tags.keyword,
  objectForm: tags.definitionKeyword,
  symbol: tags.variableName,
  call: tags.function(tags.variableName),
  definitionName: tags.definition(tags.variableName),
  quotedSymbol: tags.special(tags.atom),
  delimiter: tags.punctuation,
  error: tags.invalid,
};

/** The stream parser, for StreamLanguage and for the tests, which run it a line at a time. */
export const lispParser = {
  name: "espresso",
  startState,
  copyState,
  token,
  tokenTable,
  languageData: { commentTokens: { line: ";", block: { open: "#|", close: "|#" } } },
};

/** The dialect as a CodeMirror language. */
export const lisp = StreamLanguage.define(lispParser);
