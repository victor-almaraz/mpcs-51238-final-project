// The GUI editor's window (tauri-editor.md §5.2): layout B in Carbon
// (brand/gui/design-guide.md §4–5, patterns.md §1–13). The top bar with
// Play, Update, and Stop, the navigator, tabs over the editor, the panel
// with Console and Problems, the status bar, and the banner for a missing
// server, over the workspace (workspace.js) and the sidecar (sidecar.js).
//
// Keys come from the chosen key scheme (schemes.js), which binds the
// commands below by their TUI names. Go to File and Show All Commands are
// one picker (picker.js); find is CodeMirror's search under its overlay
// (find.js).

import { h, isMac, keyCaps, rove, roving } from "./dom.js";
import { icon } from "./icons.js";
import { Sidecar } from "./sidecar.js";
import { Workspace, baseName, projectName } from "./workspace.js";
import {
  editorState, EditorView, markEvaluated, nextDiagnostic, offsetOf, previousDiagnostic, showProblems, useScheme,
  useWrap, where,
} from "./codeEditor.js";
import { formAt, formBefore, formNames, tokenize } from "./lisp.js";
import { foldAll, foldCode, foldable, foldedRanges, unfoldAll, unfoldCode } from "@codemirror/language";
import { Picker, fuzzy } from "./picker.js";
import { completion, definitions } from "./complete.js";
import { commandNamed, commandTable, menuOrder } from "./commands.js";
import { MenuBar, acceleratorOf, menuModel, withAccessKeys } from "./menubar.js";
import * as editing from "./editing.js";
import { SearchView } from "./search.js";
import { markedLines, marksField, nextMark, previousMark, restoredMarks, savedMarks, setMarks, toggleMark } from "./marks.js";
import { findNext, findPrevious, openFind } from "./find.js";
import {
  bindingLabel, bindingLabels, bindingOf, chordTable, eventChord, schemeNames, schemes, setCommandRunner, watchVimMode,
} from "./schemes.js";

const layoutKey = "espresso.layout";
const projectKey = "espresso.project";
const recentProjectsKey = "espresso.recentProjects";  // the folders opened, the last first (Open Recent…)
const runKey = "espresso.run";
const preferencesKey = "espresso.preferences";  // word wrap and the code's size (patterns.md §23.5)
const codeSizes = { least: 8, most: 32, normal: 14 };
const schemeKey = "espresso.keys";
const recentKey = "espresso.recent";  // the recently opened files, by project
const marksKey = "espresso.marks";  // the marked lines, by project and file (patterns.md §21.7)
const pickerRows = 50;
const dragThreshold = 6;  // px a tab moves before it is dragged and not clicked
const tearDistance = 40;  // px beyond the tab strip a dragged page tab goes to leave the window
const logLimit = 1000;
const markMs = 600;  // how long an evaluated form stays marked
const toastMs = 5000;  // how long a toast shows: libadwaita's AdwToast default

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
  /**
   * Whether the editor runs its own server (editor-refactor.md §4h.2): true
   * once it starts one, false when another server already had the port.
   */
  ownServer = null;
  /** How the editor's server ended, {code, line}, until it runs again. */
  serverStopped = null;
  /** Console entries: {kind, text, location?}. */
  log = [];
  /** The errors with a place since the last Play: {path, line, column, message}. */
  problems = [];
  #problemsVersion = 0;
  layout = { side: true, panel: true };
  run = { fade: 0.05, quantize: "none", updateOnSave: false };
  preferences = { wrap: false, codeSize: codeSizes.normal, interfaceSize: 1 };
  fullscreen = false;
  panelView = "console";
  maximized = false;
  filter = "";
  /** The key scheme (schemes.js): "cua" by default (design-guide.md §8.3). */
  scheme = "cua";
  /** Vim's mode, under the Vim scheme. */
  vimMode = "normal";
  /** The project's files most recently opened, newest first. */
  recent = [];
  /** The sidecar's `hello`, for About. */
  hello = null;
  /** The sidecar's `language`, for completion, and the standard library's definitions. */
  language = null;
  #libraryDefinitions = [];
  #definitionCache = new WeakMap();
  /**
   * The piece's page (patterns.md §18): its port while a plan with a page
   * plays, whether its tab is open and current, and whether it has opened by
   * itself in this project.
   */
  page = { port: null, open: false, current: false, opened: false };
  #chords;
  #treeFocus = 0;

  constructor(root) {
    this.root = root;
    for (const [key, into] of [[layoutKey, this.layout], [runKey, this.run], [preferencesKey, this.preferences]]) {
      try {
        Object.assign(into, JSON.parse(localStorage.getItem(key) ?? "{}"));
      } catch {
        // A damaged entry keeps the defaults.
      }
    }
    const saved = localStorage.getItem(schemeKey);
    if (schemes[saved]) this.scheme = saved;
    this.#chords = chordTable(this.scheme);
    setCommandRunner((name) => this.runCommand(name));
    this.#build();
    window.addEventListener("keydown", (e) => this.#onKey(e), true);
    window.addEventListener("focus", () => this.#checkDisk());
    window.addEventListener("resize", () => this.#fitPanel());
    this.narrow.addEventListener("change", () => {
      this.sideOpen = false;
      this.renderLayout();
    });
    // The secondary click opens the editor's own menu for what it is on; elsewhere, outside a field,
    // the webview's own menu does not open (patterns.md §20.7).
    document.addEventListener("contextmenu", (e) => {
      if (e.target.closest?.("input, textarea")) return;
      e.preventDefault();
      if (this.menu && this.menuIsContext) return;  // opened by its key a moment ago
      this.#contextFor(e.target, { x: e.clientX, y: e.clientY });
    });
  }

  // ---- The skeleton, built once (§1).

  #build() {
    const iconButton = (name, label, onclick) =>
      h("button", { class: "btn flat icon-only", title: label, "aria-label": label, onclick }, icon(name));

    this.playLabel = h("span", { class: "label" }, "Play");
    this.playButton = h("button", { class: "btn primary", onclick: () => this.play() },
      icon("play"), this.playLabel);
    this.optionsButton = h("button", { class: "btn primary arrow", title: "Play Options", "aria-label": "Play Options",
      "aria-haspopup": "menu", onclick: () => this.#toggleMenu(this.optionsButton, () => this.#optionsMenu()) },
    icon("down"));
    this.stopButton = h("button", { class: "btn flat", onclick: () => this.stop() },
      icon("stop"), h("span", { class: "label" }, "Stop"));
    // The piece's page, while it serves one: its tab opened again, or brought to the front.
    this.pageButton = h("button", { class: "btn flat", hidden: true, "aria-label": "Show Page", onclick: () => this.showPage() },
      icon("globe"), h("span", { class: "label" }, "Page"));
    const top = h("header", { class: "top" },
      h("div", { class: "start" },
        h("div", { class: "split", role: "group", "aria-label": "Play" }, this.playButton, this.optionsButton),
        this.stopButton, this.pageButton),
      this.commandCenter = h("button", { class: "command-center", onclick: () => this.runCommand("switchBuffer") }),
      h("div", { class: "end" },
        this.sideToggle = iconButton("sidebar", "Side Bar", () => this.toggle("side")),
        this.panelToggle = iconButton("panel", "Panel", () => this.toggle("panel"))));

    this.banner = h("div", { class: "banner", role: "status", hidden: true });

    this.sideTitle = h("div", { class: "title" });
    this.tree = h("div", { class: "tree", role: "tree", "aria-label": "Project Files" });
    this.side = h("nav", { class: "side", "aria-label": "Project" }, this.sideTitle, this.tree);

    this.tabs = h("div", { class: "tabs", role: "tablist", "aria-label": "Open Files" });
    // A wheel scrolls the strip across, as a mouse has no horizontal wheel.
    this.tabs.addEventListener("wheel", (e) => {
      if (Math.abs(e.deltaY) > Math.abs(e.deltaX)) {
        this.tabs.scrollLeft += e.deltaY;
        e.preventDefault();
      }
    }, { passive: false });
    this.editorHost = h("div", { class: "editor" });
    this.view = new EditorView({ parent: this.editorHost });
    this.empty = h("div", { class: "empty" });
    this.editorHost.append(this.empty);
    // §18: the page's bar over its frame, or its empty state.
    this.pageAddress = h("span", { class: "address" });
    const pageButton = (name, label, onclick) =>
      h("button", { class: "btn flat icon-only", title: label, "aria-label": label, onclick }, icon(name));
    this.pageFrame = h("iframe", { class: "frame", sandbox: "allow-scripts allow-same-origin allow-forms",
      title: "The Piece's Page" });
    this.pageEmpty = h("div", { class: "page-empty" },
      h("h1", {}, "No Page Playing"),
      h("p", {}, "A piece's page shows here while it plays. A project names its page with :page-port in project.lisp."));
    this.pageView = h("div", { class: "page", hidden: true },
      h("div", { class: "bar" },
        pageButton("reload", "Reload", () => this.reloadPage()),
        this.pageAddress,
        h("span", { class: "grow" }),
        pageButton("window", "Open in Window", () => this.openPageWindow()),
        pageButton("external", "Open in Browser", () => this.openPageInBrowser())),
      this.pageFrame, this.pageEmpty);
    this.editorHost.append(this.pageView);

    // §7: Console and Problems; the filter applies to the console.
    this.logView = h("div", { class: "log", role: "log", "aria-label": "Console", id: "panel-console" });
    this.problemsView = h("div", { class: "log", role: "listbox", "aria-label": "Problems", id: "panel-problems" });
    roving(this.problemsView, ".problem", (row) => row.click());
    this.search = new SearchView({ texts: () => this.#projectTexts(), goTo: (location) => this.goTo(location) });
    this.problemsBadge = h("span", { class: "badge", hidden: true });
    // The panel's views as tabs (the APG's tabs): one Tab stop, the arrows between them.
    const viewTab = (name, label, ...extra) => h("button", {
      class: "view", role: "tab", "data-view": name, id: `panel-tab-${name}`, "aria-controls": `panel-${name}`,
      onclick: () => this.showPanelView(name),
      onkeydown: (e) => {
        const names = this.panelTabs.map((t) => t.dataset.view);
        const i = names.indexOf(name);
        const to = { ArrowRight: i + 1, ArrowLeft: i - 1, Home: 0, End: names.length - 1 }[e.key];
        if (to === undefined) return;
        e.preventDefault();
        const next = names[(to + names.length) % names.length];
        this.showPanelView(next);
        this.panelTabs[names.indexOf(next)].focus();
      },
    }, h("span", { class: "label", "data-label": label }, label), ...extra);
    this.panelTabs = [viewTab("console", "Console"), viewTab("problems", "Problems", this.problemsBadge),
      viewTab("search", "Search")];
    this.filterField = h("input", {
      class: "filter", type: "search", placeholder: "Filter messages", "aria-label": "Filter Messages",
      oninput: () => {
        this.filter = this.filterField.value;
        this.renderLog();
      },
    });
    this.maxButton = iconButton("max", "Maximize Panel", () => this.toggleMaximized());
    this.splitter = this.#makeSplitter();
    this.panel = h("section", { class: "panel", "aria-label": "Panel" }, this.splitter,
      h("div", { class: "bar" },
        h("div", { class: "views", role: "tablist", "aria-label": "Panel" }, this.panelTabs),
        h("div", { class: "grow" }),
        h("div", { class: "tools" },
          this.filterField,
          this.clearButton = iconButton("clear", "Clear Console", () => this.clearLog()),
          this.maxButton,
          this.panelClose = iconButton("close", "Close Panel", () => this.toggle("panel")))),
      this.logView, this.problemsView, this.search.element);

    this.content = h("div", { class: "content", role: "main" }, this.tabs, this.editorHost, this.panel);
    this.mainArea = h("div", { class: "main" }, this.side, this.content);
    this.statusBar = h("footer", { class: "status", "aria-label": "Status" });
    // One polite live region for what changes without focus moving: toasts, the play state, counts.
    this.liveRegion = h("div", { class: "sr-only", role: "status", "aria-live": "polite" });
    this.window = h("div", { class: "window" }, top, this.banner, this.mainArea, this.statusBar, this.liveRegion);
    this.root.append(this.window);
    this.picker = new Picker(this.window, (cancelled) => this.#restoreFocus(cancelled));
  }

  // After the picker: focus goes back where it was at once when it was cancelled, and after the
  // chosen row has run when that row left it nowhere.
  #restoreFocus(cancelled) {
    const before = this.focusBefore;
    this.focusBefore = null;
    const back = () => {
      const lost = !document.activeElement || document.activeElement === document.body;
      if (lost && before?.isConnected) before.focus();
    };
    if (cancelled) back();
    else queueMicrotask(back);
  }

  // ---- Starting.

  async start() {
    this.workspace = new Workspace(
      { request: (method, params) => this.sidecar.request(method, params) },
      (text, path) => editorState(text, path, this.scheme, (state) => this.#edited(state), {
        extras: completion(() => this.language, (state) => this.#otherDefinitions(state)),
        marks: this.#keptMarks()[path], wrap: this.preferences.wrap,
      }));
    this.#applyCodeSize();
    this.#applyInterfaceSize();
    this.workspace.addEventListener("change", (e) => this.#changed(e.detail));
    this.renderAll();
    if (!window.__TAURI__) {
      this.addLog({ kind: "error", text: "Not running in the editor: open it with cargo tauri dev" });
      return;
    }
    this.#guardClose();
    // The editor's server starts first, so the client's link finds it listening.
    const events = window.__TAURI__.event;
    await events?.listen("server-line", ({ payload }) => this.addLog({ kind: "server", text: payload.text }));
    await events?.listen("server-exit", ({ payload }) => this.#serverEnded(payload));
    // The project named on the command line (espresso PROJECT) comes before the one open last.
    const launched = await window.__TAURI__.core?.invoke("launch_project").catch(() => null);
    const saved = launched ?? localStorage.getItem(projectKey);
    await this.#startServer(saved);
    try {
      this.sidecar = await Sidecar.start();
    } catch (error) {
      this.#failed(`The client could not start: ${error.message ?? error}`, "The client could not start");
      return;
    }
    this.sidecar.on("status", (s) => {
      this.status = s;
      this.serverReported = true;
      if (s.connected) {
        this.wasConnected = true;
        this.serverStopped = null;
      }
      this.#pageChanged(s.connected ? s.page ?? null : null);
      this.renderServer();
    });
    this.sidecar.on("console", (entry) => this.addLog(entry));
    this.sidecar.on("close", (reason) => {
      this.sidecar = null;
      this.#failed(`The client stopped: ${reason}`, "The client stopped");
      this.renderServer();
    });
    const hello = await this.sidecar.request("hello");
    this.hello = hello;
    this.#learnLanguage();
    this.serverAddress = hello.server ?? "";
    this.renderBanner();
    if (saved) await this.openProject(saved, { quiet: !launched });
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
    await this.#commitTrash();
    // Another project stops the piece (the server takes the new root); the same one again does not.
    const same = this.workspace.root !== null && root.replace(/\/+$/, "") === this.workspace.root;
    if (!same && !(await this.#stopForProject(`Opening “${projectName(root)}”`))) return;
    try {
      await this.workspace.openProject(root);
      localStorage.setItem(projectKey, root);
      this.#noteRecentProject(this.workspace.root);
      this.page.opened = false;
      if (this.ownServer) await this.#startServer(this.workspace.root);  // its samples resolve against the project
      this.problems = [];
      this.#problemsChanged();
      // A folder the user opens shows its files, the side bar opened for them; one restored at start keeps the layout.
      if (!quiet) {
        this.#showSide();
        (this.tree.querySelector('[tabindex="0"]') ?? this.tree.querySelector(".row"))?.focus();
      }
    } catch (error) {
      if (quiet) localStorage.removeItem(projectKey);
      this.#failed(`Could not open ${root}: ${error.message}`, `Could not open “${projectName(root)}”`);
    }
  }

  async openFile(path) {
    try {
      if (this.workspace.current?.path !== path) this.#remember();
      await this.workspace.openFile(path);
      if (this.narrow.matches && this.sideOpen) {  // the overlay gives way to the file it opened
        this.sideOpen = false;
        this.renderLayout();
      }
      if (this.page.current) this.showDocument(this.workspace.currentId);  // the file in place of the page
      this.view.focus();
      return true;
    } catch (error) {
      this.#failed(`Could not open ${path}: ${error.message}`, `Could not open “${baseName(path)}”`);
      return false;
    }
  }

  // ---- Files (patterns.md §20.7, §23.5): new, rename, move to trash, copy a path, reveal.

  // The file a command acts on: the one named, else the tree's focused row while the tree has focus, else the current file.
  #withFile(path, act) {
    const focused = this.tree.contains(document.activeElement) ? document.activeElement.closest(".row")?.title : null;
    const target = path ?? focused ?? this.workspace.current?.path;
    if (target) return act(target);
    return undefined;
  }

  /** Asks for a new file's path, in `dir` when given, creates it, and opens it. */
  newFile(dir = "") {
    if (this.workspace.root === null) return;
    this.#askName({
      heading: "New File", value: dir ? `${dir}/` : "", button: "Create",
      submit: async (path) => {
        const error = await this.#fileRequest("file.create", { path });
        if (error) return error;
        await this.workspace.refreshFiles();
        await this.openFile(path);
        return null;
      },
    });
  }

  /** Asks for a file's new path and renames it, its open tab with it. */
  renameFile(path) {
    const slash = path.lastIndexOf("/");
    this.#askName({
      heading: `Rename “${baseName(path)}”`, value: path, select: [slash + 1, path.lastIndexOf(".") > slash ? path.lastIndexOf(".") : path.length],
      button: "Rename",
      submit: async (to) => {
        if (to === path) return null;
        const error = await this.#fileRequest("file.rename", { from: path, to });
        if (error) return error;
        this.workspace.renamed(path, to);
        this.#pathMoved(path, to);
        await this.workspace.refreshFiles();
        return null;
      },
    });
  }

  // A renamed file takes what the window keeps by its path along: its marks, history, place among the
  // recently opened and the closed, and its problems.
  #pathMoved(from, to) {
    const move = (p) => (p === from ? to : p);
    this.recent = this.recent.map(move);
    this.#closed = this.#closed.map(move);
    for (const place of [...this.#history.back, ...this.#history.forward]) place.path = move(place.path);
    for (const problem of this.problems) problem.path = move(problem.path);
    try {
      const all = JSON.parse(localStorage.getItem(marksKey) ?? "{}");
      const project = all[this.workspace.root];
      if (project?.[from]) {
        project[to] = project[from];
        delete project[from];
        localStorage.setItem(marksKey, JSON.stringify(all));
      }
    } catch {
      // Damaged storage keeps nothing.
    }
    this.renderProblems();
  }

  // A file request's error message, or null when it succeeded.
  async #fileRequest(method, params) {
    try {
      await this.sidecar.request(method, params);
      return null;
    } catch (error) {
      return error.message ?? String(error);
    }
  }

  #trashing = null;  // the file moved to the trash, until its toast goes: {path, root}

  /**
   * Moves `path` to the trash: at once to the eye, and with Undo while the
   * toast shows (the HIG: undo over confirmation); the move itself happens
   * when the toast goes, another file moves, the project changes, or the
   * window closes. An open file closes first, asking as Close does. Focus
   * goes to the row that takes the file's place.
   */
  async trashFile(path) {
    await this.#commitTrash();
    const open = this.workspace.documentOf(path);
    if (open && !(await this.close(open.id))) return;
    const row = this.workspace.files.indexOf(path);
    const inTree = this.tree.contains(document.activeElement);
    this.#trashing = { path, root: this.workspace.root };
    this.workspace.hideFile(path);
    this.#toast(`“${baseName(path)}” moved to trash`, {
      action: "Undo", onAction: () => this.#undoTrash(), onExpire: () => this.#commitTrash(),
    });
    if (inTree && this.workspace.files.length) {
      this.#treeFocus = Math.min(Math.max(row, 0), this.workspace.files.length - 1);
      this.renderSide();
      this.tree.children[this.#treeFocus]?.focus();
    }
  }

  async #commitTrash() {
    const pending = this.#trashing;
    if (!pending) return;
    this.#trashing = null;
    this.#hideToast();
    try {
      await window.__TAURI__?.core?.invoke("file_trash", { root: pending.root, path: pending.path });
    } catch (error) {
      this.#failed(`Could not move ${pending.path} to the trash: ${error.message ?? error}`,
        `Could not move “${baseName(pending.path)}” to the trash`);
    }
    if (this.workspace.root === pending.root) await this.workspace.refreshFiles();
  }

  #undoTrash() {
    const pending = this.#trashing;
    if (!pending) return;
    this.#trashing = null;
    this.#hideToast();
    if (this.workspace.root === pending.root) this.workspace.refreshFiles();
  }

  #copyText(text) {
    navigator.clipboard.writeText(text).catch((error) =>
      this.#failed(`Could not copy: ${error.message ?? error}`, "Could not copy"));
  }

  #reveal(path) {
    window.__TAURI__?.opener?.revealItemInDir(`${this.workspace.root}/${path}`).catch((error) =>
      this.#failed(`Could not show ${path} in the file manager: ${error.message ?? error}`,
        "Could not show the file in the file manager"));
  }

  /** Shows the side bar with `path`'s row focused. */
  revealInSideBar(path) {
    this.#showSide();
    const i = this.workspace.files.indexOf(path);
    if (i < 0) return;
    this.#treeFocus = i;
    this.renderSide();
    const row = this.tree.children[i];
    row?.focus();
    row?.scrollIntoView({ block: "nearest" });
  }

  // ---- The name popover (GNOME Builder's, patterns.md §20.6): a heading, the field, an inline error, one action.

  #askName({ heading, value, select, button, submit }) {
    this.#closeAsk();
    // Where focus goes back to: what had it, or the code when that leaves (the APG's dialog).
    const before = document.activeElement;
    const beforeRow = before?.closest?.(".tree .row")?.title;  // the tree is drawn again meanwhile: its row by path
    const back = () => {
      const row = beforeRow && [...this.tree.querySelectorAll(".row")].find((r) => r.title === beforeRow);
      if (row) row.focus();
      else if (before?.isConnected && before !== document.body) before.focus();
      else this.#focusContent();
    };
    const errorId = `ask-error-${this.#menuIds++}`;
    const field = h("input", { class: "field", value, "aria-label": heading, spellcheck: "false",
      "aria-describedby": errorId });
    const error = h("div", { class: "error", id: errorId, role: "alert" });
    const action = h("button", { class: "btn primary" }, button);
    const fail = (message) => {
      error.textContent = message;
      field.setAttribute("aria-invalid", "true");
      field.focus();
    };
    const go = async () => {
      const name = field.value.trim();
      if (!name || name.endsWith("/")) {
        fail("Type a name for the file");
        return;
      }
      action.disabled = true;
      const problem = await submit(name);
      action.disabled = false;
      if (problem) {
        fail(problem);
        return;
      }
      this.#closeAsk();
      // On success, focus stays where the work went: the file opened, else what had it before.
      if (!this.view.contentDOM.contains(document.activeElement)) back();
    };
    action.onclick = go;
    const box = h("div", { class: "popover", role: "dialog", "aria-modal": "true", "aria-label": heading },
      h("h2", {}, heading), field, error, h("div", { class: "actions" }, action));
    box.addEventListener("keydown", (e) => {
      if (e.key === "Enter") {
        e.preventDefault();
        go();
      } else if (e.key === "Escape") {
        e.preventDefault();
        e.stopPropagation();
        this.#closeAsk();
        back();
      } else if (e.key === "Tab") {  // focus stays in the popover: the field, then its action
        e.preventDefault();
        (document.activeElement === field ? action : field).focus();
      }
    });
    field.addEventListener("input", () => {
      error.textContent = "";
      field.removeAttribute("aria-invalid");
    });
    this.ask_ = box;
    this.window.append(box);
    field.focus();
    const [from, to] = select ?? [value.length, value.length];
    field.setSelectionRange(from, to);
  }

  #closeAsk() {
    this.ask_?.remove();
    this.ask_ = null;
  }

  // ---- The toast (the HIG's: one at a time, at the window's foot, with at most one action; dismissible).

  #toastNow = null;  // {element, timer, remaining, started, onExpire}

  /**
   * Shows `message` for `timeout` ms, with an `action` button when given.
   * The time stops while the pointer is over the toast or it has focus; a
   * newer toast, the close button, or Escape ends it, as its time does,
   * calling `onExpire`.
   */
  #toast(message, { action = null, onAction = () => {}, onExpire = () => {}, timeout = toastMs } = {}) {
    this.#endToast();
    const close = h("button", { class: "btn flat icon-only", title: "Close", "aria-label": "Close",
      onclick: () => this.#endToast() }, icon("close"));
    const actionButton = action ? h("button", { class: "btn flat action", onclick: () => {
      this.#hideToast();
      onAction();
    } }, action) : null;
    const element = h("div", { class: "toast", role: "group", "aria-label": "Notice" },
      h("span", { class: "message" }, message), actionButton, close);
    const toast = { element, timer: null, remaining: timeout, started: 0, onExpire };
    const run = () => {
      toast.started = performance.now();
      toast.timer = setTimeout(() => this.#endToast(toast), toast.remaining);
    };
    const pause = () => {
      if (toast.timer === null) return;
      clearTimeout(toast.timer);
      toast.timer = null;
      toast.remaining = Math.max(1000, toast.remaining - (performance.now() - toast.started));
    };
    element.addEventListener("pointerenter", pause);
    element.addEventListener("focusin", pause);
    element.addEventListener("pointerleave", () => !element.contains(document.activeElement) && run());
    element.addEventListener("focusout", (e) => !element.contains(e.relatedTarget) && run());
    element.addEventListener("keydown", (e) => {
      if (e.key === "Escape") {
        e.preventDefault();
        e.stopPropagation();
        this.#endToast(toast);
        this.#focusContent();
      }
    });
    this.#toastNow = toast;
    this.window.append(element);
    // Centered on a whole pixel: a whole width with the window's parity (design-guide.md §4.4).
    const room = this.window.clientWidth;
    let width = Math.ceil(element.getBoundingClientRect().width);
    if ((room - width) % 2) width += 1;
    element.style.width = `${width}px`;
    this.announce(message);
    run();
  }

  // The toast's end: its time, a newer one, Close, or Escape. Its onExpire runs once.
  #endToast(toast = this.#toastNow) {
    if (!toast || toast !== this.#toastNow) return;
    this.#hideToast();
    toast.onExpire();
  }

  // The toast leaves without its onExpire: its action ran.
  #hideToast() {
    const toast = this.#toastNow;
    if (!toast) return;
    clearTimeout(toast.timer);
    toast.element.remove();
    this.#toastNow = null;
  }

  /** A short notice for a command that could not do what was asked (VS Code's notification; the HIG's toast). */
  notify(message, options = {}) {
    this.#toast(message, options);
  }

  // What the window could not do: in full in the console, and as a notice where the user looks, which
  // offers the console while it is out of sight (patterns.md §12).
  #failed(text, notice = text) {
    this.addLog({ kind: "error", text });
    const seen = this.layout.panel && this.panelView === "console";
    this.notify(notice, seen ? {} : { action: "Show Console", onAction: () => this.showPanelView("console") });
  }

  // Focus to the window's content: the code, the page, or the empty state's first control, as when the part
  // that had it hides.
  #focusContent() {
    if (this.page.open && this.page.current) this.tabs.querySelector(".page-tab")?.focus();
    else if (this.workspace.current) this.view.focus();
    else (this.empty.querySelector("button") ?? this.commandCenter).focus();
  }

  /** Says `message` to assistive technology, through the one polite live region the window keeps. */
  announce(message) {
    // Emptied, then filled a moment later, so that a repeated message is announced again.
    this.liveRegion.textContent = "";
    setTimeout(() => { this.liveRegion.textContent = message; }, 50);
  }

  // ---- Context menus (patterns.md §20.7): the editor's own, by the secondary click, Shift+F10, or the Menu key.

  // A menu row for each command, disabled when it cannot run; `target` names what it acts on.
  #rows(names, target) {
    return names.map((name) => (name === "|" ? "separator"
      : { label: commandNamed(name).label, command: name, target, enabled: this.commandEnabled(name) }));
  }

  #contextMenu(at, anchor, label, items) {
    if (!items.length) return;
    this.closeMenu(false);
    this.#openMenu(anchor, { label, place: `left: ${Math.round(at.x)}px; top: ${Math.round(at.y)}px`, items },
      { context: true });
    // Inside the window: a menu near the right or bottom edge opens toward the inside.
    const box = this.menu.getBoundingClientRect();
    const room = this.window.getBoundingClientRect();
    if (box.right > room.right) this.menu.style.left = `${Math.max(0, Math.round(at.x - box.width))}px`;
    if (box.bottom > room.bottom) this.menu.style.top = `${Math.max(0, Math.round(at.y - box.height))}px`;
  }

  #codeMenu() {
    return this.#rows(["cut", "copy", "paste", "|", "undo", "redo", "|", "selectAll", "|", "toggleComment", "|",
      "evalForm", "evalLastForm", "|", "goToDefinition", "toggleMark", "|", "showAllCommands"]);
  }

  // The gutter's line: Toggle Mark; Fold where it folds, Unfold where it is folded.
  #gutterMenu(line) {
    const state = this.view.state;
    let folded = false;
    foldedRanges(state).between(line.from, line.to, (from) => {
      if (from >= line.from && from <= line.to) folded = true;
    });
    const at = (command) => () => {
      this.view.dispatch({ selection: { anchor: line.from } });
      this.runCommand(command);
    };
    const mark = { label: "Toggle Mark", command: "toggleMark", action: at("toggleMark") };
    if (folded) return [mark, "separator", { label: "Unfold", command: "unfold", action: at("unfold") }];
    if (foldable(state, line.from, line.to)) return [mark, "separator", { label: "Fold", command: "fold", action: at("fold") }];
    return [mark];
  }

  #fileMenu(path) {
    const dir = path.includes("/") ? path.slice(0, path.lastIndexOf("/")) : "";
    return [{ label: "Open", action: () => this.openFile(path) }, "separator",
      { label: "New File…", command: "newFile", target: dir, enabled: true }, "separator",
      ...this.#rows(["copyPath", "copyRelativePath", "openContainingFolder", "|", "renameFile", "trashFile"], path)
        .map((r) => (r === "separator" ? r : { ...r, enabled: true }))];
  }

  #tabMenu(doc) {
    return [...this.#rows(["copyPath", "copyRelativePath", "revealInSideBar"], doc.path)
      .map((r) => ({ ...r, enabled: true })), "separator",
    ...this.#rows(["closeOthers", "closeSaved", "closeAll", "reopenClosed"], doc.id), "separator",
    { label: "Close", command: "closeBuffer", action: () => this.close(doc.id) }];
  }

  #pageTabMenu() {
    return [{ label: "Reload", action: () => this.reloadPage() }, "separator",
      { label: "Open in Window", action: () => this.openPageWindow() },
      { label: "Open in Browser", action: () => this.openPageInBrowser() }, "separator",
      { label: "Close", action: () => this.closePage() }];
  }

  #consoleMenu() {
    const selected = String(window.getSelection() ?? "");
    return [{ label: "Copy", enabled: selected !== "", action: () => this.#copyText(selected) },
      { label: "Select All", action: () => window.getSelection()?.selectAllChildren(this.logView) }, "separator",
      ...this.#rows(["clearConsole"])];
  }

  #problemMenu(problem) {
    return [{ label: "Go to Problem", action: () => this.goTo(problem) },
      { label: "Copy Message", action: () => this.#copyText(problem.message) }];
  }

  // The context menu of what was clicked, or of what has focus for Shift+F10 and the Menu key; at the pointer,
  // or under the focused element (in the code, under the cursor).
  #contextFor(target, at) {
    const keyboard = at === null;
    const below = (el) => {
      const r = el.getBoundingClientRect();
      return { x: r.left, y: r.bottom };
    };
    if (this.view.dom.contains(target)) {
      if (target.closest(".cm-gutters")) {
        if (keyboard) return false;
        const block = this.view.lineBlockAtHeight(at.y - this.view.documentTop);
        this.#contextMenu(at, this.view.contentDOM, "Line", this.#gutterMenu(this.view.state.doc.lineAt(block.from)));
        return true;
      }
      let where = at;
      if (keyboard) {
        const c = this.view.coordsAtPos(this.view.state.selection.main.head);
        where = c ? { x: c.left, y: c.bottom } : below(this.view.contentDOM);
      }
      this.#contextMenu(where, this.view.contentDOM, "Code", this.#codeMenu());
      return true;
    }
    const row = target.closest?.(".tree .row");
    if (row) {
      this.#contextMenu(at ?? below(row), row, row.title, this.#fileMenu(row.title));
      return true;
    }
    if (this.tree.contains(target)) {
      this.#contextMenu(at ?? below(this.tree), this.tree, "Project", this.workspace.root === null ? []
        : [{ label: "New File…", command: "newFile", target: "", enabled: true }]);
      return true;
    }
    const tab = target.closest?.(".tab");
    if (tab) {
      if (tab.classList.contains("page-tab")) {
        this.#contextMenu(at ?? below(tab), tab, "Page", this.#pageTabMenu());
      } else {
        const doc = this.workspace.document(Number(tab.dataset.id));
        if (doc) this.#contextMenu(at ?? below(tab), tab, baseName(doc.path), this.#tabMenu(doc));
      }
      return true;
    }
    const problem = target.closest?.(".problem");
    if (problem && this.problemsView.contains(problem)) {
      const p = this.problems[[...this.problemsView.querySelectorAll(".problem")].indexOf(problem)];
      if (p) this.#contextMenu(at ?? below(problem), problem, "Problem", this.#problemMenu(p));
      return true;
    }
    if (this.logView.contains(target)) {
      this.#contextMenu(at ?? below(this.logView), this.logView, "Console", this.#consoleMenu());
      return true;
    }
    return false;
  }

  // ---- File (patterns.md §19.8): Open Recent, Save As, Save All, Reopen Closed Editor, Close Folder.

  #closed = [];  // the files closed, the last first

  #recentProjects() {
    try {
      const list = JSON.parse(localStorage.getItem(recentProjectsKey) ?? "[]");
      return Array.isArray(list) ? list.filter((r) => typeof r === "string") : [];
    } catch {
      return [];
    }
  }

  #noteRecentProject(root) {
    const list = [root, ...this.#recentProjects().filter((r) => r !== root)].slice(0, 10);
    localStorage.setItem(recentProjectsKey, JSON.stringify(list));
  }

  /** Open Recent…: the folders opened, the last first, in the picker; and Clear Recent Folders. */
  openRecent() {
    if (this.menu) this.closeMenu(false);
    if (!this.picker.isOpen) this.focusBefore = document.activeElement;
    this.picker.open({
      label: "Open Recent", value: "", placeholder: "Search recent folders",
      source: (query) => {
        const rows = [];
        for (const root of this.#recentProjects()) {
          const match = query ? fuzzy(query.trim(), root) : { score: 0, hits: [] };
          if (!match) continue;
          rows.push({ label: projectName(root), detail: root, score: match.score,
            run: async () => {
              if (root === this.workspace.root || !(await this.settleChanges())) return;
              await this.openProject(root);
            } });
        }
        if (query) rows.sort((a, b) => b.score - a.score);
        else if (rows.length) {
          rows.push({ label: "Clear Recent Folders", detail: "", run: () => localStorage.removeItem(recentProjectsKey) });
        }
        return { rows, empty: "No recent folders" };
      },
    });
  }

  /** Save As…: the document written to a new path in the project, its tab then that file's. */
  saveAs(id) {
    const doc = this.workspace.document(id);
    if (!doc) return;
    const slash = doc.path.lastIndexOf("/");
    const dot = doc.path.lastIndexOf(".");
    this.#askName({
      heading: "Save As", value: doc.path, select: [slash + 1, dot > slash ? dot : doc.path.length], button: "Save",
      submit: async (to) => {
        if (to === doc.path) return (await this.save(id)) ? null : "Could not save";
        if (this.workspace.files.includes(to)) return `A file or folder named “${baseName(to)}” already exists`;
        const written = doc.state.doc;
        const error = await this.#fileRequest("file.write", { path: to, text: written.toString() });
        if (error) return error;
        this.workspace.savedAs(id, to, written);
        await this.workspace.refreshFiles();
        return null;
      },
    });
  }

  /** Saves every file with unsaved changes; false when one could not be written. */
  async saveAll() {
    let saved = true;
    for (const doc of this.workspace.dirtyDocuments) {
      if (doc.reference) continue;
      saved = (await this.save(doc.id)) && saved;
    }
    return saved;
  }

  /** Reopens the file closed last that is still in the project. */
  async reopenClosed() {
    while (this.#closed.length) {
      const path = this.#closed.shift();
      if (this.workspace.files.includes(path)) return this.openFile(path);
    }
    return false;
  }

  // A piece plays and the project is about to change: ask before its sound stops, then stop it with its fade.
  async #stopForProject(what) {
    if (!this.playing || this.workspace.root === null) return true;
    const choice = await this.ask({
      heading: "Stop the Playing Piece?",
      body: `${what} stops “${this.workspace.name}”.`,
      responses: [["Cancel", "cancel"], ["Stop", "stop", "primary"]],
    });
    if (choice !== "stop") return false;
    await this.stop();
    return true;
  }

  /** Close Folder: the project closes, its unsaved changes settled first, its piece stopped, its page closed. */
  async closeFolder() {
    if (this.workspace.root === null || !(await this.settleChanges())) return;
    if (!(await this.#stopForProject("Closing the folder"))) return;
    await this.#commitTrash();
    if (this.page.open) this.closePage();
    this.workspace.closeProject();
    localStorage.removeItem(projectKey);
    this.#closed = [];
  }

  // The project's files for Find in Files: each open file as it is in the editor, the others as on disk.
  async #projectTexts() {
    const ws = this.workspace;
    const found = [];
    for (const path of ws.files) {
      const open = ws.documentOf(path);
      if (open) {
        found.push({ path, text: open.state.doc.toString() });
        continue;
      }
      try {
        const { text } = await this.sidecar.request("file.read", { path });
        found.push({ path, text });
      } catch {
        // A file that is not text is not searched.
      }
    }
    return found;
  }

  /** Find in Files…: the panel's Search view, with the selection in its field. */
  findInFiles(doc) {
    if (this.workspace.root === null) return;
    const selected = doc ? doc.state.sliceDoc(doc.state.selection.main.from, doc.state.selection.main.to) : "";
    this.showPanelView("search");
    this.search.focus(selected.includes("\n") ? "" : selected);
    if (selected && !selected.includes("\n")) this.search.run();
  }

  /**
   * Saves the document with `id`; false when nothing was written. A file
   * changed on disk since it was read asks whether to reload it or
   * overwrite it. Updates the plan after, when asked to.
   */
  async save(id, { overwrite = false } = {}) {
    const doc = this.workspace.document(id);
    if (!doc || doc.reference) return false;
    try {
      await this.workspace.save(id, { overwrite });
    } catch (error) {
      if (error.code === -32003) return this.#saveConflict(id);
      this.#failed(`Could not save ${doc.path}: ${error.message}`, `Could not save “${baseName(doc.path)}”`);
      return false;
    }
    if (this.run.updateOnSave && this.playing) this.play({ onSave: true });
    return true;
  }

  // The file changed on disk since it was read (patterns.md §13): keep the editor's version or the disk's.
  async #saveConflict(id) {
    const doc = this.workspace.document(id);
    if (!doc) return false;
    const choice = await this.ask({
      heading: `“${baseName(doc.path)}” Changed on Disk`,
      body: "Another program changed the file since it was opened. Overwrite replaces that version with yours; Reload discards your changes.",
      responses: [["Cancel", "cancel"], ["Reload", "reload", "destructive"], ["Overwrite", "overwrite", "primary"]],
    });
    if (choice === "overwrite") return this.save(id, { overwrite: true });
    if (choice === "reload") await this.workspace.reread(id, { force: true });
    return false;
  }

  #diskChecked = 0;

  // On the window's focus: the file list again, and each open file as it is on disk; a clean file takes the
  // disk's text, as gedit and GNOME Builder do.
  async #checkDisk() {
    if (this.workspace.root === null || performance.now() - this.#diskChecked < 1000) return;
    this.#diskChecked = performance.now();
    await this.workspace.refreshFiles().catch(() => {});
    for (const doc of [...this.workspace.documents]) {
      const result = await this.workspace.reread(doc.id);
      if (result === "missing" && !doc.missing) {
        doc.missing = true;
        this.notify(`“${baseName(doc.path)}” was deleted on disk`);
      } else if (result === "changed" && !doc.changedOnDisk) {
        doc.changedOnDisk = true;
        this.notify(`“${baseName(doc.path)}” changed on disk; saving asks which to keep`);
      }
    }
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
      if (choice === "cancel") return false;
      if (choice === "save" && !(await this.save(id))) return false;
    }
    if (!doc.reference) this.#closed = [doc.path, ...this.#closed.filter((p) => p !== doc.path)].slice(0, 20);
    const hadFocus = this.view.contentDOM.contains(document.activeElement);
    this.workspace.close(id);
    if (hadFocus && !this.workspace.current) this.#focusContent();  // the last tab closed: the empty state
    return true;
  }

  /**
   * Closes the open files, each asking as Close does, and stops at a
   * Cancel: all of them, all but the one `except` names, or the saved ones.
   */
  async closeAll({ except = null, saved = false } = {}) {
    const ws = this.workspace;
    const closing = ws.documents.filter((doc) => doc.id !== except && !(saved && ws.isDirty(doc)));
    // One question for all their changes, as Quit asks it.
    if (!(await this.settleChanges(closing.filter((doc) => ws.isDirty(doc))))) return false;
    for (const doc of closing) {
      if (!doc.reference) this.#closed = [doc.path, ...this.#closed.filter((p) => p !== doc.path)].slice(0, 20);
      ws.close(doc.id);
    }
    return true;
  }

  /**
   * Before the documents close together (a new project, the window):
   * asks once about every unsaved one; false when the user cancels or a
   * save fails.
   */
  async settleChanges(dirty = this.workspace.dirtyDocuments) {
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
      this.#failed(`${method}: ${error.message}`, error.message);
      return null;
    }
  }

  /** Compiles the project, with the editor's buffers, and plays it or updates the plan that plays. */
  /** Plays the project's entry, or updates the playing piece; `onSave`, for Update on Save, leaves the page be. */
  async play({ onSave = false } = {}) {
    if (this.workspace.root === null) return;
    if (!this.connected) {
      this.notify("Not connected to espresso-server");
      return;
    }
    const updating = this.playing;
    this.problems = [];
    this.#problemsChanged();
    const result = await this.#request("run.play", {
      sources: this.#sources(), file: this.workspace.entry ?? this.workspace.current?.path, fade: this.run.fade,
      quantize: this.run.quantize,
    });
    // A failed Play or Update says so where the user looks, not only in a hidden console (patterns.md §12).
    if (result?.errors > 0) {
      const n = `${result.errors} error${result.errors === 1 ? "" : "s"}`;
      const message = updating ? `Not updated: ${n}; the playing plan goes on` : `Not played: ${n}`;
      const seen = this.layout.panel && this.panelView === "problems";
      this.notify(message, seen ? {} : { action: "Show Problems", onAction: () => this.showPanelView("problems") });
    } else if (result) {
      this.announce(updating ? "Updated" : "Playing");
      if (!onSave) this.#pageAfterPlay();
    }
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
    if (!doc) return;
    if (!this.connected) {
      this.notify("Not connected to espresso-server");
      return;
    }
    const text = doc.state.doc.toString();
    const head = doc.state.selection.main.head;
    const range = which === "form" ? formAt(text, head) : formBefore(text, head);
    if (!range) {
      this.notify("No complete form at the cursor");
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

  /**
   * Starts the editor's server with the project `root`, or keeps it when it
   * runs with that root (main.rs, server_start). When another server has
   * the port, the editor uses that one and leaves it alone.
   */
  async #startServer(root) {
    const invoke = window.__TAURI__?.core?.invoke;
    if (!invoke || this.ownServer === false) return;
    let started;
    try {
      started = await invoke("server_start", { root: root ?? null });
    } catch (error) {
      this.#serverEnded({ code: null, line: String(error) });
      return;
    }
    if (started.state === "running") {
      this.ownServer = true;
      this.serverStopped = null;
    } else if (/cannot listen/.test(started.line)) {
      this.ownServer = false;
      this.addLog({ kind: "server", text: `server: another espresso-server has ${this.serverAddress || "the port"}; the editor uses it` });
    } else {
      this.#serverEnded(started);
      return;
    }
    this.renderServer();
  }

  // The editor's server ended without being asked: the banner says so until Retry starts it again.
  #serverEnded({ code, line }) {
    this.serverStopped = { code, line };
    const status = code === null || code === undefined ? "" : ` (status ${code})`;
    this.addLog({ kind: "error", text: `server: espresso-server stopped${status}${line ? `: ${line}` : ""}` });
    this.renderServer();
  }

  /** Starts the editor's server again when it stopped, and asks the link to reach it (the banner's Retry). */
  async retry() {
    if (!this.sidecar || this.connecting) return;
    this.connecting = true;
    this.renderBanner();
    if (this.ownServer !== false) await this.#startServer(this.workspace.root);
    await this.#request("server.connect", {});
    this.connecting = false;
    this.renderBanner();
  }

  #setRun(changes) {
    Object.assign(this.run, changes);
    localStorage.setItem(runKey, JSON.stringify(this.run));
    this.renderMenuBar();
  }

  // ---- The picker (design-guide.md §5.9, patterns.md §9.2): files; ">" commands, ":" a line, "@" a definition.

  /** Opens the picker with `value` in its field: "" for files, ">" for commands, ":" for a line. */
  openPicker(value) {
    if (this.menu) this.closeMenu(false);
    if (!this.picker.isOpen) this.focusBefore = document.activeElement;
    const label = value.startsWith(">") ? "Show All Commands" : value.startsWith(":") ? "Go to Line" : "Go to File";
    this.picker.open({
      label, value, source: (query) => this.#pickerRows(query),
      placeholder: "Search files by name (append : to go to line or @ to go to definition)",
    });
  }

  #pickerRows(query) {
    if (query.startsWith(">")) return this.#commandRows(query.slice(1).trim());
    if (query.startsWith(":")) return this.#lineRows(query.slice(1).trim());
    if (query.startsWith("@")) return this.#definitionRows(query.slice(1).trim());
    return this.#fileRows(query.trim());
  }

  // Files, the recently opened first; "name:12" opens a file at its line 12.
  #fileRows(query) {
    const ws = this.workspace;
    if (ws.root === null) return { rows: [], empty: "Open a folder to go to its files" };
    const [, name, line] = /^(.*?)(?::(\d+))?$/.exec(query);
    const where = (path) => {
      const slash = path.lastIndexOf("/");
      return slash < 0 ? ws.name : path.slice(0, slash);
    };
    const open = (path) => async () => {
      if (!(await this.openFile(path))) return;
      if (line) this.#goToLine(Number(line), 1);
    };
    const row = (path, hits, group) => ({
      icon: "file", label: baseName(path), detail: where(path), hits, group, run: open(path),
    });
    if (!name) {
      const recent = this.recent.filter((p) => ws.files.includes(p));
      const rest = ws.files.filter((p) => !recent.includes(p));
      return { rows: [...recent.map((p) => row(p, [], "Recently Opened")), ...rest.map((p) => row(p, [], "Files"))]
        .slice(0, pickerRows) };
    }
    const found = [];
    for (const path of ws.files) {
      const base = baseName(path);
      const onName = fuzzy(name, base);
      const onPath = onName ?? fuzzy(name, path);
      if (!onPath) continue;
      const recency = this.recent.indexOf(path);
      found.push({ path, hits: onName?.hits ?? [], score: onPath.score + (onName ? 10 : 0) + (recency < 0 ? 0 : 1) });
    }
    found.sort((a, b) => b.score - a.score || a.path.localeCompare(b.path));
    return { rows: found.slice(0, pickerRows).map((f) => row(f.path, f.hits)), empty: "No matching files" };
  }

  // The command table, each with its binding in the key scheme.
  #commandRows(query) {
    const rows = [];
    // Only what can run now, as VS Code's palette shows it.
    for (const { name, label } of commandTable.filter((c) => this.commandEnabled(c.name))) {
      const match = query ? fuzzy(query, label) : { score: 0, hits: [] };
      if (!match) continue;
      rows.push({ label, hits: match.hits, score: match.score, keys: keyCaps(this.label(name)),
        run: () => this.runCommand(name) });
    }
    if (query) rows.sort((a, b) => b.score - a.score);
    return { rows, empty: "No matching commands" };
  }

  // ":12" or ":12:5", a line and a column of the current file.
  #lineRows(query) {
    const doc = this.workspace.current;
    if (!doc) return { rows: [], empty: "Open a file to go to a line" };
    const lines = doc.state.doc.lines;
    const head = doc.state.selection.main.head;
    const at = doc.state.doc.lineAt(head);
    const [, line, column] = /^(\d*)(?::(\d+))?$/.exec(query) ?? [];
    const prompt = `Current line: ${at.number}, column ${head - at.from + 1}. Type a line number between 1 and ${lines}.`;
    if (!line) return { rows: [], empty: prompt };
    const n = Math.min(Math.max(Number(line), 1), lines);
    const c = column ? Number(column) : 1;
    return { rows: [{ label: column ? `Go to line ${n}, column ${c}` : `Go to line ${n}`, run: () => this.#goToLine(n, c) }] };
  }

  // The names the current file defines, from the highlighter's definition names (lisp.js).
  #definitionRows(query) {
    const doc = this.workspace.current;
    if (!doc) return { rows: [], empty: "Open a file to go to a definition" };
    const text = doc.state.doc.toString();
    const rows = [];
    for (const t of tokenize(text)) {
      if (t.category !== "definitionName") continue;
      const name = text.slice(t.from, t.to);
      const match = query ? fuzzy(query, name) : { score: 0, hits: [] };
      if (!match) continue;
      rows.push({ label: name, hits: match.hits, score: match.score, detail: `line ${doc.state.doc.lineAt(t.from).number}`,
        run: () => this.#moveTo(t.from) });
    }
    if (query) rows.sort((a, b) => b.score - a.score);
    return { rows: rows.slice(0, pickerRows), empty: "No definitions" };
  }

  #goToLine(line, column) {
    this.#moveTo(offsetOf(this.view.state, line, column));
  }

  #moveTo(offset) {
    this.#remember();
    this.view.dispatch({ selection: { anchor: offset }, scrollIntoView: true });
    this.view.focus();
  }

  // ---- Editing commands (editing.js) and the clipboard.

  #edit(command) {
    const applied = command(this.view);
    this.view.focus();
    return applied;
  }

  // Cut, Copy, and Paste from a menu or the picker; their keys are the webview's own (schemes.js, `shown`).
  async #clipboard(kind) {
    const view = this.view;
    try {
      if (kind === "paste") {
        const text = await navigator.clipboard.readText();
        view.dispatch(view.state.replaceSelection(text), { userEvent: "input.paste" });
      } else {
        const { state } = view;
        const text = state.selection.ranges.map((r) => state.sliceDoc(r.from, r.to)).join("\n");
        if (text) {
          await navigator.clipboard.writeText(text);
          if (kind === "cut") view.dispatch(view.state.replaceSelection(""), { userEvent: "delete.cut" });
        }
      }
    } catch (error) {
      this.#failed(`Could not use the clipboard: ${error.message ?? error}`, "Could not use the clipboard");
    }
    view.focus();
  }

  // ---- Back and Forward (patterns.md §22.5): the places a jump left, as VS Code and GNOME's browsers keep them.

  #history = { back: [], forward: [] };
  #travelling = false;

  // The cursor's place in the current file, before a jump: a file opened, a line, a definition, a problem.
  #remember() {
    const doc = this.workspace.current;
    if (this.#travelling || !doc) return;
    const place = { path: doc.path, offset: doc.state.selection.main.head };
    const last = this.#history.back.at(-1);
    if (last?.path === place.path && last.offset === place.offset) return;
    this.#history.back.push(place);
    if (this.#history.back.length > 50) this.#history.back.shift();
    this.#history.forward = [];
  }

  async #travel(direction) {
    const [from, to] = direction < 0 ? ["back", "forward"] : ["forward", "back"];
    const place = this.#history[from].pop();
    const doc = this.workspace.current;
    if (!place) return;
    if (doc) this.#history[to].push({ path: doc.path, offset: doc.state.selection.main.head });
    this.#travelling = true;
    try {
      if (!(await this.openFile(place.path))) return;
      const offset = Math.min(place.offset, this.view.state.doc.length);
      this.view.dispatch({ selection: { anchor: offset }, scrollIntoView: true });
      this.view.focus();
    } finally {
      this.#travelling = false;
    }
  }

  // ---- Go to Definition: the name at the cursor, defined in this file, another open file, or the project's.

  /** Opens the definition of the name at the cursor; says so in the console when there is none. */
  async goToDefinition() {
    const doc = this.workspace.current;
    if (!doc) return false;
    const text = doc.state.doc.toString();
    const head = doc.state.selection.main.head;
    const at = tokenize(text).find((t) => t.from <= head && head <= t.to && /^[^\s()[\]"';]/.test(text[t.from]));
    if (!at) return false;
    const name = text.slice(at.from, at.to);
    const definedIn = (source) => tokenize(source).find((t) => t.category === "definitionName"
      && source.slice(t.from, t.to) === name)?.from;
    const here = definedIn(text);
    if (here !== undefined) {
      this.#moveTo(here);
      return true;
    }
    const others = this.workspace.documents.filter((d) => d !== doc && d.path.endsWith(".lisp"));
    for (const other of others) {
      const offset = definedIn(other.state.doc.toString());
      if (offset !== undefined) return this.#openAt(other.path, offset);
    }
    const unopened = (this.workspace.files ?? []).filter((p) => p.endsWith(".lisp") && !this.workspace.documentOf(p));
    for (const path of unopened) {
      const read = await this.#request("file.read", { path });
      const offset = read && definedIn(read.text);
      if (offset !== undefined && offset !== null) return this.#openAt(path, offset);
    }
    const library = this.#libraryDefinitions.find((d) => d.name === name);
    const builtIn = formNames().includes(name) || this.language?.builtins?.includes(name)
      || this.language?.primitives?.some((p) => p.name === name);
    this.notify(library ? `${name} is defined in the standard library (${library.path})`
      : builtIn ? `${name} is built into the language` : `No definition of ${name} in the project`);
    return false;
  }

  async #openAt(path, offset) {
    this.#remember();
    if (!(await this.openFile(path))) return false;
    this.view.dispatch({ selection: { anchor: offset }, scrollIntoView: true });
    this.view.focus();
    return true;
  }

  // Puts the current file first among the recently opened, remembered for the project.
  #noteRecent() {
    const ws = this.workspace;
    const path = ws.current?.path;
    if (!path || this.recent[0] === path) return;
    this.recent = [path, ...this.recent.filter((p) => p !== path)].slice(0, 20);
    const all = this.#savedRecent();
    all[ws.root] = this.recent;
    localStorage.setItem(recentKey, JSON.stringify(all));
  }

  #savedRecent() {
    try {
      return JSON.parse(localStorage.getItem(recentKey) ?? "{}");
    } catch {
      return {};
    }
  }

  // ---- Completion (complete.js): the language from the sidecar, and the definitions of the other files.

  async #learnLanguage() {
    const language = await this.#request("language", {});
    if (!language) return;
    this.language = language;
    this.#libraryDefinitions = language.library.flatMap((f) =>
      definitions(f.text).map((d) => ({ ...d, path: f.path.replace(/\.lisp$/, "") })));
  }

  // The definitions of the open .lisp files but the one in `state`, and the library's.
  #otherDefinitions(state) {
    const found = [];
    for (const doc of this.workspace.documents) {
      if (doc.state.doc === state.doc || !doc.path.endsWith(".lisp")) continue;
      let defs = this.#definitionCache.get(doc.state.doc);
      if (!defs) {
        defs = definitions(doc.state.doc.toString()).map((d) => ({ ...d, path: doc.path }));
        this.#definitionCache.set(doc.state.doc, defs);
      }
      found.push(...defs);
    }
    return [...found, ...this.#libraryDefinitions];
  }

  // ---- The piece's page (patterns.md §18, editor-refactor.md §4h.1).

  get pageUrl() {
    return this.page.port ? `http://127.0.0.1:${this.page.port}/` : "";
  }

  // The active plan's page port changed: the first page of a project opens by itself, as does the page of a
  // piece Play plays while the page's tab is closed (#pageAfterPlay); one that comes back reloads.
  #pageChanged(port) {
    if (port === this.page.port) return;
    this.page.port = port;
    this.renderTop();  // the Page button shows while a page is served
    if (port && !this.page.opened) {
      this.#openPageByItself();
      return;
    }
    this.renderTabs();
    this.renderEditor();
  }

  #openPageByItself() {
    this.page.opened = true;
    const typing = this.view.contentDOM.contains(document.activeElement);
    this.showPage();
    // Keys go on somewhere they act: the page's tab, from which Ctrl+Page Up goes back to the code.
    if (typing) this.tabs.querySelector(".page-tab")?.focus();
    this.announce("The piece's page opened");
  }

  // Play or Update brings back a page whose tab was closed: at once when the piece serves it already, else
  // when its port arrives. A page whose tab is open stays where it is, and the code keeps the front.
  #pageAfterPlay() {
    if (this.page.open) return;
    if (this.page.port) this.#openPageByItself();
    else this.page.opened = false;
  }

  /** Opens the page's tab and shows it. */
  showPage() {
    this.page.open = true;
    this.page.current = true;
    this.renderTabs();
    this.renderEditor();
    this.renderStatus();
  }

  /** Shows the document with `id`, leaving the page's tab. */
  showDocument(id) {
    const wasPage = this.page.current;
    this.page.current = false;
    this.workspace.select(id);
    if (wasPage) this.renderAll();
  }

  closePage() {
    const hadFocus = this.pageView.contains(document.activeElement) || document.activeElement?.closest?.(".page-tab");
    this.page.open = false;
    this.page.current = false;
    this.pageFrame.removeAttribute("src");
    this.renderAll();
    if (hadFocus) this.#focusContent();
  }

  reloadPage() {
    if (this.pageUrl) this.pageFrame.src = this.pageUrl;
  }

  /** The page in a window of its own, for a second screen; again, the same window comes forward. */
  async openPageWindow(at) {
    const webview = window.__TAURI__?.webviewWindow;
    if (!webview || !this.pageUrl) return;
    const open = await webview.WebviewWindow.getByLabel("page");
    if (open) {
      await open.setFocus();
      return;
    }
    const created = new webview.WebviewWindow("page", { url: this.pageUrl, title: `Page — ${this.workspace.name}`,
      width: 900, height: 640, ...(at ?? {}) });
    created.once("tauri://error", (e) => this.#failed(`Could not open the page's window: ${e.payload}`,
      "Could not open the page's window"));
  }

  /** The page in the system's browser. */
  async openPageInBrowser() {
    if (!this.pageUrl) return;
    try {
      await window.__TAURI__?.shell?.open(this.pageUrl);
    } catch (error) {
      this.#failed(`Could not open the browser: ${error.message ?? error}`, "Could not open the browser");
    }
  }

  #pageTab() {
    const name = `Page · ${this.workspace.name || "espresso"}`;
    const current = this.page.current;
    return h("div", {
      class: `tab page-tab${current ? " current" : ""}`, role: "tab", "aria-selected": String(current),
      tabindex: current ? "0" : "-1", title: this.pageUrl || "No page playing",
      onclick: () => this.showPage(),
      onauxclick: (e) => {
        if (e.button === 1) this.closePage();
      },
      onpointerdown: (e) => this.#dragPageTab(e),
      onkeydown: (e) => this.#tabKey(e, "page"),
    },
    icon("globe"), h("span", { class: "label", "data-label": name }, name),
    h("span", { class: "slot" }, h("button", {
      class: "close", title: this.#titled("Close", "closeBuffer"), "aria-label": `Close ${name}`, tabindex: "-1",
      onclick: (e) => {
        e.stopPropagation();
        this.closePage();
      },
    }, icon("close"))));
  }

  /** The page in a window of its own at `at`, the screen position where its tab was dropped; its tab leaves this window. */
  tearOffPage(at) {
    this.openPageWindow(at);
    this.closePage();
  }

  // The page's tab pulled out of the tab strip opens the page in a window of its own, as a browser's tab does.
  #dragPageTab(down) {
    if (down.button !== 0 || down.target.closest(".close")) return;
    const tab = down.currentTarget;
    const strip = this.tabs.getBoundingClientRect();
    let ghost = null;
    let tearing = false;
    try {
      tab.setPointerCapture(down.pointerId);  // the pointer is followed past the window's edge
    } catch {
      // A pointer the page did not see go down: the window's listeners follow it anyway.
    }
    const move = (e) => {
      if (!ghost && Math.hypot(e.clientX - down.clientX, e.clientY - down.clientY) < dragThreshold) return;
      if (!ghost) {
        ghost = h("div", { class: "tab-ghost", "aria-hidden": "true" }, icon("globe"), h("span", {}, tab.textContent));
        this.window.append(ghost);
        tab.classList.add("dragging");
      }
      tearing = Boolean(this.pageUrl) && (e.clientY < strip.top - tearDistance || e.clientY > strip.bottom + tearDistance ||
        e.clientX < 0 || e.clientX > window.innerWidth);
      ghost.classList.toggle("tearing", tearing);
      ghost.style.transform = `translate(${e.clientX + 10}px, ${e.clientY + 10}px)`;
    };
    const end = (e) => {
      for (const type of ["pointermove", "pointerup", "pointercancel"]) {
        window.removeEventListener(type, type === "pointermove" ? move : end, true);
      }
      if (!ghost) return;
      ghost.remove();
      tab.classList.remove("dragging");
      if (tearing && e.type === "pointerup") this.tearOffPage({ x: e.screenX - 40, y: e.screenY - 14 });
    };
    window.addEventListener("pointermove", move, true);
    window.addEventListener("pointerup", end, true);
    window.addEventListener("pointercancel", end, true);
  }

  // The bar, and the frame while a page plays or the empty state while none does.
  #renderPage() {
    const url = this.pageUrl;
    this.pageAddress.textContent = url || "No page playing";
    for (const button of this.pageView.querySelectorAll(".bar button")) button.disabled = !url;
    this.pageFrame.hidden = !url;
    this.pageEmpty.hidden = Boolean(url);
    if (url && this.pageFrame.getAttribute("src") !== url) this.pageFrame.src = url;
    if (!url) this.pageFrame.removeAttribute("src");
  }

  // ---- The panel.

  // Below 1000 px the side bar lies over the code, closed until asked for (patterns.md §26; the HIG's adaptive sidebar).
  narrow = matchMedia("(max-width: 1000px)");
  sideOpen = false;

  // The panel's height: the user's (the splitter's), within the window: at least two bars, and room left for the code.
  #fitPanel() {
    const room = this.content.clientHeight;
    const least = 2 * 36;
    const most = Math.max(least, room - 36 - least);
    const height = Math.round(Math.min(Math.max(this.layout.panelHeight ?? 280, least), room ? most : Infinity));
    this.content.style.setProperty("--panelHeight", `${height}px`);
    this.splitter?.setAttribute("aria-valuenow", String(height));
    this.splitter?.setAttribute("aria-valuemax", String(most));
    return height;
  }

  // The splitter (a separator the pointer drags and the arrows move, the APG's window splitter).
  #makeSplitter() {
    const splitter = h("div", { class: "splitter", role: "separator", "aria-orientation": "horizontal", tabindex: "0",
      "aria-label": "Panel Height", "aria-valuemin": "72", title: "Drag to Resize the Panel" });
    const set = (height) => {
      this.layout.panelHeight = Math.round(height);
      this.layout.panelHeight = this.#fitPanel();
      localStorage.setItem(layoutKey, JSON.stringify(this.layout));
    };
    splitter.addEventListener("pointerdown", (e) => {
      if (e.button !== 0) return;
      e.preventDefault();
      splitter.setPointerCapture(e.pointerId);
      const startY = e.clientY;
      const startHeight = this.panel.getBoundingClientRect().height;
      const move = (m) => set(startHeight - (m.clientY - startY));
      const up = () => {
        splitter.removeEventListener("pointermove", move);
        splitter.removeEventListener("pointerup", up);
      };
      splitter.addEventListener("pointermove", move);
      splitter.addEventListener("pointerup", up);
    });
    splitter.addEventListener("keydown", (e) => {
      const by = { ArrowUp: 24, ArrowDown: -24 }[e.key];
      if (by === undefined) return;
      e.preventDefault();
      set(this.panel.getBoundingClientRect().height + by);
    });
    return splitter;
  }

  /** Whether the side bar shows: as a column, or over the code in a narrow window. */
  get sideShown() {
    return this.narrow.matches ? this.sideOpen : this.layout.side;
  }

  /** Shows the side bar, as a column or over the code, when it is hidden. */
  #showSide() {
    if (!this.sideShown) this.toggle("side");
  }

  toggle(region) {
    const part = region === "side" ? this.side : this.panel;
    const hadFocus = part.contains(document.activeElement);
    if (region === "side" && this.narrow.matches) {
      this.sideOpen = !this.sideOpen;
      this.renderLayout();
      this.renderEditor();
      if (this.sideOpen) (this.tree.querySelector('[tabindex="0"]') ?? this.tree.querySelector(".row"))?.focus();
      else this.#focusContent();
      return;
    }
    this.layout[region] = !this.layout[region];
    if (region === "panel" && !this.layout.panel) this.maximized = false;
    localStorage.setItem(layoutKey, JSON.stringify(this.layout));
    this.renderLayout();
    if (region === "side") this.renderEditor();  // the empty state names the side bar only while it shows
    if (hadFocus && !this.layout[region]) this.#focusContent();  // the part that hides takes no focus with it
  }

  toggleMaximized() {
    this.maximized = !this.maximized;
    this.renderLayout();
  }

  /** Shows the panel's "console", "problems", or "search" view, opening the panel. */
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
    this.#remember();
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
    const trimmed = this.log.length > logLimit;
    if (trimmed) this.log.splice(0, this.log.length - logLimit);
    // A new line is added to the log, not the log drawn again, so assistive technology reads the new line only.
    if (!this.filter.trim() && this.logView.querySelector(".msg")) {
      if (trimmed) this.logView.firstElementChild?.remove();
      this.logView.append(this.#logEntry(entry));
      this.logView.scrollTop = this.logView.scrollHeight;
    } else {
      this.renderLog();
    }
    // A compiler warning comes as a line with a place, "loops.lisp:14:5: warning: …": a problem of its own severity.
    const warning = entry.kind === "info" && entry.location && messageOf(entry).startsWith("warning: ");
    if ((entry.kind === "error" || warning) && entry.location) {
      // The same error again (an evaluation repeated) is one problem, not two.
      const problem = { ...entry.location, message: warning ? messageOf(entry).slice("warning: ".length) : messageOf(entry),
        severity: warning ? "warning" : "error" };
      const known = this.problems.some((p) => p.path === problem.path && p.line === problem.line
        && p.column === problem.column && p.message === problem.message);
      if (!known) {
        this.problems.push(problem);
        this.#problemsChanged();
      }
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
      if (settled || (this.workspace.dirtyDocuments.length === 0 && !this.#trashing)) return;
      event.preventDefault();
      await this.#commitTrash();
      if (await this.settleChanges()) {
        settled = true;
        await tauriWindow.destroy();
      }
    });
  }

  // ---- Commands by name (tui.md §8.1), for the key schemes and the menus.

  #commands = {
    openFolder: () => this.openFolder(),
    save: (doc) => doc && this.save(doc.id),
    saveAndQuit: async (doc) => {
      if (doc && !(await this.save(doc.id))) return;
      this.quit();
    },
    quit: () => this.quit(),
    quitWithoutSaving: () => this.quit({ force: true }),
    closeBuffer: (doc) => (this.page.current ? this.closePage() : doc && this.close(doc.id)),
    showPage: () => this.showPage(),
    fold: (doc) => doc && foldCode(this.view),
    unfold: (doc) => doc && unfoldCode(this.view),
    foldAll: (doc) => doc && foldAll(this.view),
    unfoldAll: (doc) => doc && unfoldAll(this.view),
    nextBuffer: () => this.cycleBuffer(1),
    previousBuffer: () => this.cycleBuffer(-1),
    play: () => this.play(),
    update: () => this.play(),
    stop: () => this.stop(),
    evalForm: () => this.evaluate("form"),
    evalLastForm: () => this.evaluate("last"),
    toggleFileList: () => this.toggle("side"),
    toggleConsole: () => this.toggle("panel"),
    clearConsole: () => this.clearLog(),
    showProblems: () => this.showPanelView("problems"),
    nextProblem: (doc) => doc && nextDiagnostic(this.view),
    previousProblem: (doc) => doc && previousDiagnostic(this.view),
    showConsole: () => this.showPanelView("console"),
    back: () => this.#travel(-1),
    forward: () => this.#travel(1),
    goToDefinition: (doc) => doc && this.goToDefinition(),
    cut: (doc) => doc && this.#clipboard("cut"),
    copy: (doc) => doc && this.#clipboard("copy"),
    paste: (doc) => doc && this.#clipboard("paste"),
    // The editing commands (editing.js), on the code.
    ...Object.fromEntries([
      "undo", "redo", "selectAll", "selectNone", "toggleComment", "toggleBlockComment", "expandSelection",
      "shrinkSelection", "addCursorAbove", "addCursorBelow", "duplicate", "moveLineUp", "moveLineDown", "deleteLine",
      "indentMore", "indentLess", "joinLines", "sortLines", "upperCase", "lowerCase", "titleCase",
    ].map((name) => [name, (doc) => doc && this.#edit(editing[name])])),
    addNextOccurrence: (doc) => doc && this.#edit(editing.selectNextOccurrence),
    selectAllOccurrences: (doc) => doc && this.#edit(editing.selectSelectionMatches),
    goToMatchingBracket: (doc) => doc && this.#edit(editing.cursorMatchingBracket),
    toggleMark: (doc) => doc && this.#edit(toggleMark),
    nextMark: (doc) => doc && this.#edit(nextMark),
    previousMark: (doc) => doc && this.#edit(previousMark),
    showMarks: () => this.showMarks(),
    clearMarks: () => this.clearMarks(),
    // View and Help (patterns.md §19.8, §23.5).
    zoomIn: () => this.setCodeSize(this.preferences.codeSize + 1),
    zoomOut: () => this.setCodeSize(this.preferences.codeSize - 1),
    resetZoom: () => this.setCodeSize(codeSizes.normal),
    toggleWordWrap: () => this.setWrap(!this.preferences.wrap),
    toggleFullscreen: () => this.toggleFullscreen(),
    preferences: () => this.showPreferences(),
    showLanguageReference: () => this.showReference("language", "Language Reference"),
    showLibraryReference: () => this.showReference("library", "Standard Library"),
    toggleUpdateOnSave: () => this.#setRun({ updateOnSave: !this.run.updateOnSave }),
    // Files (patterns.md §20.7, §23.5): each acts on the file a context menu names, else the tree's
    // focused row while the tree has focus, else the current file.
    newFile: (doc, dir) => this.newFile(dir),
    renameFile: (doc, path) => this.#withFile(path, (p) => this.renameFile(p)),
    trashFile: (doc, path) => this.#withFile(path, (p) => this.trashFile(p)),
    copyPath: (doc, path) => this.#withFile(path, (p) => this.#copyText(`${this.workspace.root}/${p}`)),
    copyRelativePath: (doc, path) => this.#withFile(path, (p) => this.#copyText(p)),
    openContainingFolder: (doc, path) => this.#withFile(path, (p) => this.#reveal(p)),
    revealInSideBar: (doc, path) => this.#withFile(path, (p) => this.revealInSideBar(p)),
    closeOthers: (doc, id) => this.closeAll({ except: id ?? doc?.id }),
    closeSaved: () => this.closeAll({ saved: true }),
    closeAll: () => this.closeAll(),
    openRecent: () => this.openRecent(),
    saveAs: (doc) => doc && this.saveAs(doc.id),
    saveAll: () => this.saveAll(),
    reopenClosed: () => this.reopenClosed(),
    closeFolder: () => this.closeFolder(),
    findInFiles: (doc) => this.findInFiles(doc),
    showSearch: () => this.showPanelView("search"),
    focusNextPart: () => this.#focusPart(1),
    focusPreviousPart: () => this.#focusPart(-1),
    switchBuffer: () => this.openPicker(""),
    showAllCommands: () => this.openPicker(">"),
    goToLine: (doc) => doc && this.openPicker(":"),
    searchForward: (doc) => doc && openFind(this.view),
    searchBackward: (doc) => doc && (openFind(this.view), findPrevious(this.view)),
    searchNext: (doc) => doc && findNext(this.view),
    searchPrevious: (doc) => doc && findPrevious(this.view),
    replace: (doc) => doc && openFind(this.view, { replace: true }),
    changeKeyScheme: () => this.#toggleMenu(this.schemeItem, () => this.#schemeMenu()),
    showShortcuts: () => this.showShortcuts(),
    showAbout: () => this.showAbout(),
  };

  // ---- The menu bar (menubar.js, patterns.md §19.8): the command table's menus, kept current.

  menuBar = new MenuBar((name) => this.#runFromMenu(name));
  #menuQueued = false;
  #lastRun = { command: null, at: 0, from: null };  // the last command a key or the menu bar ran

  /** Brings the menu bar up to the window's state, once for all the renders of one turn. */
  renderMenuBar() {
    if (this.#menuQueued) return;
    this.#menuQueued = true;
    queueMicrotask(() => {
      this.#menuQueued = false;
      this.menuBar.show(this.menuModel()).catch((error) =>
        this.addLog({ kind: "error", text: `Could not build the menu bar: ${error.message ?? error}` }));
    });
  }

  /** Whether the command `name` can run now: the menus disable it otherwise. */
  commandEnabled(name) {
    const doc = Boolean(this.workspace.current);
    const project = this.workspace.root !== null;
    const documents = this.workspace.documents.length;
    const needs = {
      save: doc, closeBuffer: doc, searchForward: doc, replace: doc, searchNext: doc, searchPrevious: doc,
      goToLine: doc, goToDefinition: doc, fold: doc, unfold: doc, foldAll: doc, unfoldAll: doc,
      nextProblem: doc && this.problems.length > 0, previousProblem: doc && this.problems.length > 0,
      switchBuffer: project, play: this.connected && project && !this.playing,
      update: this.connected && project && this.playing, stop: this.playing,
      evalForm: doc && this.connected, evalLastForm: doc && this.connected,
      back: this.#history.back.length > 0, forward: this.#history.forward.length > 0,
      newFile: project, renameFile: doc, trashFile: doc, copyPath: doc, copyRelativePath: doc,
      openContainingFolder: doc, revealInSideBar: doc, closeAll: documents > 0, closeOthers: documents > 1,
      closeSaved: this.workspace.documents.some((d) => !this.workspace.isDirty(d)),
      nextMark: doc && markedLines(this.workspace.current.state).length > 0,
      previousMark: doc && markedLines(this.workspace.current.state).length > 0,
      clearMarks: doc && markedLines(this.workspace.current.state).length > 0,
      showMarks: project,
      saveAs: doc, saveAll: this.workspace.dirtyDocuments.length > 0, closeFolder: project, findInFiles: project,
      reopenClosed: this.#closed.length > 0, openRecent: this.#recentProjects().length > 0,
    };
    // A reference (Help) is read-only and no file of the project: what acts on a file does not act on it.
    const reference = this.workspace.current?.reference === true;
    const onFile = ["save", "saveAs", "renameFile", "trashFile", "copyPath", "copyRelativePath", "openContainingFolder",
      "revealInSideBar", "evalForm", "evalLastForm"];
    if (reference && onFile.includes(name)) return false;
    return needs[name] ?? (commandNamed(name)?.editor ? doc : true);
  }

  /** The menu bar for the window's state (menubar.js, menuModel). */
  menuModel(mac = isMac) {
    const checks = { toggleFileList: this.sideShown, toggleConsole: this.layout.panel,
      toggleUpdateOnSave: this.run.updateOnSave, toggleWordWrap: this.preferences.wrap,
      toggleFullscreen: this.fullscreen };
    // Linux: GTK takes a menu's key before the webview sees it, so Cut, Copy, and Paste keep the webview's.
    const unaccelerated = mac ? new Set() : new Set(["cut", "copy", "paste"]);
    return menuModel({
      label: (name) => (mac ? { quit: "Quit espresso", preferences: "Settings…" }[name] : undefined),
      accelerator: (name) => (unaccelerated.has(name) ? null : acceleratorOf(bindingOf(this.scheme, name))),
      enabled: (name) => this.commandEnabled(name),
      checked: (name) => checks[name],
    }, mac);
  }

  // Whether `from` (a key, or the menu bar) ran `command` a moment ago: one key press seen by both.
  #ranJustNow(command, from) {
    const last = this.#lastRun;
    return last.command === command && last.from === from && performance.now() - last.at < 150;
  }

  // A menu item's command. A key the menu bar took (GTK takes them first) runs once: not again when the
  // webview sees it too. An editing command with a field focused does the field's work, or nothing.
  #runFromMenu(name) {
    if (this.#ranJustNow(name, "key")) return;
    this.#lastRun = { command: name, at: performance.now(), from: "menu" };
    if (commandNamed(name)?.editor && !this.view.contentDOM.contains(document.activeElement)) {
      const field = { undo: "undo", redo: "redo", selectAll: "selectAll" }[name];
      if (field && document.activeElement?.matches?.("input, textarea")) document.execCommand(field);
      return;
    }
    this.runCommand(name);
  }

  /** The names of the commands the window runs. */
  commandNames() {
    return new Set(Object.keys(this.#commands));
  }

  /** Runs the command `name`; false when there is no such command. */
  runCommand(name, target) {
    const command = this.#commands[name];
    if (!command) return false;
    command(this.workspace.current, target);
    return true;
  }

  /** How the key scheme writes `command`'s binding: "⌘S", "C-x C-s", ":w", "^O"; "" when it has none. */
  label(command) {
    return bindingLabel(this.scheme, command);
  }

  /** A control's tooltip: its name, then its key when the scheme binds one. */
  #titled(name, command) {
    const keys = this.label(command);
    return keys ? `${name} (${keys})` : name;
  }

  setScheme(name) {
    if (!schemes[name]) return;
    this.scheme = name;
    localStorage.setItem(schemeKey, name);
    this.#chords = chordTable(name);
    for (const doc of this.workspace.documents) this.#dispatchTo(doc.id, () => useScheme(name));
    this.#watchMode();
    this.renderAll();
  }

  // Follows Vim's mode in the view, whose Vim instance is new with each state it shows.
  #watchMode() {
    this.vimMode = "normal";
    if (this.scheme === "vim") {
      watchVimMode(this.view, (mode) => {
        this.vimMode = mode;
        this.renderStatus();
      });
    }
  }

  /** Shows the open document `step` places on, round the tabs. */
  cycleBuffer(step) {
    const docs = this.workspace.documents;
    const i = docs.findIndex((d) => d.id === this.workspace.currentId);
    if (docs.length < 2 || i < 0) return;
    this.workspace.select(docs[(i + step + docs.length) % docs.length].id);
  }

  /** Closes the window, which first settles unsaved changes; with `force`, without asking. */
  async quit({ force = false } = {}) {
    const tauriWindow = window.__TAURI__?.window?.getCurrentWindow();
    if (!tauriWindow) return;
    if (force) await this.#commitTrash();
    (force ? tauriWindow.destroy() : tauriWindow.close()).catch((error) =>
      this.#failed(`Could not close the window: ${error.message ?? error}`, "Could not close the window"));
  }

  // The window's chords in the key scheme, whatever has focus; Escape closes a menu. Keys that
  // type (no Ctrl, Alt, or Cmd, and not an F key) act only from the editor's text.
  #onKey(e) {
    if (e.key === "Escape" && this.menu) {
      e.preventDefault();
      e.stopPropagation();
      this.closeMenu(true);
      return;
    }
    if (this.scrim) return;
    if (e.key === "ContextMenu" || (e.key === "F10" && e.shiftKey && !e.ctrlKey && !e.altKey && !e.metaKey)) {
      if (this.#contextFor(document.activeElement, null)) {
        e.preventDefault();
        e.stopPropagation();
      }
      return;
    }
    // Alt+1…9 shows the first nine tabs (the HIG's tabs); Alt with a digit is no access key.
    if (e.altKey && !e.ctrlKey && !e.metaKey && !e.shiftKey && /^Digit[1-9]$/.test(e.code)) {
      const tab = [...this.tabs.querySelectorAll(".tab")][Number(e.code.slice(5)) - 1];
      if (tab) {
        e.preventDefault();
        if (tab.classList.contains("page-tab")) this.showPage();
        else this.showDocument(Number(tab.dataset.id));
      }
      return;
    }
    const command = this.#chords.get(eventChord(e));
    if (!command) return;
    // A key that types, or an editing command's key, acts only from the code, so a field keeps its own.
    const types = !(e.ctrlKey || e.metaKey || e.altKey || /^F\d+$/.test(e.key));
    if ((types || commandNamed(command)?.editor) && !this.view.contentDOM.contains(document.activeElement)) return;
    e.preventDefault();
    e.stopPropagation();
    if (this.#ranJustNow(command, "menu")) return;  // the menu bar took the key first
    this.#lastRun = { command, at: performance.now(), from: "key" };
    this.runCommand(command);
  }

  // Focus Next and Previous Part (VS Code's F6; Ctrl+F6 here, as F6 stops): the side bar, the tabs, the code or the
  // page, the panel, the status bar, in turn.
  #focusPart(direction) {
    const parts = [
      this.layout.side && !this.side.hidden && this.side.offsetParent !== null
        ? this.tree.querySelector('[tabindex="0"]') ?? this.tree.querySelector(".row") : null,
      this.tabs.querySelector('.tab[tabindex="0"]') ?? this.tabs.querySelector(".tab"),
      this.page.current ? this.pageFrame : this.workspace.current ? this.view.contentDOM : this.empty.querySelector("button"),
      this.layout.panel ? this.panelTabs.find((t) => t.tabIndex === 0) : null,
      this.statusBar.querySelector("button"),
    ].filter(Boolean);
    if (parts.length === 0) return;
    const here = parts.findIndex((p) => p === document.activeElement || p.closest?.(".tree, .tabs, .editor, .panel, .status")
      ?.contains(document.activeElement));
    const next = parts[((here < 0 ? (direction > 0 ? -1 : 0) : here) + direction + parts.length) % parts.length];
    next.focus();
  }

  // ---- The workspace's changes.

  #edited(state) {
    const doc = this.workspace.current;
    if (!doc) return;
    const marked = doc.state.field(marksField, false) !== state.field(marksField, false);
    this.workspace.update(doc.id, state);
    if (marked) this.#keepMarks(doc.path, state);
  }

  // ---- Preferences (patterns.md §23.5): applied at once, kept by the webview.

  #keepPreferences() {
    localStorage.setItem(preferencesKey, JSON.stringify(this.preferences));
    this.renderMenuBar();
  }

  /** Zoom: the code's size, from 8 to 32 px, its line in proportion and on a whole pixel. */
  setCodeSize(size) {
    this.preferences.codeSize = Math.min(Math.max(Math.round(size), codeSizes.least), codeSizes.most);
    this.#keepPreferences();
    this.#applyCodeSize();
    this.#applyInterfaceSize();
  }

  #applyCodeSize() {
    const size = this.preferences.codeSize;
    this.window.style.setProperty("--codeSize", `${size}px`);
    this.window.style.setProperty("--codeLine", `${Math.round((size * 22) / 14)}px`);
    this.view.requestMeasure();
  }

  /**
   * Interface Size: the whole window's scale (the webview's zoom), for large text; GNOME's text scaling does not
   * reach a webview's pixels (WCAG 1.4.4).
   */
  setInterfaceSize(scale) {
    this.preferences.interfaceSize = Number(scale);
    this.#keepPreferences();
    this.#applyInterfaceSize();
  }

  #applyInterfaceSize() {
    const scale = this.preferences.interfaceSize ?? 1;
    window.__TAURI__?.webview?.getCurrentWebview?.().setZoom(scale).catch((error) =>
      this.#failed(`Could not scale the window: ${error.message ?? error}`, "Could not scale the window"));
  }

  /** Word Wrap, in every open file. */
  setWrap(on) {
    this.preferences.wrap = on;
    this.#keepPreferences();
    for (const doc of this.workspace.documents) if (!doc.reference) this.#dispatchTo(doc.id, () => useWrap(on));
  }

  async toggleFullscreen() {
    const tauriWindow = window.__TAURI__?.window?.getCurrentWindow();
    if (!tauriWindow) return;
    try {
      this.fullscreen = !(await tauriWindow.isFullscreen());
      await tauriWindow.setFullscreen(this.fullscreen);
    } catch (error) {
      this.#failed(`Could not change full screen: ${error.message ?? error}`, "Could not change full screen");
    }
    this.renderMenuBar();
  }

  /** Help's references (docs/), read-only in a tab of their own. */
  async showReference(name, title) {
    try {
      const response = await fetch(new URL(`docs/${name}.md`, import.meta.url));
      if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
      const doc = this.workspace.openText(title, await response.text());
      this.#dispatchTo(doc.id, () => useWrap(true));  // prose: its lines wrap, whatever the code's do
      if (this.page.current) this.showDocument(this.workspace.currentId);
      this.view.focus();
    } catch (error) {
      this.#failed(`Could not open the ${title}: ${error.message ?? error}`, `Could not open the ${title}`);
    }
  }

  // The HIG's preferences: groups of rows, each applied as it changes; no OK or Apply.
  showPreferences() {
    const row = (title, control) => h("label", { class: "pref" }, h("span", {}, title), control);
    const select = (options, value, onchange) => {
      const el = h("select", { class: "choice", onchange: (e) => onchange(e.target.value) },
        options.map(([v, label]) => h("option", { value: String(v), selected: String(v) === String(value) }, label)));
      return el;
    };
    const toggle = (on, onchange) => h("input", { type: "checkbox", class: "switch", role: "switch", checked: on,
      onchange: (e) => onchange(e.target.checked) });
    const sizeLabel = h("span", { class: "value" }, `${this.preferences.codeSize} px`);
    const step = (by, label) => h("button", { class: "btn flat icon-only", "aria-label": label, title: label,
      onclick: (e) => {
        e.preventDefault();
        this.setCodeSize(this.preferences.codeSize + by);
        sizeLabel.textContent = `${this.preferences.codeSize} px`;
      } }, icon(by < 0 ? "minus" : "plus"));
    const content = h("div", { class: "preferences" },
      h("h3", {}, "Editor"),
      h("div", { class: "group" },
        row("Key Scheme", select(schemeNames.map((n) => [n, schemes[n].label]), this.scheme, (v) => this.setScheme(v))),
        row("Interface Size", select([[1, "100%"], [1.25, "125%"], [1.5, "150%"], [1.75, "175%"], [2, "200%"]],
          this.preferences.interfaceSize ?? 1, (v) => this.setInterfaceSize(v))),
        row("Word Wrap", toggle(this.preferences.wrap, (on) => this.setWrap(on))),
        // Two buttons and a value: a group named by its title, not a label (which would name one button).
        h("div", { class: "pref", role: "group", "aria-labelledby": "pref-code-size" },
          h("span", { id: "pref-code-size" }, "Code Size"),
          h("span", { class: "stepper" }, step(-1, "Smaller"), sizeLabel, step(1, "Larger")))),
      h("h3", {}, "Play"),
      h("div", { class: "group" },
        row("Crossfade", select(fades, this.run.fade, (v) => this.#setRun({ fade: Number(v) }))),
        row("Quantize", select(grids, this.run.quantize, (v) => this.#setRun({ quantize: v }))),
        row("Update on Save", toggle(this.run.updateOnSave, (on) => this.#setRun({ updateOnSave: on })))));
    return this.#notice("Preferences", "preferences-heading", content, "wide");
  }

  // ---- Marks (patterns.md §21.7), kept per project as each marked line's number and text.

  #keptMarks() {
    try {
      return JSON.parse(localStorage.getItem(marksKey) ?? "{}")[this.workspace?.root] ?? {};
    } catch {
      return {};
    }
  }

  #keepMarks(path, state) {
    let all = {};
    try {
      all = JSON.parse(localStorage.getItem(marksKey) ?? "{}");
    } catch {
      all = {};
    }
    const project = all[this.workspace.root] ?? {};
    const kept = savedMarks(state);
    if (kept.length) project[path] = kept;
    else delete project[path];
    all[this.workspace.root] = project;
    localStorage.setItem(marksKey, JSON.stringify(all));
    this.renderMenuBar();
  }

  // Every mark of the project: the open files' as they are, the others' as kept.
  #allMarks() {
    const found = [];
    const kept = this.#keptMarks();
    for (const doc of this.workspace.documents) {
      for (const n of markedLines(doc.state)) found.push({ path: doc.path, line: n, text: doc.state.doc.line(n).text });
    }
    for (const [path, lines] of Object.entries(kept)) {
      if (this.workspace.documentOf(path)) continue;
      for (const { line, text } of lines) found.push({ path, line, text });
    }
    return found.sort((a, b) => a.path.localeCompare(b.path) || a.line - b.line);
  }

  /** Show Marks: the project's marks in the picker, each with its file, line, and text. */
  showMarks() {
    if (this.menu) this.closeMenu(false);
    if (!this.picker.isOpen) this.focusBefore = document.activeElement;
    this.picker.open({
      label: "Marks", value: "", placeholder: "Search marks by file or text",
      source: (query) => {
        const rows = [];
        for (const m of this.#allMarks()) {
          const name = `${baseName(m.path)}:${m.line}`;
          const match = query ? fuzzy(query.trim(), `${name} ${m.text.trim()}`) : { score: 0, hits: [] };
          if (!match) continue;
          rows.push({ label: name, detail: m.text.trim(), score: match.score,
            run: () => this.goTo({ path: m.path, line: m.line, column: 1 }) });
        }
        if (query) rows.sort((a, b) => b.score - a.score);
        return { rows: rows.slice(0, pickerRows), empty: "No marks: Toggle Mark sets one on the cursor's line" };
      },
    });
  }

  /** Clears the current file's marks, with Undo while the toast shows. */
  async clearMarks() {
    const doc = this.workspace.current;
    if (!doc) return;
    const before = savedMarks(doc.state);
    if (before.length === 0) return;
    this.view.dispatch({ effects: setMarks.of([]) });
    const id = doc.id;
    this.#toast("Marks cleared", { action: "Undo", onAction: () => {
      const again = this.workspace.document(id);
      if (!again) return;
      this.#dispatchTo(id, (state) => ({ effects: setMarks.of(restoredMarks(state.doc, before)) }));
      this.#keepMarks(again.path, this.workspace.document(id).state);
    } });
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
    if (what === "project") this.recent = this.#savedRecent()[this.workspace.root] ?? [];
    if (what === "project" || what === "documents") this.#showProblemsInDocuments();
    if (what === "documents" || what === "current") this.#noteRecent();
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
    for (const tab of this.panelTabs) {
      tab.setAttribute("aria-selected", String(tab.dataset.view === this.panelView));
      tab.tabIndex = tab.dataset.view === this.panelView ? 0 : -1;
    }
    this.logView.hidden = this.panelView !== "console";
    this.problemsView.hidden = this.panelView !== "problems";
    this.search.element.hidden = this.panelView !== "search";
    this.filterField.hidden = this.panelView !== "console";
    this.clearButton.hidden = this.panelView !== "console";
  }

  /** Everything that shows the server: Play and Stop, the banner, the status bar. */
  renderServer() {
    this.renderTop();
    this.renderBanner();
    this.renderStatus();
  }

  renderLayout() {
    this.renderMenuBar();
    const narrow = this.narrow.matches;
    this.mainArea.classList.toggle("no-side", !this.layout.side);
    this.mainArea.classList.toggle("side-open", narrow && this.sideOpen);
    this.side.hidden = narrow ? !this.sideOpen : !this.layout.side;
    this.#fitPanel();
    this.content.classList.toggle("no-panel", !this.layout.panel);
    this.content.classList.toggle("max-panel", this.layout.panel && this.maximized);
    this.panel.hidden = !this.layout.panel;
    this.sideToggle.setAttribute("aria-pressed", String(this.sideShown));
    this.panelToggle.setAttribute("aria-pressed", String(this.layout.panel));
    this.sideToggle.title = this.#titled("Side Bar", "toggleFileList");
    this.panelToggle.title = this.#titled("Panel", "toggleConsole");
    this.panelClose.title = this.#titled("Close Panel", "toggleConsole");
    const label = this.maximized ? "Restore Panel" : "Maximize Panel";
    this.maxButton.replaceChildren(icon(this.maximized ? "down" : "max"));
    this.maxButton.title = label;
    this.maxButton.setAttribute("aria-label", label);
  }

  // §2: Play reads Update while a plan plays; both run buttons need a server.
  renderTop() {
    this.renderMenuBar();
    const playing = this.playing;
    const enabled = this.connected && this.workspace.root !== null;
    const label = playing && enabled ? "Update" : "Play";  // Update only where Play can update
    this.playLabel.textContent = label;
    // A disabled Play says why (patterns.md §2).
    const entry = this.workspace.entry;  // which file Play plays, named where Play is
    this.playButton.title = enabled ? this.#titled(entry ? `${label} ${baseName(entry)}` : label, "play")
      : this.workspace.root === null ? "Open a Folder to Play" : "Not Connected to espresso-server";
    this.stopButton.title = this.#titled("Stop", "stop");
    this.playButton.disabled = !enabled;
    this.optionsButton.disabled = this.playButton.disabled;  // one control: both halves alike
    this.stopButton.disabled = !playing;
    this.pageButton.hidden = !this.page.port;
    this.pageButton.title = this.#titled("Show Page", "showPage");
    this.commandCenter.replaceChildren(icon("search"), h("span", {}, this.workspace.name || "espresso"));
    this.commandCenter.title = this.#titled("Go to File", "switchBuffer");
    this.commandCenter.setAttribute("aria-label", `Go to File: ${this.workspace.name || "espresso"}`);
  }

  // §5.10: a missing or lost server is a state, shown across the window until it answers.
  renderBanner() {
    const stopped = this.serverStopped !== null && !this.connected;
    const absent = stopped || (this.serverReported && !this.connected && this.sidecar !== null);
    this.banner.hidden = !absent;
    if (!absent) return;
    const at = this.serverAddress ? ` at ${this.serverAddress}` : "";
    const message = stopped ? `espresso-server stopped${this.serverStopped.line ? `: ${this.serverStopped.line}` : ""}`
      : this.wasConnected ? `No reply from espresso-server${at}` : `Not connected to espresso-server${at}`;
    this.banner.replaceChildren(icon("warning"),
      h("span", {}, message),
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
      this.tree.replaceChildren(h("div", { class: "none" }, "The folder is empty"),
        h("button", { class: "btn flat", style: "margin: 0 var(--gapTight)", onclick: () => this.newFile("") }, "New File…"));
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
      path === ws.entry ? h("span", { class: "tag" }, "entry") : null);
    });
    this.tree.replaceChildren(...rows);
  }

  #treeKey(e, i) {
    const files = this.workspace.files;
    if (e.key === "Escape" && this.narrow.matches && this.sideOpen) {
      e.preventDefault();
      this.toggle("side");
      return;
    }
    if (e.key === "Delete") {
      e.preventDefault();
      this.trashFile(files[i]);
      return;
    }
    // Undo, with the tree's focus, takes back a move to the trash while its toast shows.
    if (this.#trashing && (e.ctrlKey || e.metaKey) && !e.shiftKey && e.key.toLowerCase() === "z") {
      e.preventDefault();
      this.#undoTrash();
      return;
    }
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
    this.renderMenuBar();
    const ws = this.workspace;
    // Two open files of one name: each tab names its folder too, dim (VS Code's).
    const counts = new Map();
    for (const d of ws.documents) counts.set(baseName(d.path), (counts.get(baseName(d.path)) ?? 0) + 1);
    const tabs = ws.documents.map((doc) => {
      const current = doc.id === ws.currentId && !this.page.current;
      const dirty = ws.isDirty(doc);
      const name = baseName(doc.path);
      const close = h("button", {
        class: "close", title: this.#titled("Close", "closeBuffer"), "aria-label": `Close ${name}`, tabindex: "-1",
        onclick: (e) => {
          e.stopPropagation();
          this.close(doc.id);
        },
      }, icon("close"));
      return h("div", {
        class: `tab${current ? " current" : ""}${dirty ? " unsaved" : ""}`, "data-id": doc.id,
        role: "tab",
        "aria-selected": String(current),
        tabindex: current ? "0" : "-1",
        title: dirty ? `${doc.path} (unsaved)` : doc.path,
        onclick: () => this.showDocument(doc.id),
        onauxclick: (e) => {
          if (e.button === 1) this.close(doc.id);
        },
        onkeydown: (e) => this.#tabKey(e, doc.id),
      },
      h("span", { class: "label", "data-label": name }, name,
        counts.get(name) > 1 && doc.path.includes("/")
          ? h("span", { class: "dim" }, ` ${doc.path.slice(0, doc.path.lastIndexOf("/"))}`) : null),
      h("span", { class: "slot" },
        dirty ? h("span", { role: "img", "aria-label": "Unsaved Changes", title: "Unsaved Changes" }, icon("dot", "dot")) : null,
        close));
    });
    if (this.page.open) tabs.push(this.#pageTab());
    this.tabs.replaceChildren(...tabs);
    // The current tab in view, and a cue at the strip's end when more tabs than fit are open.
    this.tabs.querySelector(".tab.current")?.scrollIntoView({ inline: "nearest", block: "nearest" });
    this.tabs.classList.toggle("overflows", this.tabs.scrollWidth > this.tabs.clientWidth + 1);
  }

  // The tab strip's keys, the page's tab among the files' (the APG's tabs): the arrows show the next tab,
  // Delete closes the focused one.
  #tabKey(e, id) {
    const tabs = [...this.tabs.querySelectorAll(".tab")];
    const i = tabs.findIndex((t) => (id === "page" ? t.classList.contains("page-tab") : Number(t.dataset.id) === id));
    if (e.key === "Delete") {
      e.preventDefault();
      if (id === "page") this.closePage();
      else this.close(id);
      return;
    }
    const to = { ArrowRight: i + 1, ArrowLeft: i - 1, Home: 0, End: tabs.length - 1 }[e.key];
    if (to === undefined || to < 0 || to >= tabs.length) return;
    e.preventDefault();
    const target = tabs[to];
    if (target.classList.contains("page-tab")) this.showPage();
    else this.showDocument(Number(target.dataset.id));
    const page = target.classList.contains("page-tab");
    (page ? this.tabs.querySelector(".page-tab") : this.tabs.querySelector(`.tab[data-id="${target.dataset.id}"]`))?.focus();
  }

  // §5, §11: the current document, or the empty state.
  renderEditor() {
    const doc = this.workspace.current;
    const page = this.page.open && this.page.current;
    this.view.dom.hidden = !doc || page;
    this.empty.hidden = Boolean(doc) || page;
    this.pageView.hidden = !page;
    if (page) {
      this.#renderPage();
      return;
    }
    if (doc) {
      if (this.view.state !== doc.state) {
        this.view.setState(doc.state);
        this.#watchMode();
      }
      return;
    }
    const noProject = this.workspace.root === null;
    const shortcuts = (noProject
      ? [["Open Folder…", "openFolder"], ["Show All Commands…", "showAllCommands"], ["Toggle Side Bar", "toggleFileList"]]
      : [["Go to File…", "switchBuffer"], ["Show All Commands…", "showAllCommands"], ["Toggle Side Bar", "toggleFileList"],
        ["Toggle Panel", "toggleConsole"]])
      .map(([label, command]) => [label, this.label(command)]).filter(([, keys]) => keys);
    this.empty.replaceChildren(h("div", { class: "center" },
      h("img", { src: new URL("./mark.png", import.meta.url).href, height: "112", alt: "" }),
      h("div", { class: "heading" },
        h("h1", {}, noProject ? "No Folder Open" : "No File Open"),
        h("p", {}, noProject ? "Open a project folder to edit and play its files"
          : this.sideShown ? `Open a file of ${this.workspace.name} from the side bar, or go to one by name`
            : `Show the side bar to open a file of ${this.workspace.name}, or go to one by name`)),
      noProject ? h("button", { class: "btn primary", onclick: () => this.openFolder() }, "Open Folder…")
        : this.workspace.files.length === 0
          ? h("button", { class: "btn primary", onclick: () => this.newFile("") }, "New File…")
          : this.sideShown ? null : h("button", { class: "btn primary", onclick: () => this.toggle("side") }, "Show Side Bar"),
      // The folders opened before, as VS Code's Welcome and GNOME Text Editor's Open list them.
      noProject && this.#recentProjects().length ? h("div", { class: "recent", role: "list", "aria-label": "Recent Folders" },
        h("div", { class: "group" }, "Recent Folders"),
        this.#recentProjects().slice(0, 5).map((root) => h("button", { class: "row", role: "listitem", title: root,
          onclick: () => this.openProject(root) }, h("span", { class: "name" }, projectName(root)),
        h("span", { class: "dim" }, root)))) : null,
      h("div", { class: "watermark" },
        shortcuts.flatMap(([label, keys]) => [h("span", { class: "label" }, label),
          h("span", {}, keyCaps(keys).map((k) => h("kbd", {}, k)))]))));
  }

  // §7: the console's entries, an error's place a link to it, through the filter.
  renderLog() {
    const filter = this.filter.trim().toLowerCase();
    const shown = filter ? this.log.filter((e) => e.text.toLowerCase().includes(filter)) : this.log;
    if (shown.length === 0) {
      this.logView.replaceChildren(h("div", { class: "none" }, filter ? "No messages match the filter" : "No messages"));
      return;
    }
    this.logView.replaceChildren(...shown.map((entry) => this.#logEntry(entry)));
    this.logView.scrollTop = this.logView.scrollHeight;
  }

  #logEntry(entry) {
    const place = entry.location && where(entry.location);
    const text = place && entry.text.startsWith(place)
      ? [h("a", { href: "#", title: `Go to ${place}`, onclick: (e) => {
        e.preventDefault();
        this.goTo(entry.location);
      } }, place), entry.text.slice(place.length)]
      : [entry.text];
    return h("div", { class: `msg ${entry.kind}` }, icon(entry.kind === "error" ? "error" : "info"), h("span", {}, text));
  }

  // §7: Problems, each row a link to its place; the tab's badge counts them.
  #announcedProblems = 0;

  renderProblems() {
    const count = this.problems.length;
    this.problemsBadge.hidden = count === 0;
    this.problemsBadge.textContent = String(count);
    this.panelTabs[1].setAttribute("aria-label", `Problems (${count})`);
    if (count === 0) {
      this.problemsView.replaceChildren(h("div", { class: "none" }, "No problems"));
      return;
    }
    this.problemsView.replaceChildren(...this.problems.map((problem) => {
      const severity = problem.severity ?? "error";
      return h("div", {
        class: `msg ${severity} problem`, role: "option", "aria-selected": "false", title: `Go to ${where(problem)}`,
        "aria-label": `${severity === "warning" ? "Warning" : "Error"}: ${problem.message}, ${where(problem)}`,
        onclick: () => this.goTo(problem),
      }, icon(severity), h("span", {}, problem.message), h("span", { class: "where" }, where(problem)));
    }));
    rove([...this.problemsView.querySelectorAll(".problem")]);
    if (count !== this.#announcedProblems) this.announce(`${count} ${count === 1 ? "problem" : "problems"}`);
    this.#announcedProblems = count;
  }

  // §8: the server on the left, problems after it; the editor's position and key scheme on the right.
  renderStatus() {
    this.renderMenuBar();
    const s = this.status;
    const items = [];
    if (this.connected) {
      this.serverItem = h("button", {
        class: "item", "data-item": "server", title: "Server Status", "aria-haspopup": "dialog",
        onclick: () => this.#toggleMenu(this.serverItem, () => this.#serverMenu()),
      }, h("span", { class: "ok" }, icon("dot")), `${serverName(s)} · ${Math.round(busiest(s) * 100)}%`);
      items.push(this.serverItem);
      items.push(h("div", { class: "item hide-narrow", title: "Tempo and Position (Bar.Beat)" },
        `${tempoText(s)} · ${beatText(s)}`));
      items.push(h("div", { class: "item hide-narrow", title: "The Playing Plan" }, s.plan === 0 ? "No Plan" : `Plan ${s.plan}`));
    } else if (!this.serverReported) {
      items.push(h("div", { class: "item" }, icon("ring"), "Connecting…"));
    } else if (this.wasConnected) {
      items.push(h("div", { class: "item" }, h("span", { class: "warn" }, icon("warning")), "espresso-server · No reply"));
    } else {
      items.push(h("div", { class: "item" }, icon("ring"), "Not connected"));
    }
    if (this.workspace.root !== null) {
      const warnings = this.problems.filter((p) => p.severity === "warning").length;
      const errors = this.problems.length - warnings;
      const counts = `${errors} ${errors === 1 ? "error" : "errors"}, ${warnings} ${warnings === 1 ? "warning" : "warnings"}`;
      items.push(h("button", {
        class: "item", "data-item": "problems", title: this.#titled("Problems", "showProblems"), "aria-label": counts,
        onclick: () => this.showPanelView("problems"),
      }, icon("error"), String(errors), icon("warning"), String(warnings)));
    }
    items.push(h("div", { class: "grow" }));
    const doc = this.workspace.current;
    // A reference says it is read-only, where the editor's state shows (the HIG names read-only a state to show).
    if (doc?.reference) items.push(h("div", { class: "item", "data-item": "readonly", title: "This Reference Cannot Be Edited" }, "Read-Only"));
    if (doc) {
      const head = doc.state.selection.main.head;
      const line = doc.state.doc.lineAt(head);
      items.push(h("button", { class: "item", "data-item": "position", title: this.#titled("Go to Line", "goToLine"),
        onclick: () => this.runCommand("goToLine") }, `Ln ${line.number}, Col ${head - line.from + 1}`));
    }
    const scheme = schemes[this.scheme].label + (this.scheme === "vim" ? ` · ${this.vimMode.toUpperCase()}` : "");
    this.schemeItem = h("button", {
      class: "item hide-narrow", "data-item": "scheme", title: "Key Scheme", "aria-haspopup": "menu",
      onclick: () => this.#toggleMenu(this.schemeItem, () => this.#schemeMenu()),
    }, scheme);
    items.push(this.schemeItem);
    // The bar is drawn again with each status: focus, and an open menu's button, stay with their item.
    const focused = this.statusBar.contains(document.activeElement) ? document.activeElement.dataset.item : null;
    const anchored = this.menu && this.statusBar.contains(this.menuAnchor) ? this.menuAnchor.dataset.item : null;
    this.statusBar.replaceChildren(...items);
    const named = (key) => this.statusBar.querySelector(`[data-item="${key}"]`);
    if (focused && named(focused)) named(focused).focus();
    if (anchored && named(anchored)) {
      this.menuAnchor = named(anchored);
      this.menuAnchor.setAttribute("aria-expanded", "true");
    }
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
        row("Missed Deadlines", String(s.missed)),
        row("Xruns", String(s.xruns)),
        row("Drops", String(s.drops)),
        row("Overflows", String(s.overflows)),
        row("Plan", s.plan === 0 ? "None" : String(s.plan)),
      ],
    };
  }

  // §5.8: the key scheme, on the status bar's right (tui.md §8.2).
  #schemeMenu() {
    return {
      label: "Key scheme", place: "right: 4px; bottom: 24px",
      items: schemeNames.map((name) => ({
        label: schemes[name].label, checked: this.scheme === name, role: "menuitemradio",
        action: () => this.setScheme(name),
      })),
    };
  }

  #toggleMenu(anchor, build) {
    const same = this.menuAnchor === anchor;
    if (this.menu) this.closeMenu(same);
    if (!same) this.#openMenu(anchor, build());
  }

  #openMenu(anchor, { label, place, items }, { context = false } = {}) {
    const actionable = items.filter((i) => i !== "separator" && i.head === undefined && i.value === undefined);
    // Access keys, as GTK's menus have them (the HIG's Menus): the letter chooses its item; Alt shows them.
    const keyed = isMac ? actionable.map((i) => i.label) : withAccessKeys(actionable.map((i) => i.label));
    const keyOf = new Map(actionable.map((item, n) => [item, keyed[n]]));
    const labelled = (item) => {
      const text = keyOf.get(item) ?? item.label;
      const at = text.indexOf("&");
      if (at < 0) return [text];
      return [text.slice(0, at), h("span", { class: "key" }, text[at + 1]), text.slice(at + 2)];
    };
    const rows = [];
    let group = null;  // a radio set under its heading (the APG's menu: role=group, named by the heading)
    for (const item of items) {
      if (item === "separator") {
        group = null;
        rows.push(h("div", { class: "sep", role: "separator" }));
        continue;
      }
      if (item.head !== undefined) {
        const id = `menu-head-${this.#menuIds++}`;
        rows.push(h("div", { class: "head", id, role: "presentation" }, item.head, item.small ? h("small", {}, item.small) : null));
        group = h("div", { role: "group", "aria-labelledby": id });
        rows.push(group);
        continue;
      }
      let row;
      if (item.value !== undefined) {
        row = h("div", { class: "row info", role: actionable.length ? "presentation" : "listitem" },
          h("span", {}, item.label), h("span", { class: "v" }, item.value));
      } else {
        const checkable = item.checked !== undefined;
        const action = item.action ?? (() => this.runCommand(item.command, item.target));
        const keys = item.command ? this.label(item.command) : "";
        const access = keyOf.get(item)?.match(/&(.)/)?.[1].toLowerCase();
        row = h("button", {
          class: `row${checkable ? " checkable" : ""}`, role: item.role ?? "menuitem", tabindex: "-1",
          disabled: item.enabled === false, "aria-checked": checkable ? String(item.checked) : null,
          "aria-keyshortcuts": access ? access : null, "data-key": access ?? null,
          onclick: () => {
            this.closeMenu(false);
            action();
          },
          // The pointer's row takes focus, so one row is lit, as the picker's is.
          onpointermove: (e) => {
            if (document.activeElement !== e.currentTarget && !e.currentTarget.disabled) e.currentTarget.focus();
          },
        },
        checkable ? h("span", { class: "check" }, item.checked ? icon("check") : null) : null,
        h("span", { class: "grow" }, labelled(item)), h("span", { class: "v" }, keys));
      }
      (group ?? { append: (r) => rows.push(r) }).append(row);
    }
    // Nothing to choose (the server's counters): a popover with a list, not a menu (the HIG's Popovers).
    const role = actionable.length ? "menu" : "dialog";
    const menu = h("div", { class: "menu", role, "aria-label": label, style: place, tabindex: "-1" },
      role === "dialog" ? h("div", { role: "list", "aria-label": label }, rows) : rows);
    this.menu = menu;
    menu.addEventListener("keydown", (e) => {
      const buttons = [...menu.querySelectorAll("button.row:not([disabled])")];
      if (e.key === "Alt") menu.classList.add("keys");
      if (e.key === "Tab") {  // the APG's menu: Tab closes it
        e.preventDefault();
        this.closeMenu(true);
        return;
      }
      if (e.key.length === 1 && !e.ctrlKey && !e.metaKey) {
        const chosen = buttons.find((b) => b.dataset.key === e.key.toLowerCase());
        if (chosen) {
          e.preventDefault();
          chosen.click();
        }
        return;
      }
      const i = buttons.indexOf(document.activeElement);
      const to = { ArrowDown: i + 1, ArrowUp: i - 1, Home: 0, End: buttons.length - 1 }[e.key];
      if (to === undefined || buttons.length === 0) return;
      e.preventDefault();
      buttons[(to + buttons.length) % buttons.length].focus();
    });
    menu.addEventListener("keyup", (e) => {
      if (e.key === "Alt") menu.classList.remove("keys");
    });
    this.menuAnchor = anchor;
    this.menuIsContext = context;  // a context menu's anchor is what it acts on, not a button that expands
    if (!context) anchor.setAttribute("aria-expanded", "true");
    this.menuTarget = context ? anchor : null;
    this.menuTarget?.classList.add("context-target");
    this.window.append(this.menu);
    // Opened by the pointer, the menu itself has focus and no row is lit until one is pointed at or keyed to.
    const byPointer = window.event instanceof MouseEvent && (window.event.detail > 0 || window.event.type === "contextmenu");
    (byPointer ? this.menu : this.menu.querySelector("button.row:not([disabled])") ?? this.menu).focus();
    // A press outside the menu closes it.
    this.menuOutside = (e) => {
      if (!this.menu.contains(e.target) && !anchor.contains(e.target)) this.closeMenu(false);
    };
    document.addEventListener("pointerdown", this.menuOutside, true);
  }

  #menuIds = 0;

  closeMenu(refocus) {
    if (!this.menu) return;
    this.menuTarget?.classList.remove("context-target");
    this.menuTarget = null;
    this.menu.remove();
    this.menu = null;
    document.removeEventListener("pointerdown", this.menuOutside, true);
    const anchor = this.menuAnchor;
    this.menuAnchor = null;
    if (!this.menuIsContext) anchor.setAttribute("aria-expanded", "false");
    if (refocus && anchor.isConnected) anchor.focus();
  }

  // ---- Dialogs (design-guide.md §5.11): the alert, Keyboard Shortcuts, and About.

  /**
   * Shows `dialog` over the window until `close(value)` is called, which
   * resolves the promise. Focus stays in the dialog and starts on `first`;
   * Escape and a press outside it close it with `cancel`.
   */
  #modal(build, cancel) {
    return new Promise((resolve) => {
      if (this.menu) this.closeMenu(false);
      const before = document.activeElement;
      const close = (value) => {
        this.scrim.remove();
        this.scrim = null;
        before?.focus?.();
        resolve(value);
      };
      const { dialog, first } = build(close);
      this.scrim = h("div", { class: "scrim" }, dialog);
      this.scrim.addEventListener("keydown", (e) => {
        if (e.key === "Escape") {
          e.preventDefault();
          e.stopPropagation();
          close(cancel);
        }
        if (e.key === "Tab") {
          // Every control the dialog holds, in order (the APG's dialog pattern).
          const focusable = [...dialog.querySelectorAll(
            "button, input, select, textarea, a[href], [tabindex]:not([tabindex='-1'])")]
            .filter((el) => !el.disabled && el.offsetParent !== null);
          const i = focusable.indexOf(document.activeElement);
          e.preventDefault();
          focusable[(i + (e.shiftKey ? -1 : 1) + focusable.length) % focusable.length]?.focus();
        }
      });
      this.scrim.addEventListener("pointerdown", (e) => {
        if (e.target === this.scrim) close(cancel);
      });
      this.window.append(this.scrim);
      first.focus();
    });
  }

  /** The alert: Cancel, Discard, Save, with Save the default. Resolves to "cancel", "discard", or "save". */
  /**
   * The HIG's alert: a heading, its consequence, and responses, Cancel
   * first; the last response is the default. Answers the chosen response's
   * name, or "cancel".
   */
  ask({ heading, body, responses = [["Cancel", "cancel"], ["Discard", "discard", "destructive"], ["Save", "save", "primary"]] }) {
    return this.#modal((close) => {
      const buttons = responses.map(([label, choice, kind = ""]) =>
        h("button", { class: `btn ${kind}`, onclick: () => close(choice) }, label));
      const dialog = h("div", { class: "dialog", role: "alertdialog", "aria-modal": "true", "aria-labelledby": "alert-heading",
        "aria-describedby": "alert-body" },
        h("div", { class: "body" }, h("h2", { id: "alert-heading" }, heading), h("p", { id: "alert-body" }, body)),
        h("div", { class: "actions" }, buttons));
      return { dialog, first: buttons.at(-1) };
    }, "cancel");
  }

  // A dialog of `content` under `heading`, with Close.
  #notice(heading, id, content, className = "") {
    return this.#modal((close) => {
      const done = h("button", { class: "btn primary", onclick: () => close() }, "Close");
      const dialog = h("div", { class: `dialog ${className}`, role: "dialog", "aria-modal": "true", "aria-labelledby": id },
        h("div", { class: "body" }, h("h2", { id }, heading), content),
        h("div", { class: "actions" }, done));
      return { dialog, first: done };
    });
  }

  /** Keyboard Shortcuts: every command the key scheme binds, with its keys (tui.md §8.1). */
  showShortcuts() {
    // By the menu bar's menus, general keys first under General (patterns.md §25.4).
    const groups = menuOrder.map((menu) => [menu, commandTable
      .filter((c) => (c.general ? "General" : c.menu) === menu)
      .map((c) => [c.label, bindingLabels(this.scheme, c.name)])
      .filter(([, keys]) => keys.length)]).filter(([, rows]) => rows.length);
    return this.#notice("Keyboard Shortcuts", "shortcuts-heading", h("div", {},
      h("p", {}, `The ${schemes[this.scheme].label} key scheme`),
      h("div", { class: "shortcuts", tabindex: "0", role: "table", "aria-label": "Shortcuts" },
        groups.map(([menu, rows]) => h("div", { role: "rowgroup", "aria-label": menu },
          h("div", { class: "group", role: "row" }, h("span", { role: "columnheader" }, menu)),
          rows.map(([title, keys]) => h("div", { class: "row", role: "row" },
            h("span", { role: "cell" }, title),
            h("span", { role: "cell", class: "bindings" }, keys.flatMap((label, n) =>
              [n > 0 ? h("span", { class: "or" }, "or") : null, ...keyCaps(label).map((k) => h("kbd", {}, k))])))))))),
      "wide");
  }

  /** About: the editor's versions and its license. */
  showAbout() {
    const hello = this.hello;
    // The HIG's writing: no period ending a single line, and the version first, in the user's terms.
    return this.#notice("espresso", "about-heading", h("div", {},
      h("p", {}, hello ? `Version ${hello.client}` : "The client is not running"),
      hello ? h("p", { class: "dim" }, `Protocol ${hello.protocol} · IR metadata ${hello.metadata}`) : null,
      h("p", {}, "Licensed under the GNU Affero General Public License, version 3 or later")));
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
