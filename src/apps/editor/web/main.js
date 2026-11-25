// The GUI editor's window (tauri-editor.md §5.2): layout B in Carbon
// (brand/gui/design-guide.md §4–5, patterns.md §1–13). The top bar with
// Play, Update, and Stop, the navigator, tabs over the editor, the panel
// with Console and Problems, the status bar, and the banner for a missing
// server, over the workspace (workspace.js) and the sidecar (sidecar.js).
//
// The pickers and find come with step 7 (editor-refactor.md §4); until then
// the command center shows the project's name.

import { h, hasMod, keyLabel, keyCaps } from "./dom.js";
import { icon } from "./icons.js";
import { Sidecar } from "./sidecar.js";
import { Workspace, baseName, entryFile } from "./workspace.js";
import { editorState, EditorView, markEvaluated, offsetOf, showProblems, where } from "./codeEditor.js";
import { formAt, formBefore } from "./lisp.js";

const layoutKey = "espresso.layout";
const projectKey = "espresso.project";
const runKey = "espresso.run";
const logLimit = 1000;
const markMs = 600;  // how long an evaluated form stays marked

/** Play's options (design-guide.md §5.1): crossfades in seconds, and the quantize grids. */
export const fades = [[0, "No Crossfade"], [0.05, "50 ms"], [0.25, "250 ms"], [1, "1 s"]];
export const grids = [["none", "None"], ["beat", "Beat"], ["bar", "Bar"]];

class App {
  /** @type {Sidecar | null} */
  sidecar = null;
  /** @type {Workspace} */
  workspace;
  /** The server's last `status` (tauri-editor.md §4.6), or null before the first. */
  status = null;
  /** Where the sidecar reaches the server, from `hello`: "127.0.0.1:57290". */
  serverAddress = "";
  /** True once the link has said anything about the server: a status or a server line. */
  serverReported = false;
  /** True once the server has answered in this session, so its absence is a lost connection. */
  wasConnected = false;
  /** True while a Retry is in flight. */
  connecting = false;
  /** Console entries: {kind, text, location?}. */
  log = [];
  /** The errors with a place since the last Play: {path, line, column, message}. */
  problems = [];
  #problemsVersion = 0;
  layout = { side: true, panel: true };
  run = { fade: 0.05, quantize: "none", updateOnSave: false };
  panelView = "console";
  maximized = false;
  filter = "";
  #treeFocus = 0;

  constructor(root) {
    this.root = root;
    for (const [key, into] of [[layoutKey, this.layout], [runKey, this.run]]) {
      try {
        Object.assign(into, JSON.parse(localStorage.getItem(key) ?? "{}"));
      } catch {
        // A damaged entry keeps the defaults.
      }
    }
    this.#build();
    window.addEventListener("keydown", (e) => this.#onKey(e), true);
  }

  // ---- The skeleton, built once (§1).

  #build() {
    const iconButton = (name, label, keys, onclick) =>
      h("button", { class: "btn flat", title: keys ? `${label} (${keyLabel(keys)})` : label, "aria-label": label, onclick },
        icon(name));

    this.playLabel = h("span", { class: "label" }, "Play");
    this.playButton = h("button", { class: "btn primary", title: "Play (F5)", onclick: () => this.play() },
      icon("play"), this.playLabel);
    this.optionsButton = h("button", { class: "btn primary arrow", title: "Play options", "aria-label": "Play options",
      "aria-haspopup": "menu", onclick: () => this.#toggleMenu(this.optionsButton, () => this.#optionsMenu()) },
    icon("down"));
    this.stopButton = h("button", { class: "btn flat", title: `Stop (${keyLabel("Mod+.")})`, onclick: () => this.stop() },
      icon("stop"), h("span", { class: "label" }, "Stop"));
    this.menuButton = iconButton("menu", "Main menu", "", () => this.#toggleMenu(this.menuButton, () => this.#mainMenu()));
    this.menuButton.setAttribute("aria-haspopup", "menu");
    const top = h("header", { class: "top" },
      h("div", { class: "start" }, this.playButton, this.optionsButton, this.stopButton),
      this.commandCenter = h("div", { class: "command-center" }),
      h("div", { class: "end" },
        this.sideToggle = iconButton("sidebar", "Toggle side bar", "Mod+B", () => this.toggle("side")),
        this.panelToggle = iconButton("panel", "Toggle panel", "Mod+J", () => this.toggle("panel")),
        this.menuButton));

    this.banner = h("div", { class: "banner", role: "status", hidden: true });

    this.sideTitle = h("div", { class: "title" });
    this.tree = h("div", { class: "tree", role: "tree", "aria-label": "Project files" });
    this.side = h("nav", { class: "side", "aria-label": "Project" }, this.sideTitle, this.tree);

    this.tabs = h("div", { class: "tabs", role: "tablist", "aria-label": "Open files" });
    this.editorHost = h("div", { class: "editor" });
    this.view = new EditorView({ parent: this.editorHost });
    this.empty = h("div", { class: "empty" });
    this.editorHost.append(this.empty);

    // §7: Console and Problems; the filter applies to the console.
    this.logView = h("div", { class: "log", role: "log", "aria-label": "Console" });
    this.problemsView = h("div", { class: "log", role: "list", "aria-label": "Problems" });
    this.problemsBadge = h("span", { class: "badge", hidden: true });
    const viewTab = (name, label, ...extra) => h("button", {
      class: "view", role: "tab", "data-view": name, onclick: () => this.showPanelView(name),
    }, label, ...extra);
    this.panelTabs = [viewTab("console", "Console"), viewTab("problems", "Problems", this.problemsBadge)];
    this.filterField = h("input", {
      class: "filter", type: "search", placeholder: "Filter messages", "aria-label": "Filter messages",
      oninput: () => {
        this.filter = this.filterField.value;
        this.renderLog();
      },
    });
    this.maxButton = iconButton("max", "Maximize panel", "", () => this.toggleMaximized());
    this.panel = h("section", { class: "panel", "aria-label": "Panel" },
      h("div", { class: "bar" },
        h("div", { role: "tablist", style: "display: contents" }, this.panelTabs),
        h("div", { class: "grow" }),
        h("div", { class: "tools" },
          this.filterField,
          iconButton("clear", "Clear console", "", () => this.clearLog()),
          this.maxButton,
          iconButton("close", "Close panel", "Mod+J", () => this.toggle("panel")))),
      this.logView, this.problemsView);

    this.content = h("div", { class: "content" }, this.tabs, this.editorHost, this.panel);
    this.mainArea = h("div", { class: "main" }, this.side, this.content);
    this.statusBar = h("footer", { class: "status", "aria-label": "Status" });
    this.window = h("div", { class: "window" }, top, this.banner, this.mainArea, this.statusBar);
    this.root.append(this.window);
  }

  // ---- Starting.

  async start() {
    this.workspace = new Workspace(
      { request: (method, params) => this.sidecar.request(method, params) },
      (text, path) => editorState(text, path, (state) => this.#edited(state)));
    this.workspace.addEventListener("change", (e) => this.#changed(e.detail));
    this.renderAll();
    if (!window.__TAURI__) {
      this.addLog({ kind: "error", text: "Not running in the editor: open it with cargo tauri dev" });
      return;
    }
    this.#guardClose();
    try {
      this.sidecar = await Sidecar.start();
    } catch (error) {
      this.addLog({ kind: "error", text: `The client could not start: ${error.message ?? error}` });
      return;
    }
    this.sidecar.on("status", (s) => {
      this.status = s;
      this.serverReported = true;
      if (s.connected) this.wasConnected = true;
      this.renderServer();
    });
    this.sidecar.on("console", (entry) => this.addLog(entry));
    this.sidecar.on("close", (reason) => {
      this.sidecar = null;
      this.addLog({ kind: "error", text: `The client stopped: ${reason}` });
      this.renderServer();
    });
    const hello = await this.sidecar.request("hello");
    this.serverAddress = hello.server ?? "";
    this.renderBanner();
    const saved = localStorage.getItem(projectKey);
    if (saved) await this.openProject(saved, { quiet: true });
  }

  // ---- Commands.

  /** Chooses a folder and opens it as the project (Open Folder…). */
  async openFolder() {
    const dialog = window.__TAURI__?.dialog;
    if (!dialog) return;
    const root = await dialog.open({ directory: true, multiple: false, title: "Open Folder" });
    if (typeof root !== "string") return;  // cancelled
    if (!(await this.settleChanges())) return;
    await this.openProject(root);
  }

  async openProject(root, { quiet = false } = {}) {
    try {
      await this.workspace.openProject(root);
      localStorage.setItem(projectKey, root);
      this.problems = [];
      this.#problemsChanged();
    } catch (error) {
      if (quiet) localStorage.removeItem(projectKey);
      this.addLog({ kind: "error", text: `Could not open ${root}: ${error.message}` });
    }
  }

  async openFile(path) {
    try {
      await this.workspace.openFile(path);
      this.view.focus();
      return true;
    } catch (error) {
      this.addLog({ kind: "error", text: `Could not open ${path}: ${error.message}` });
      return false;
    }
  }

  /** Saves the document with `id`; false when the write failed. Updates the plan after, when asked to. */
  async save(id) {
    const doc = this.workspace.document(id);
    if (!doc) return false;
    try {
      await this.workspace.save(id);
    } catch (error) {
      this.addLog({ kind: "error", text: `Could not save ${doc.path}: ${error.message}` });
      return false;
    }
    if (this.run.updateOnSave && this.playing) this.play();
    return true;
  }

  /** Closes the document with `id`, asking first when it has unsaved changes (patterns.md §13). */
  async close(id) {
    const doc = this.workspace.document(id);
    if (!doc) return;
    if (this.workspace.isDirty(doc)) {
      const choice = await this.ask({
        heading: "Save Changes?",
        body: `Unsaved changes to “${baseName(doc.path)}” will be lost.`,
      });
      if (choice === "cancel") return;
      if (choice === "save" && !(await this.save(id))) return;
    }
    this.workspace.close(id);
  }

  /**
   * Before the documents close together (a new project, the window):
   * asks once about every unsaved one; false when the user cancels or a
   * save fails.
   */
  async settleChanges() {
    const dirty = this.workspace.dirtyDocuments;
    if (dirty.length === 0) return true;
    const ids = dirty.map((d) => d.id);
    const choice = await this.ask(dirty.length === 1
      ? { heading: "Save Changes?", body: `Unsaved changes to “${baseName(dirty[0].path)}” will be lost.` }
      : { heading: `Save Changes to ${dirty.length} Files?`, body: "Their unsaved changes will be lost." });
    if (choice === "cancel") return false;
    if (choice === "discard") return true;
    for (const id of ids) {
      if (!(await this.save(id))) return false;
    }
    return true;
  }

  // ---- Running (design-guide.md §5.1; tauri-editor.md §4.5.1).

  get connected() {
    return Boolean(this.sidecar && this.status?.connected);
  }

  /** True while a plan plays: Play reads Update. */
  get playing() {
    return this.connected && this.status.plan > 0;
  }

  /** The open .lisp documents' text, unsaved edits included, by path. */
  #sources() {
    const sources = {};
    for (const doc of this.workspace.documents) {
      if (doc.path.endsWith(".lisp")) sources[doc.path] = doc.state.doc.toString();
    }
    return sources;
  }

  async #request(method, params) {
    try {
      return await this.sidecar.request(method, params);
    } catch (error) {
      this.addLog({ kind: "error", text: `${method}: ${error.message}` });
      return null;
    }
  }

  /** Compiles the project, with the editor's buffers, and plays it or updates the plan that plays. */
  async play() {
    if (!this.connected || this.workspace.root === null) return;
    this.problems = [];
    this.#problemsChanged();
    await this.#request("run.play", {
      sources: this.#sources(), file: this.workspace.current?.path, fade: this.run.fade, quantize: this.run.quantize,
    });
  }

  async stop() {
    if (!this.playing) return;
    await this.#request("run.stop", { fade: this.run.fade });
  }

  /**
   * Evaluates the form at the cursor: with "form", the top-level form it is
   * in; with "last", the datum that ends before it. The form's lines are
   * marked for a moment.
   */
  async evaluate(which) {
    const doc = this.workspace.current;
    if (!doc || !this.connected) return;
    const text = doc.state.doc.toString();
    const head = doc.state.selection.main.head;
    const range = which === "form" ? formAt(text, head) : formBefore(text, head);
    if (!range) {
      this.addLog({ kind: "info", text: "evaluate: no complete form at the cursor" });
      return;
    }
    const line = doc.state.doc.lineAt(range.from);
    const column = [...line.text.slice(0, range.from - line.from)].length + 1;
    this.#dispatchTo(doc.id, markEvaluated(range));
    setTimeout(() => this.#dispatchTo(doc.id, markEvaluated(null)), markMs);
    await this.#request("run.eval", {
      text: text.slice(range.from, range.to), file: doc.path, line: line.number, column,
      sources: this.#sources(), fade: this.run.fade, quantize: this.run.quantize,
    });
  }

  /** Asks the link to reach the server now (the banner's Retry). */
  async retry() {
    if (!this.sidecar || this.connecting) return;
    this.connecting = true;
    this.renderBanner();
    await this.#request("server.connect", {});
    this.connecting = false;
    this.renderBanner();
  }

  #setRun(changes) {
    Object.assign(this.run, changes);
    localStorage.setItem(runKey, JSON.stringify(this.run));
  }

  // ---- The panel.

  toggle(region) {
    this.layout[region] = !this.layout[region];
    if (region === "panel" && !this.layout.panel) this.maximized = false;
    localStorage.setItem(layoutKey, JSON.stringify(this.layout));
    this.renderLayout();
  }

  toggleMaximized() {
    this.maximized = !this.maximized;
    this.renderLayout();
  }

  /** Shows the panel's "console" or "problems" view, opening the panel. */
  showPanelView(name) {
    this.panelView = name;
    if (!this.layout.panel) this.toggle("panel");
    this.renderPanelView();
  }

  /** Shows `problem` in Problems, its row focused (a hover's View Problem). */
  viewProblem(problem) {
    this.showPanelView("problems");
    this.problemsView.querySelectorAll(".msg")[this.problems.indexOf(problem)]?.focus();
  }

  /** Opens a problem's file with the cursor at its place. */
  async goTo(location) {
    if (!(await this.openFile(location.path))) return;
    const offset = offsetOf(this.view.state, location.line, location.column);
    this.view.dispatch({ selection: { anchor: offset }, scrollIntoView: true });
    this.view.focus();
  }

  addLog(entry) {
    if (entry.kind === "server" && !this.serverReported) {
      this.serverReported = true;
      this.renderServer();
    }
    this.log.push(entry);
    if (this.log.length > logLimit) this.log.splice(0, this.log.length - logLimit);
    this.renderLog();
    if (entry.kind === "error" && entry.location) {
      this.problems.push({ ...entry.location, message: messageOf(entry) });
      this.#problemsChanged();
    }
  }

  clearLog() {
    this.log = [];
    this.renderLog();
  }

  // Shows the problems in Problems, the status bar, and every open document.
  #problemsChanged() {
    ++this.#problemsVersion;
    this.renderProblems();
    this.renderStatus();
    this.#showProblemsInDocuments();
  }

  #showProblemsInDocuments() {
    for (const doc of this.workspace.documents) {
      if (doc.problemsShown === this.#problemsVersion) continue;
      doc.problemsShown = this.#problemsVersion;
      const own = this.problems.filter((p) => p.path === doc.path);
      this.#dispatchTo(doc.id, (state) => showProblems(state, own, (p) => this.viewProblem(p)));
    }
  }

  // Applies a transaction to a document's state: through the view when it
  // shows the document, else to the state the workspace keeps.
  #dispatchTo(id, spec) {
    const doc = this.workspace.document(id);
    if (!doc) return;
    const make = typeof spec === "function" ? spec : () => spec;
    if (id === this.workspace.currentId && this.view.state === doc.state) {
      this.view.dispatch(make(this.view.state));
    } else {
      this.workspace.update(id, doc.state.update(make(doc.state)).state);
    }
  }

  // ---- The window's close (patterns.md §13.2).

  #guardClose() {
    const tauriWindow = window.__TAURI__?.window?.getCurrentWindow();
    if (!tauriWindow) return;
    let settled = false;
    tauriWindow.onCloseRequested(async (event) => {
      if (settled || this.workspace.dirtyDocuments.length === 0) return;
      event.preventDefault();
      if (await this.settleChanges()) {
        settled = true;
        await tauriWindow.destroy();
      }
    });
  }

  // ---- Keys (patterns.md §14.3: cua). Shift+Enter evaluates only from the editor.

  #onKey(e) {
    if (e.key === "Escape" && this.menu) {
      e.preventDefault();
      this.closeMenu(true);
      return;
    }
    if (this.scrim || e.altKey || (e.ctrlKey && e.metaKey)) return;
    const mod = hasMod(e);
    if (!mod && (e.ctrlKey || e.metaKey)) return;
    const key = e.key.length === 1 ? e.key.toUpperCase() : e.key;
    const chord = `${mod ? "Mod+" : ""}${e.shiftKey ? "Shift+" : ""}${key}`;
    const current = this.workspace.current;
    const inEditor = Boolean(current) && this.view.dom.contains(document.activeElement);
    const run = {
      "Mod+O": () => this.openFolder(),
      "Mod+S": current && (() => this.save(current.id)),
      "Mod+W": current && (() => this.close(current.id)),
      "Mod+B": () => this.toggle("side"),
      "Mod+J": () => this.toggle("panel"),
      "Mod+Shift+M": () => this.showPanelView("problems"),
      F5: () => this.play(),
      "Mod+.": () => this.stop(),
      "Mod+Enter": inEditor && (() => this.evaluate("form")),
      "Shift+Enter": inEditor && (() => this.evaluate("last")),
    }[chord];
    if (!run) return;
    e.preventDefault();
    e.stopPropagation();
    run();
  }

  // ---- The workspace's changes.

  #edited(state) {
    const id = this.workspace.currentId;
    if (id !== null) this.workspace.update(id, state);
  }

  #changed(what) {
    if (what === "state") {
      this.renderStatus();
      return;
    }
    if (what === "dirty") {
      this.renderTabs();
      this.renderTitle();
      this.renderStatus();
      return;
    }
    if (what === "project" || what === "documents") this.#showProblemsInDocuments();
    this.renderAll();
  }

  // ---- Rendering.

  renderAll() {
    this.renderLayout();
    this.renderTop();
    this.renderBanner();
    this.renderSide();
    this.renderTabs();
    this.renderEditor();
    this.renderLog();
    this.renderProblems();
    this.renderStatus();
    this.renderTitle();
    this.renderPanelView();
  }

  renderPanelView() {
    for (const tab of this.panelTabs) tab.setAttribute("aria-selected", String(tab.dataset.view === this.panelView));
    this.logView.hidden = this.panelView !== "console";
    this.problemsView.hidden = this.panelView !== "problems";
    this.filterField.hidden = this.panelView !== "console";
  }

  /** Everything that shows the server: Play and Stop, the banner, the status bar. */
  renderServer() {
    this.renderTop();
    this.renderBanner();
    this.renderStatus();
  }

  renderLayout() {
    this.mainArea.classList.toggle("no-side", !this.layout.side);
    this.side.hidden = !this.layout.side;
    this.content.classList.toggle("no-panel", !this.layout.panel);
    this.content.classList.toggle("max-panel", this.layout.panel && this.maximized);
    this.panel.hidden = !this.layout.panel;
    this.sideToggle.setAttribute("aria-pressed", String(this.layout.side));
    this.panelToggle.setAttribute("aria-pressed", String(this.layout.panel));
    const label = this.maximized ? "Restore panel" : "Maximize panel";
    this.maxButton.replaceChildren(icon(this.maximized ? "down" : "max"));
    this.maxButton.title = label;
    this.maxButton.setAttribute("aria-label", label);
  }

  // §2: Play reads Update while a plan plays; both run buttons need a server.
  renderTop() {
    const playing = this.playing;
    const label = playing ? "Update" : "Play";
    this.playLabel.textContent = label;
    this.playButton.title = `${label} (F5)`;
    this.playButton.disabled = !this.connected || this.workspace.root === null;
    this.optionsButton.disabled = !this.connected;
    this.stopButton.disabled = !playing;
    this.commandCenter.replaceChildren(icon("search"), this.workspace.name || "espresso");
  }

  // §5.10: a missing or lost server is a state, shown across the window until it answers.
  renderBanner() {
    const absent = this.serverReported && !this.connected && this.sidecar !== null;
    this.banner.hidden = !absent;
    if (!absent) return;
    const at = this.serverAddress ? ` at ${this.serverAddress}` : "";
    this.banner.replaceChildren(icon("warning"),
      h("span", {}, this.wasConnected ? `No reply from espresso-server${at}` : `Not connected to espresso-server${at}`),
      h("button", { class: "btn", disabled: this.connecting, onclick: () => this.retry() },
        this.connecting ? "Connecting…" : "Retry"));
  }

  // §3: the project's files, the current one marked, the entry tagged.
  renderSide() {
    const ws = this.workspace;
    this.sideTitle.textContent = ws.name || "No Folder Open";
    this.sideTitle.title = ws.root ?? "";
    if (ws.root === null) {
      this.tree.replaceChildren();
      return;
    }
    if (ws.files.length === 0) {
      this.tree.replaceChildren(h("div", { class: "none" }, "The folder is empty"));
      return;
    }
    this.#treeFocus = Math.min(this.#treeFocus, ws.files.length - 1);
    const current = ws.current?.path;
    const rows = ws.files.map((path, i) => {
      const slash = path.lastIndexOf("/");
      return h("div", {
        class: `row${path === current ? " current" : ""}`,
        role: "treeitem",
        "aria-selected": String(path === current),
        tabindex: i === this.#treeFocus ? "0" : "-1",
        title: path,
        onclick: () => {
          this.#treeFocus = i;
          this.openFile(path);
        },
        onkeydown: (e) => this.#treeKey(e, i),
      },
      icon("file"),
      h("span", { class: "name" }, slash < 0 ? "" : h("span", { class: "dim" }, path.slice(0, slash + 1)),
        path.slice(slash + 1)),
      path === entryFile ? h("span", { class: "tag" }, "entry") : null);
    });
    this.tree.replaceChildren(...rows);
  }

  #treeKey(e, i) {
    const files = this.workspace.files;
    const to = { ArrowDown: i + 1, ArrowUp: i - 1, Home: 0, End: files.length - 1 }[e.key];
    if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      this.openFile(files[i]);
      return;
    }
    if (to === undefined || to < 0 || to >= files.length) return;
    e.preventDefault();
    this.#treeFocus = to;
    const rows = this.tree.querySelectorAll(".row");
    rows[i].tabIndex = -1;
    rows[to].tabIndex = 0;
    rows[to].focus();
  }

  // §4: tabs; an unsaved tab's dot stands in its close button's place.
  renderTabs() {
    const ws = this.workspace;
    const tabs = ws.documents.map((doc) => {
      const current = doc.id === ws.currentId;
      const dirty = ws.isDirty(doc);
      const name = baseName(doc.path);
      const close = h("button", {
        class: "close", title: `Close (${keyLabel("Mod+W")})`, "aria-label": `Close ${name}`, tabindex: "-1",
        onclick: (e) => {
          e.stopPropagation();
          this.close(doc.id);
        },
      }, icon("close"));
      return h("div", {
        class: `tab${current ? " current" : ""}${dirty ? " unsaved" : ""}`,
        role: "tab",
        "aria-selected": String(current),
        tabindex: current ? "0" : "-1",
        title: dirty ? `${doc.path} (unsaved)` : doc.path,
        onclick: () => ws.select(doc.id),
        onauxclick: (e) => {
          if (e.button === 1) this.close(doc.id);
        },
        onkeydown: (e) => this.#tabKey(e, doc.id),
      },
      name,
      h("span", { class: "slot" },
        dirty ? h("span", { role: "img", "aria-label": "Unsaved changes", title: "Unsaved changes" }, icon("dot", "dot")) : null,
        close));
    });
    this.tabs.replaceChildren(...tabs);
  }

  #tabKey(e, id) {
    const docs = this.workspace.documents;
    const i = docs.findIndex((d) => d.id === id);
    const to = { ArrowRight: i + 1, ArrowLeft: i - 1, Home: 0, End: docs.length - 1 }[e.key];
    if (to === undefined || to < 0 || to >= docs.length) return;
    e.preventDefault();
    this.workspace.select(docs[to].id);
    this.tabs.querySelectorAll(".tab")[to]?.focus();
  }

  // §5, §11: the current document, or the empty state.
  renderEditor() {
    const doc = this.workspace.current;
    this.view.dom.hidden = !doc;
    this.empty.hidden = Boolean(doc);
    if (doc) {
      if (this.view.state !== doc.state) this.view.setState(doc.state);
      return;
    }
    const noProject = this.workspace.root === null;
    const shortcuts = [["Open Folder…", "Mod+O"], ["Toggle Side Bar", "Mod+B"], ["Toggle Panel", "Mod+J"]];
    this.empty.replaceChildren(h("div", { class: "center" },
      h("img", { src: new URL("./mark.png", import.meta.url).href, height: "112", alt: "" }),
      h("div", { class: "heading" },
        h("h1", {}, noProject ? "No Folder Open" : "No File Open"),
        h("p", {}, noProject ? "Open a project folder to edit and play its files"
          : `Open a file of ${this.workspace.name} from the side bar`)),
      noProject ? h("button", { class: "btn primary", onclick: () => this.openFolder() }, "Open Folder…") : null,
      h("div", { class: "watermark" },
        shortcuts.flatMap(([label, keys]) => [h("span", { class: "label" }, label), h("span", {}, keyCaps(keys))]))));
  }

  // §7: the console's entries, an error's place a link to it, through the filter.
  renderLog() {
    const filter = this.filter.trim().toLowerCase();
    const shown = filter ? this.log.filter((e) => e.text.toLowerCase().includes(filter)) : this.log;
    if (shown.length === 0) {
      this.logView.replaceChildren(h("div", { class: "none" }, filter ? "No messages match the filter" : "No messages"));
      return;
    }
    this.logView.replaceChildren(...shown.map((entry) => {
      const place = entry.location && where(entry.location);
      const text = place && entry.text.startsWith(place)
        ? [h("a", { href: "#", title: `Go to ${place}`, onclick: (e) => {
          e.preventDefault();
          this.goTo(entry.location);
        } }, place), entry.text.slice(place.length)]
        : [entry.text];
      return h("div", { class: `msg ${entry.kind}` }, icon(entry.kind === "error" ? "error" : "info"), h("span", {}, text));
    }));
    this.logView.scrollTop = this.logView.scrollHeight;
  }

  // §7: Problems, each row a link to its place; the tab's badge counts them.
  renderProblems() {
    const count = this.problems.length;
    this.problemsBadge.hidden = count === 0;
    this.problemsBadge.textContent = String(count);
    this.panelTabs[1].setAttribute("aria-label", `Problems (${count})`);
    if (count === 0) {
      this.problemsView.replaceChildren(h("div", { class: "none" }, "No problems"));
      return;
    }
    this.problemsView.replaceChildren(...this.problems.map((problem) => h("div", {
      class: "msg error problem", role: "listitem", tabindex: "0", title: `Go to ${where(problem)}`,
      onclick: () => this.goTo(problem),
      onkeydown: (e) => {
        if (e.key === "Enter") this.goTo(problem);
      },
    }, icon("error"), h("span", {}, problem.message), h("span", { class: "where" }, where(problem)))));
  }

  // §8: the server on the left, problems after it; the editor's position and key scheme on the right.
  renderStatus() {
    const s = this.status;
    const items = [];
    if (this.connected) {
      this.serverItem = h("button", {
        class: "item", title: "Server status and counters", "aria-haspopup": "menu",
        onclick: () => this.#toggleMenu(this.serverItem, () => this.#serverMenu()),
      }, h("span", { class: "ok" }, icon("dot")), `${serverName(s)} · ${Math.round(busiest(s) * 100)}%`);
      items.push(this.serverItem);
      items.push(h("div", { class: "item hide-narrow" }, `${tempoText(s)} · ${beatText(s)}`));
      items.push(h("div", { class: "item hide-narrow" }, s.plan === 0 ? "No Plan" : `Plan ${s.plan}`));
    } else if (!this.serverReported) {
      items.push(h("div", { class: "item" }, icon("ring"), "Connecting…"));
    } else if (this.wasConnected) {
      items.push(h("div", { class: "item" }, h("span", { class: "warn" }, icon("warning")), "espresso-server · No reply"));
    } else {
      items.push(h("div", { class: "item" }, icon("ring"), "Not connected"));
    }
    if (this.workspace.root !== null) {
      const errors = this.problems.length;
      items.push(h("button", {
        class: "item", title: `${errors} ${errors === 1 ? "error" : "errors"}, 0 warnings (${keyLabel("Mod+Shift+M")})`,
        onclick: () => this.showPanelView("problems"),
      }, icon("error"), String(errors), icon("warning"), "0"));
    }
    items.push(h("div", { class: "grow" }));
    const doc = this.workspace.current;
    if (doc) {
      const head = doc.state.selection.main.head;
      const line = doc.state.doc.lineAt(head);
      items.push(h("div", { class: "item" }, `Ln ${line.number}, Col ${head - line.from + 1}`));
    }
    items.push(h("div", { class: "item hide-narrow", title: "Key scheme" }, "CUA"));
    this.statusBar.replaceChildren(...items);
  }

  renderTitle() {
    const ws = this.workspace;
    const doc = ws.current;
    const title = doc ? `${ws.isDirty(doc) ? "● " : ""}${baseName(doc.path)} — ${ws.name}`
      : ws.name ? `${ws.name} — espresso` : "espresso";
    if (document.title === title) return;
    document.title = title;
    window.__TAURI__?.window?.getCurrentWindow().setTitle(title).catch(() => {});
  }

  // ---- Menus (patterns.md §2, §8.3): opened from their button, closed by Escape or a press outside.

  #mainMenu() {
    const doc = this.workspace.current;
    return {
      label: "Main menu", place: "top: 34px; right: 8px",
      items: [
        { label: "Open Folder…", keys: "Mod+O", action: () => this.openFolder() },
        { label: "Save", keys: "Mod+S", action: () => this.save(doc.id), enabled: Boolean(doc) },
        { label: "Close Editor", keys: "Mod+W", action: () => this.close(doc.id), enabled: Boolean(doc) },
        "separator",
        { label: this.playing ? "Update" : "Play", keys: "F5", action: () => this.play(),
          enabled: this.connected && this.workspace.root !== null },
        { label: "Stop", keys: "Mod+.", action: () => this.stop(), enabled: this.playing },
        { label: "Evaluate Form", keys: "Mod+Enter", action: () => this.evaluate("form"), enabled: Boolean(doc) && this.connected },
        { label: "Evaluate Last Form", keys: "Shift+Enter", action: () => this.evaluate("last"),
          enabled: Boolean(doc) && this.connected },
        "separator",
        { label: this.layout.side ? "Hide Side Bar" : "Show Side Bar", keys: "Mod+B", action: () => this.toggle("side") },
        { label: this.layout.panel ? "Hide Panel" : "Show Panel", keys: "Mod+J", action: () => this.toggle("panel") },
        { label: "Show Problems", keys: "Mod+Shift+M", action: () => this.showPanelView("problems") },
      ],
    };
  }

  // §5.1: the crossfade and quantize a run uses, and whether a save updates the plan.
  #optionsMenu() {
    const radio = ([value, label], key) => ({
      label, checked: this.run[key] === value, role: "menuitemradio", action: () => this.#setRun({ [key]: value }),
    });
    return {
      label: "Play options", place: "top: 34px; left: 12px",
      items: [
        { head: "Crossfade" }, ...fades.map((f) => radio(f, "fade")),
        "separator",
        { head: "Quantize" }, ...grids.map((g) => radio(g, "quantize")),
        "separator",
        { label: "Update on Save", checked: this.run.updateOnSave, role: "menuitemcheckbox",
          action: () => this.#setRun({ updateOnSave: !this.run.updateOnSave }) },
      ],
    };
  }

  // §5.8: the server's counters, as SuperCollider's server menu shows them (patterns.md §8.3).
  #serverMenu() {
    const s = this.status;
    const row = (label, value) => ({ label, value });
    return {
      label: "Server", place: "left: 4px; bottom: 24px",
      items: [
        { head: s.server, small: [this.serverAddress, `${s.threads} ${s.threads === 1 ? "thread" : "threads"}`].filter(Boolean).join(" · ") },
        "separator",
        row("Load", `${Math.round(busiest(s) * 100)}%`),
        row("Missed deadlines", String(s.missed)),
        row("Xruns", String(s.xruns)),
        row("Drops", String(s.drops)),
        row("Overflows", String(s.overflows)),
        row("Plan", s.plan === 0 ? "None" : String(s.plan)),
      ],
    };
  }

  #toggleMenu(anchor, build) {
    const same = this.menuAnchor === anchor;
    if (this.menu) this.closeMenu(same);
    if (!same) this.#openMenu(anchor, build());
  }

  #openMenu(anchor, { label, place, items }) {
    const rows = items.map((item) => {
      if (item === "separator") return h("div", { class: "sep", role: "separator" });
      if (item.head !== undefined) {
        return h("div", { class: "head", role: "presentation" }, item.head, item.small ? h("small", {}, item.small) : null);
      }
      if (item.value !== undefined) {
        return h("div", { class: "row info", role: "presentation" }, h("span", {}, item.label), h("span", { class: "v" }, item.value));
      }
      const checkable = item.checked !== undefined;
      return h("button", {
        class: `row${checkable ? " checkable" : ""}`, role: item.role ?? "menuitem", tabindex: "-1",
        disabled: item.enabled === false, "aria-checked": checkable ? String(item.checked) : null,
        onclick: () => {
          this.closeMenu(false);
          item.action();
        },
      },
      checkable ? h("span", { class: "check" }, item.checked ? icon("check") : null) : null,
      h("span", { class: "grow" }, item.label), h("span", { class: "v" }, item.keys ? keyLabel(item.keys) : ""));
    });
    this.menu = h("div", { class: "menu", role: "menu", "aria-label": label, style: place, tabindex: "-1" }, rows);
    this.menu.addEventListener("keydown", (e) => {
      const buttons = [...this.menu.querySelectorAll("button.row:not([disabled])")];
      const i = buttons.indexOf(document.activeElement);
      const to = { ArrowDown: i + 1, ArrowUp: i - 1, Home: 0, End: buttons.length - 1 }[e.key];
      if (to === undefined || buttons.length === 0) return;
      e.preventDefault();
      buttons[(to + buttons.length) % buttons.length].focus();
    });
    this.menuAnchor = anchor;
    anchor.setAttribute("aria-expanded", "true");
    this.window.append(this.menu);
    (this.menu.querySelector("button.row:not([disabled])") ?? this.menu).focus();
    // A press outside the menu closes it.
    this.menuOutside = (e) => {
      if (!this.menu.contains(e.target) && !anchor.contains(e.target)) this.closeMenu(false);
    };
    document.addEventListener("pointerdown", this.menuOutside, true);
  }

  closeMenu(refocus) {
    if (!this.menu) return;
    this.menu.remove();
    this.menu = null;
    document.removeEventListener("pointerdown", this.menuOutside, true);
    const anchor = this.menuAnchor;
    this.menuAnchor = null;
    anchor.setAttribute("aria-expanded", "false");
    if (refocus && anchor.isConnected) anchor.focus();
  }

  // ---- The alert (design-guide.md §5.11): Cancel, Discard, Save; Save is the default.

  /** Resolves to "cancel", "discard", or "save". */
  ask({ heading, body }) {
    return new Promise((resolve) => {
      const before = document.activeElement;
      const done = (choice) => {
        this.scrim.remove();
        this.scrim = null;
        before?.focus?.();
        resolve(choice);
      };
      const button = (label, choice, kind = "") =>
        h("button", { class: `btn ${kind}`, onclick: () => done(choice) }, label);
      const save = button("Save", "save", "primary");
      const dialog = h("div", { class: "dialog", role: "alertdialog", "aria-modal": "true", "aria-labelledby": "alert-heading",
        "aria-describedby": "alert-body" },
        h("div", { class: "body" }, h("h2", { id: "alert-heading" }, heading), h("p", { id: "alert-body" }, body)),
        h("div", { class: "actions" }, button("Cancel", "cancel"), button("Discard", "discard", "destructive"), save));
      this.scrim = h("div", { class: "scrim" }, dialog);
      // Focus stays in the alert; Escape cancels; a press outside it cancels (lessons.md).
      this.scrim.addEventListener("keydown", (e) => {
        if (e.key === "Escape") {
          e.preventDefault();
          done("cancel");
        }
        if (e.key === "Tab") {
          const buttons = [...dialog.querySelectorAll("button")];
          const i = buttons.indexOf(document.activeElement);
          e.preventDefault();
          buttons[(i + (e.shiftKey ? -1 : 1) + buttons.length) % buttons.length].focus();
        }
      });
      this.scrim.addEventListener("pointerdown", (e) => {
        if (e.target === this.scrim) done("cancel");
      });
      this.window.append(this.scrim);
      save.focus();
    });
  }
}

/** A console error's message without the place it starts with: "loops.lisp:82:44: x" is "x". */
export function messageOf(entry) {
  const place = `${where(entry.location)}: `;
  return entry.text.startsWith(place) ? entry.text.slice(place.length) : entry.text;
}

/** The server's name without its version: "espresso-server". */
function serverName(s) {
  return s.server.split(" ")[0] || "server";
}

/** The busiest thread's load, from 0 to 1 (`interpreterLink.h`, busiest). */
function busiest(s) {
  return Math.max(s.load, ...(s.threadLoads ?? []));
}

/** The tempo as the status bar shows it, to one decimal place: "120 BPM". */
export function tempoText(s) {
  if (!s.connected || s.bpm === null) return "— BPM";
  return `${Number(s.bpm.toFixed(1))} BPM`;
}

/** The beat position as bar.beat, both from 1, or "—" while no plan plays (`interpreterLink.h`, beatText). */
export function beatText(s) {
  if (!s.connected || s.plan === 0 || s.beat === null) return "—";
  const perBar = s.beatsPerBar > 0 ? s.beatsPerBar : 4;
  const beats = Math.floor(s.beat);
  return `${Math.floor(beats / perBar) + 1}.${(beats % perBar) + 1}`;
}

const app = new App(document.getElementById("app"));
window.espressoEditor = app;  // for the tests
app.start().catch((error) => app.addLog({ kind: "error", text: `The editor could not start: ${error.message ?? error}` }));
