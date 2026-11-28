// The editor's workspace: the open project, its files, and the documents
// open in tabs, read and written through the sidecar (tauri-editor.md §4.5).
// It holds no DOM; the views render it and call its methods.

import { EditorState, StateEffect } from "@codemirror/state";

/** The name of a project from its root: the last component of the path. */
export function projectName(root) {
  return withoutSlash(root).split("/").pop() || root;
}

/** A folder's path without the slashes it ends with; "/" stays itself. */
export function withoutSlash(path) {
  return path.replace(/(.)\/+$/, "$1");
}

/** The folder that holds `path`: "/home/a" for "/home/a/b", "/" for "/b". */
export function parentOf(path) {
  const plain = withoutSlash(path);
  return plain.slice(0, plain.lastIndexOf("/")) || "/";
}

/** `name` inside the folder `dir`. */
export function joinPath(dir, name) {
  return `${withoutSlash(dir).replace(/\/$/, "")}/${name}`;
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

/** The one change that turns `from` into `to`: what lies between their common start and their common end. */
export function difference(from, to) {
  let start = 0;
  const shorter = Math.min(from.length, to.length);
  while (start < shorter && from[start] === to[start]) start += 1;
  let end = 0;
  while (end < shorter - start && from[from.length - 1 - end] === to[to.length - 1 - end]) end += 1;
  return { from: start, to: from.length - end, insert: to.slice(start, to.length - end) };
}

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
  #hidden = new Set();  // files left out of the list until told (a file waiting to move to the trash)

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
   * project NAME, else the only .lisp file at the project's top level other than project.lisp; null when none of
   * these is.
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

  /** Unsaved changes, or a file deleted on disk whose text only the tab still holds. */
  isDirty(doc) {
    return !doc.state.doc.eq(doc.saved) || doc.missing;
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
    this.#hidden.clear();
    this.files = opened.files;
    this.documents = [];
    this.currentId = null;
    this.#changed("project");
    return opened;
  }

  /**
   * Reads the project's file list again (project.files, which leaves the sidecar's project as it is); false when it
   * could not be read, which a later refresh mends.
   */
  async refreshFiles() {
    if (this.root === null) return false;
    const root = this.root;
    let listed;
    try {
      listed = await this.#sidecar.request("project.files", {});
    } catch {
      return false;
    }
    if (this.root !== root) return false;  // another project opened meanwhile: the list is the last one's
    const files = listed.filter((p) => !this.#hidden.has(p));
    // An unchanged list is not drawn again: the side bar keeps its scroll, and nothing is redrawn for a save.
    if (files.length === this.files.length && files.every((p, i) => p === this.files[i])) return true;
    this.files = files;
    this.#changed("files");
    return true;
  }

  /** Names the open project to a sidecar that started again (Restart), its documents kept. */
  async reattach() {
    if (this.root === null) return;
    const opened = await this.#sidecar.request("project.open", { root: this.root });
    this.files = opened.files.filter((p) => !this.#hidden.has(p));
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
    const doc = { id: this.#nextId++, path, state, saved: state.doc, mtime, missing: false, changedOnDisk: false };
    this.documents.push(doc);
    this.currentId = doc.id;
    this.#changed("documents");
    return doc;
  }

  /**
   * Opens `text` read-only in a tab titled `title`, or shows its tab: a
   * reference (Help), which belongs to no project and is never saved. The
   * fields of `extra` (its view) are the document's before it is shown.
   */
  openText(title, text, extra = {}) {
    const open = this.documents.find((d) => d.reference && d.path === title);
    if (open) {
      this.select(open.id);
      return open;
    }
    const state = this.#makeState(text, title);
    const readOnly = state.update({ effects: StateEffect.appendConfig.of(EditorState.readOnly.of(true)) }).state;
    const doc = { id: this.#nextId++, path: title, state: readOnly, saved: readOnly.doc, reference: true, ...extra };
    this.documents.push(doc);
    this.currentId = doc.id;
    this.#changed("documents");
    return doc;
  }

  /** Makes document `id` the file at `path`, saved as `written` with the token `mtime`, after Save As wrote it. */
  savedAs(id, path, written, mtime) {
    const doc = this.document(id);
    if (!doc) return;
    Object.assign(doc, { path, saved: written, reference: false, mtime, missing: false, changedOnDisk: false });
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

  /** Leaves `path` out of the file list, read again or not, until unhideFile: a file moving to the trash. */
  hideFile(path) {
    this.#hidden.add(path);
    this.files = this.files.filter((p) => p !== path);
    this.#changed("files");
  }

  /** Lists `path` again when it is on disk (the next refreshFiles). */
  unhideFile(path) {
    this.#hidden.delete(path);
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

  /**
   * Writes document `id`, whose saved text becomes what was written; false
   * when there was nothing to write (a reference). Unless `overwrite`, the
   * write is refused, with a SidecarError of code -32003, when the file
   * changed on disk since it was read.
   */
  async save(id, options = {}) {
    const doc = this.document(id);
    if (!doc || doc.reference) return false;
    // One save of a file at a time: each sends the token the one before it got back. With none in flight,
    // the write goes at once; a save of the very text being written is that write (Vim's :w runs Save twice).
    if (doc.saving && doc.savingText === doc.state.doc && !options.overwrite) return doc.saving;
    const turn = doc.saving ? doc.saving.catch(() => {}).then(() => this.#write(doc, options)) : this.#write(doc, options);
    doc.saving = turn;
    doc.savingText = doc.state.doc;
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
    doc.missing = false;  // on disk again, as the editor has it
    doc.changedOnDisk = false;
    this.#changed("dirty");
    return true;
  }

  /**
   * Reads document `id`'s file again: a clean document takes the text on
   * disk, keeping its cursor where it can; one with changes keeps them,
   * unless `force`. Answers "same", "reloaded", "changed" (on disk, under
   * unsaved changes), "missing" (deleted on disk), or "unread" (it could not
   * be read: not text, no permission, no client).
   */
  async reread(id, { force = false } = {}) {
    const doc = this.document(id);
    if (!doc || doc.reference) return "same";
    const failed = (error) => (error?.code === -32004 ? "missing" : "unread");
    // Its token first: a file that did not change (the watcher's echo of a save) is not read.
    if (!force) {
      try {
        const { mtime } = await this.#sidecar.request("file.stat", { path: doc.path });
        if (mtime === doc.mtime) return "same";
      } catch (error) {
        return failed(error);
      }
    }
    let read;
    try {
      read = await this.#sidecar.request("file.read", { path: doc.path });
    } catch (error) {
      return failed(error);
    }
    if (this.document(id) !== doc || (read.mtime === doc.mtime && !force)) return "same";
    if (!doc.state.doc.eq(doc.saved) && !force) return "changed";  // the editor's changes, whatever the disk's
    if (read.text === doc.state.doc.toString()) {
      doc.mtime = read.mtime;
      return "same";
    }
    // Only the lines that differ change, so the cursor, marks, and folds outside them stay where they were.
    doc.state = doc.state.update({ changes: difference(doc.state.doc.toString(), read.text) }).state;
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
