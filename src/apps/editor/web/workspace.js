// The editor's workspace: the open project, its files, and the documents
// open in tabs, read and written through the sidecar (tauri-editor.md §4.5).
// It holds no DOM; the views render it and call its methods.

import { EditorState, StateEffect } from "@codemirror/state";

/** The name of a project from its root: the last component of the path. */
export function projectName(root) {
  return root.replace(/\/+$/, "").split("/").pop() || root;
}

/** The last component of a project path, such as "tone.lisp". */
export function baseName(path) {
  return path.split("/").pop();
}

/**
 * The file the editor plays (`interpreter.md` §11): main.lisp when the
 * project has one.
 */
export const entryFile = "main.lisp";

export class Workspace extends EventTarget {
  /** The project's root, absolute, or null with no project open. */
  root = null;
  /** The project's files, relative to the root, sorted. */
  files = [];
  /**
   * The open documents, in tab order. Each has a stable `id`, its `path`,
   * `state` (a CodeMirror EditorState), and `saved` (the text on disk, as
   * a CodeMirror Text), so unsaved edits are `!state.doc.eq(saved)`.
   */
  documents = [];
  /** The id of the current document, or null. */
  currentId = null;

  #sidecar;
  #makeState;
  #nextId = 1;

  /**
   * @param {{request: Function}} sidecar
   * @param {(text: string, path: string) => import("@codemirror/state").EditorState} makeState
   */
  constructor(sidecar, makeState) {
    super();
    this.#sidecar = sidecar;
    this.#makeState = makeState;
  }

  get name() {
    return this.root === null ? "" : projectName(this.root);
  }

  /**
   * The file Play plays, as headless mode chooses it (headless.rs, entry_file): main.lisp, else NAME.lisp in a
   * project NAME, else the project's only .lisp file other than project.lisp; null when none of these is.
   */
  get entry() {
    const files = this.files ?? [];
    if (files.includes(entryFile)) return entryFile;
    if (files.includes(`${this.name}.lisp`)) return `${this.name}.lisp`;
    const pieces = files.filter((p) => p.endsWith(".lisp") && !p.includes("/") && p !== "project.lisp");
    return pieces.length === 1 ? pieces[0] : null;
  }

  get current() {
    return this.document(this.currentId);
  }

  /** The document with `id`, or undefined once it is closed. */
  document(id) {
    return this.documents.find((d) => d.id === id);
  }

  /** The open document of `path`, or undefined. */
  documentOf(path) {
    return this.documents.find((d) => d.path === path);
  }

  isDirty(doc) {
    return !doc.state.doc.eq(doc.saved);
  }

  get dirtyDocuments() {
    return this.documents.filter((d) => this.isDirty(d));
  }

  /**
   * Opens the project at `root` in place of the one open, whose documents
   * close: the caller settles their unsaved changes first.
   */
  async openProject(root) {
    const opened = await this.#sidecar.request("project.open", { root });
    this.root = opened.root;
    this.files = opened.files;
    this.documents = [];
    this.currentId = null;
    this.#changed("project");
    return opened;
  }

  /** Reads the project's file list again. */
  async refreshFiles() {
    if (this.root === null) return;
    const opened = await this.#sidecar.request("project.open", { root: this.root });
    this.files = opened.files;
    this.#changed("files");
  }

  /** Opens `path` in a new tab, or shows its tab when it is open. */
  async openFile(path) {
    const open = this.documentOf(path);
    if (open) {
      this.select(open.id);
      return open;
    }
    const { text, mtime } = await this.#sidecar.request("file.read", { path });
    const again = this.documentOf(path);  // opened while the read was in flight
    if (again) {
      this.select(again.id);
      return again;
    }
    const state = this.#makeState(text, path);
    // `mtime`: the file's token when read, so a save never replaces a change made on disk since (patterns.md §13).
    const doc = { id: this.#nextId++, path, state, saved: state.doc, mtime };
    this.documents.push(doc);
    this.currentId = doc.id;
    this.#changed("documents");
    return doc;
  }

  /**
   * Opens `text` read-only in a tab titled `title`, or shows its tab: a
   * reference (Help), which belongs to no project and is never saved.
   */
  openText(title, text) {
    const open = this.documents.find((d) => d.reference && d.path === title);
    if (open) {
      this.select(open.id);
      return open;
    }
    const state = this.#makeState(text, title);
    const readOnly = state.update({ effects: StateEffect.appendConfig.of(EditorState.readOnly.of(true)) }).state;
    const doc = { id: this.#nextId++, path: title, state: readOnly, saved: readOnly.doc, reference: true };
    this.documents.push(doc);
    this.currentId = doc.id;
    this.#changed("documents");
    return doc;
  }

  /** Makes document `id` the file at `path`, saved as `written`, after Save As wrote it there. */
  savedAs(id, path, written) {
    const doc = this.document(id);
    if (!doc) return;
    Object.assign(doc, { path, saved: written, reference: false });
    this.#changed("documents");
  }

  /** Closes the project and its documents, whose changes the caller settled. */
  closeProject() {
    this.root = null;
    this.files = [];
    this.documents = [];
    this.currentId = null;
    this.#changed("project");
  }

  /** Renames the open documents of `from` to `to`, after the file itself was renamed. */
  renamed(from, to) {
    for (const doc of this.documents) if (doc.path === from) doc.path = to;
    this.#changed("documents");
  }

  /** Leaves `path` out of the file list until it is read again, as a file moving to the trash. */
  hideFile(path) {
    this.files = this.files.filter((p) => p !== path);
    this.#changed("files");
  }

  select(id) {
    if (!this.document(id) || this.currentId === id) return;
    this.currentId = id;
    this.#changed("current");
  }

  /** Records the document's new editor state, after an edit or a move. */
  update(id, state) {
    const doc = this.document(id);
    if (!doc) return;
    const wasDirty = this.isDirty(doc);
    doc.state = state;
    if (this.isDirty(doc) !== wasDirty) this.#changed("dirty");
    else this.#changed("state");
  }

  /** Writes the document with `id`; its saved text is what was written. */
  /**
   * Writes document `id`; false when there was nothing to write (a
   * reference). Unless `overwrite`, the write is refused, with a SidecarError
   * of code -32003, when the file changed on disk since it was read.
   */
  async save(id, options = {}) {
    const doc = this.document(id);
    if (!doc || doc.reference) return false;
    // One save of a file at a time: each sends the token the one before it got back. With none in flight,
    // the write goes at once.
    const turn = doc.saving ? doc.saving.catch(() => {}).then(() => this.#write(doc, options)) : this.#write(doc, options);
    doc.saving = turn;
    const settled = () => {
      if (doc.saving === turn) doc.saving = null;
    };
    turn.then(settled, settled);
    return turn;
  }

  async #write(doc, { overwrite = false }) {
    const id = doc.id;
    const written = doc.state.doc;
    const params = { path: doc.path, text: written.toString() };
    if (!overwrite && doc.mtime !== undefined) params.mtime = doc.mtime;
    const answer = await this.#sidecar.request("file.write", params);
    if (this.document(id) !== doc) return true;  // closed while the write was in flight
    doc.saved = written;
    doc.mtime = answer?.mtime;
    this.#changed("dirty");
    return true;
  }

  /**
   * Reads document `id`'s file again: a clean document takes the text on
   * disk, keeping its cursor where it can; one with changes keeps them,
   * unless `force`. Answers "same", "reloaded", "changed" (on disk, under
   * unsaved changes), or "missing".
   */
  async reread(id, { force = false } = {}) {
    const doc = this.document(id);
    if (!doc || doc.reference) return "same";
    let read;
    try {
      read = await this.#sidecar.request("file.read", { path: doc.path });
    } catch {
      return "missing";
    }
    if (this.document(id) !== doc || (read.mtime === doc.mtime && !force)) return "same";
    if (this.isDirty(doc) && !force) return "changed";
    if (read.text === doc.state.doc.toString()) {
      doc.mtime = read.mtime;
      return "same";
    }
    const head = Math.min(doc.state.selection.main.head, read.text.length);
    doc.state = doc.state.update({ changes: { from: 0, to: doc.state.doc.length, insert: read.text },
      selection: { anchor: head } }).state;
    doc.saved = doc.state.doc;
    doc.mtime = read.mtime;
    this.#changed("reloaded");
    return "reloaded";
  }

  /**
   * Closes the document with `id`, whatever its changes; the current tab
   * moves to its right-hand neighbour, or else its left.
   */
  close(id) {
    const index = this.documents.findIndex((d) => d.id === id);
    if (index < 0) return;
    this.documents.splice(index, 1);
    if (this.currentId === id) {
      const next = this.documents[index] ?? this.documents[index - 1];
      this.currentId = next ? next.id : null;
    }
    this.#changed("documents");
  }

  #changed(what) {
    this.dispatchEvent(new CustomEvent("change", { detail: what }));
  }
}
