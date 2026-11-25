// The GUI editor's window (tauri-editor.md §5.2): layout B in Carbon
// (brand/gui/design-guide.md §4–5, patterns.md §1–13). The top bar, the
// navigator, tabs over the editor, the bottom panel, and the status bar,
// over the workspace (workspace.js) and the sidecar (sidecar.js).
//
// Play and Stop come with step 5, the pickers and find with step 7
// (editor-refactor.md §4); until then they are drawn and disabled.

import { h, hasMod, keyLabel, keyCaps } from "./dom.js";
import { icon } from "./icons.js";
import { Sidecar } from "./sidecar.js";
import { Workspace, baseName, entryFile } from "./workspace.js";
import { editorState, EditorView } from "./codeEditor.js";

const layoutKey = "espresso.layout";
const projectKey = "espresso.project";
const logLimit = 1000;

class App {
  /** @type {Sidecar | null} */
  sidecar = null;
  /** @type {Workspace} */
  workspace;
  /** The server's last `status` (tauri-editor.md §4.6), or null. */
  status = null;
  /** Console entries: {kind, text}. */
  log = [];
  layout = { side: true, panel: true };
  #treeFocus = 0;

  constructor(root) {
    this.root = root;
    try {
      Object.assign(this.layout, JSON.parse(localStorage.getItem(layoutKey) ?? "{}"));
    } catch {
      // A damaged entry keeps the defaults.
    }
    this.#build();
    window.addEventListener("keydown", (e) => this.#onKey(e), true);
  }

  // ---- The skeleton, built once (§1).

  #build() {
    const iconButton = (name, label, keys, onclick) =>
      h("button", { class: "btn flat", title: keys ? `${label} (${keyLabel(keys)})` : label, "aria-label": label, onclick },
        icon(name));

    this.playButton = h("button", { class: "btn primary", title: "Play (F5)", disabled: true },
      icon("play"), h("span", { class: "label" }, "Play"));
    const top = h("header", { class: "top" },
      h("div", { class: "start" },
        this.playButton,
        h("button", { class: "btn primary arrow", title: "Play options", "aria-label": "Play options", disabled: true },
          icon("down")),
        h("button", { class: "btn flat", title: "Stop (Ctrl+.)", disabled: true }, icon("stop"),
          h("span", { class: "label" }, "Stop"))),
      this.commandCenter = h("div", { class: "command-center" }),
      h("div", { class: "end" },
        this.sideToggle = iconButton("sidebar", "Toggle side bar", "Mod+B", () => this.toggle("side")),
        this.panelToggle = iconButton("panel", "Toggle panel", "Mod+J", () => this.toggle("panel")),
        this.menuButton = iconButton("menu", "Main menu", "", () => this.openMenu())));
    this.menuButton.setAttribute("aria-haspopup", "menu");

    this.banner = h("div", { class: "banner", role: "status", hidden: true });

    this.sideTitle = h("div", { class: "title" });
    this.tree = h("div", { class: "tree", role: "tree", "aria-label": "Project files" });
    this.side = h("nav", { class: "side", "aria-label": "Project" }, this.sideTitle, this.tree);

    this.tabs = h("div", { class: "tabs", role: "tablist", "aria-label": "Open files" });
    this.editorHost = h("div", { class: "editor" });
    this.view = new EditorView({ parent: this.editorHost });
    this.empty = h("div", { class: "empty" });
    this.editorHost.append(this.empty);

    this.logView = h("div", { class: "log", role: "log", "aria-label": "Console" });
    this.problemsView = h("div", { class: "log", hidden: true }, h("div", { class: "none" }, "No problems"));
    const viewTab = (label, panelView, on) => {
      const tab = h("button", { class: "view", role: "tab", "aria-selected": String(on) }, label);
      tab.addEventListener("click", () => {
        for (const t of this.panelTabs) t.setAttribute("aria-selected", String(t === tab));
        this.logView.hidden = panelView !== this.logView;
        this.problemsView.hidden = panelView !== this.problemsView;
      });
      return tab;
    };
    this.panelTabs = [viewTab("Console", this.logView, true), viewTab("Problems", this.problemsView, false)];
    this.panel = h("section", { class: "panel", "aria-label": "Panel" },
      h("div", { class: "bar" },
        h("div", { role: "tablist", style: "display: contents" }, this.panelTabs),
        h("div", { class: "grow" }),
        h("div", { class: "tools" },
          iconButton("clear", "Clear console", "", () => this.clearLog()),
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
      (text) => editorState(text, (state) => this.#edited(state)));
    this.workspace.addEventListener("change", (e) => this.#changed(e.detail));
    this.renderAll();
    if (!window.__TAURI__) {
      this.addLog("error", "Not running in the editor: open it with cargo tauri dev");
      return;
    }
    this.#guardClose();
    try {
      this.sidecar = await Sidecar.start();
    } catch (error) {
      this.addLog("error", `The client could not start: ${error.message ?? error}`);
      return;
    }
    this.sidecar.on("status", (s) => {
      this.status = s;
      this.renderStatus();
    });
    this.sidecar.on("console", (entry) => this.addLog(entry.kind, entry.text));
    this.sidecar.on("close", (reason) => {
      this.sidecar = null;
      this.addLog("error", `The client stopped: ${reason}`);
    });
    await this.sidecar.request("hello");
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
    } catch (error) {
      if (quiet) localStorage.removeItem(projectKey);
      this.addLog("error", `Could not open ${root}: ${error.message}`);
    }
  }

  async openFile(path) {
    try {
      await this.workspace.openFile(path);
      this.view.focus();
    } catch (error) {
      this.addLog("error", `Could not open ${path}: ${error.message}`);
    }
  }

  /** Saves the document with `id`; false when the write failed. */
  async save(id) {
    const doc = this.workspace.document(id);
    if (!doc) return false;
    try {
      await this.workspace.save(id);
      return true;
    } catch (error) {
      this.addLog("error", `Could not save ${doc.path}: ${error.message}`);
      return false;
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

  toggle(region) {
    this.layout[region] = !this.layout[region];
    localStorage.setItem(layoutKey, JSON.stringify(this.layout));
    this.renderLayout();
  }

  addLog(kind, text) {
    this.log.push({ kind, text });
    if (this.log.length > logLimit) this.log.splice(0, this.log.length - logLimit);
    this.renderLog();
  }

  clearLog() {
    this.log = [];
    this.renderLog();
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

  // ---- Keys (patterns.md §14.3: cua).

  #onKey(e) {
    if (e.key === "Escape" && this.menu) {
      e.preventDefault();
      this.closeMenu(true);
      return;
    }
    if (!hasMod(e) || e.altKey || this.scrim) return;
    const key = e.key.toLowerCase();
    const run = {
      o: () => this.openFolder(),
      s: () => this.workspace.current && this.save(this.workspace.currentId),
      w: () => this.workspace.current && this.close(this.workspace.currentId),
      b: () => this.toggle("side"),
      j: () => this.toggle("panel"),
    }[key];
    if (!run || e.shiftKey) return;
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
    this.renderAll();
  }

  // ---- Rendering.

  renderAll() {
    this.renderLayout();
    this.renderTop();
    this.renderSide();
    this.renderTabs();
    this.renderEditor();
    this.renderLog();
    this.renderStatus();
    this.renderTitle();
  }

  renderLayout() {
    this.mainArea.classList.toggle("no-side", !this.layout.side);
    this.side.hidden = !this.layout.side;
    this.content.classList.toggle("no-panel", !this.layout.panel);
    this.panel.hidden = !this.layout.panel;
    this.sideToggle.setAttribute("aria-pressed", String(this.layout.side));
    this.panelToggle.setAttribute("aria-pressed", String(this.layout.panel));
  }

  renderTop() {
    this.commandCenter.replaceChildren(icon("search"), this.workspace.name || "espresso");
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

  // §7: the console's entries.
  renderLog() {
    if (this.log.length === 0) {
      this.logView.replaceChildren(h("div", { class: "none" }, "No messages"));
      return;
    }
    this.logView.replaceChildren(...this.log.map(({ kind, text }) =>
      h("div", { class: `msg ${kind}` }, icon(kind === "error" ? "error" : "info"), h("span", {}, text))));
    this.logView.scrollTop = this.logView.scrollHeight;
  }

  // §8: the server on the left, the editor's position and key scheme on the right.
  renderStatus() {
    const s = this.status;
    const items = [];
    if (s?.connected) {
      const busiest = Math.max(s.load, ...(s.threadLoads ?? []));
      items.push(h("div", { class: "item", title: s.server }, icon("dot"),
        `${s.server.split(" ")[0]} · ${Math.round(busiest * 100)}%`));
      items.push(h("div", { class: "item hide-narrow" }, `${tempoText(s)} · ${beatText(s)}`));
      items.push(h("div", { class: "item hide-narrow" }, s.plan === 0 ? "Plan —" : `Plan ${s.plan}`));
    } else {
      items.push(h("div", { class: "item" }, icon("ring"), "Not connected"));
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

  // ---- The main menu (patterns.md §2: at the top bar's end).

  openMenu() {
    if (this.menu) {
      this.closeMenu(true);
      return;
    }
    const doc = this.workspace.current;
    const item = (label, keys, action, enabled = true) => h("button", {
      class: "row", role: "menuitem", tabindex: "-1", disabled: !enabled,
      onclick: () => {
        this.closeMenu(false);
        action();
      },
    }, h("span", {}, label), h("span", { class: "v" }, keys ? keyLabel(keys) : ""));
    this.menu = h("div", { class: "menu", role: "menu", "aria-label": "Main menu", style: "top: 34px; right: 8px" },
      item("Open Folder…", "Mod+O", () => this.openFolder()),
      item("Save", "Mod+S", () => this.save(doc.id), Boolean(doc)),
      item("Close Editor", "Mod+W", () => this.close(doc.id), Boolean(doc)),
      h("div", { class: "sep", role: "separator" }),
      item(this.layout.side ? "Hide Side Bar" : "Show Side Bar", "Mod+B", () => this.toggle("side")),
      item(this.layout.panel ? "Hide Panel" : "Show Panel", "Mod+J", () => this.toggle("panel")));
    this.menu.addEventListener("keydown", (e) => {
      const items = [...this.menu.querySelectorAll(".row:not([disabled])")];
      const i = items.indexOf(document.activeElement);
      const to = { ArrowDown: i + 1, ArrowUp: i - 1, Home: 0, End: items.length - 1 }[e.key];
      if (to === undefined) return;
      e.preventDefault();
      items[(to + items.length) % items.length].focus();
    });
    this.menuButton.setAttribute("aria-expanded", "true");
    this.window.append(this.menu);
    this.menu.querySelector(".row:not([disabled])").focus();
    // A press outside the menu closes it.
    this.menuOutside = (e) => {
      if (!this.menu.contains(e.target) && !this.menuButton.contains(e.target)) this.closeMenu(false);
    };
    document.addEventListener("pointerdown", this.menuOutside, true);
  }

  closeMenu(refocus) {
    if (!this.menu) return;
    this.menu.remove();
    this.menu = null;
    document.removeEventListener("pointerdown", this.menuOutside, true);
    this.menuButton.setAttribute("aria-expanded", "false");
    if (refocus) this.menuButton.focus();
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
app.start().catch((error) => app.addLog("error", `The editor could not start: ${error.message ?? error}`));
