// The editor's workspace: the open project, its files, and the documents
// open in tabs, read and written through the sidecar (tauri-editor.md §4.5).
// It holds no DOM; the views render it and call its methods.

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
    const { text } = await this.#sidecar.request("file.read", { path });
    const again = this.documentOf(path);  // opened while the read was in flight
    if (again) {
      this.select(again.id);
      return again;
    }
    const state = this.#makeState(text, path);
    const doc = { id: this.#nextId++, path, state, saved: state.doc };
    this.documents.push(doc);
    this.currentId = doc.id;
    this.#changed("documents");
    return doc;
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
  async save(id) {
    const doc = this.document(id);
    if (!doc) return;
    const written = doc.state.doc;
    await this.#sidecar.request("file.write", { path: doc.path, text: written.toString() });
    if (this.document(id) !== doc) return;  // closed while the write was in flight
    doc.saved = written;
    this.#changed("dirty");
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
