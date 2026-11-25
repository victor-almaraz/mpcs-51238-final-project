// The GUI editor's page (tauri-editor.md §5): starts the sidecar and shows
// the server's status (editor-refactor.md §4, step 2), with a CodeMirror
// editor that the import map loads.

import { EditorState } from "@codemirror/state";
import { EditorView, keymap, lineNumbers, highlightActiveLine } from "@codemirror/view";
import { defaultKeymap, history, historyKeymap } from "@codemirror/commands";
import { Sidecar } from "./sidecar.js";

const status = document.getElementById("status");

new EditorView({
  parent: document.getElementById("editor"),
  state: EditorState.create({
    doc: "; espresso\n",
    extensions: [lineNumbers(), highlightActiveLine(), history(), keymap.of([...defaultKeymap, ...historyKeymap])],
  }),
});

/** The status bar's text for a `status` notification (§4.6). */
function describe(s) {
  if (!s.connected) return "No server";
  const tempo = s.bpm === null ? "— BPM" : `${s.bpm.toFixed(1).replace(/\.0$/, "")} BPM`;
  const plan = s.plan === 0 ? "Plan —" : `Plan ${s.plan}`;
  return `${s.server} · ${tempo} · ${plan}`;
}

async function start() {
  if (!window.__TAURI__) {
    status.textContent = "Not running in the editor: open it with cargo tauri dev";
    return;
  }
  const sidecar = await Sidecar.start();
  let reported = false;
  sidecar.on("status", (s) => {
    reported = true;
    status.textContent = describe(s);
    status.dataset.connected = String(s.connected);
  });
  sidecar.on("close", (reason) => {
    status.textContent = `The client stopped: ${reason}`;
  });
  const hello = await sidecar.request("hello");
  if (!reported) status.textContent = `espresso-client ${hello.client} · connecting…`;
}

start().catch((error) => {
  status.textContent = `The client could not start: ${error.message ?? error}`;
});
