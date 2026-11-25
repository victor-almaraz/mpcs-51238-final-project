// The client as the editor's sidecar (tauri-editor.md §4): JSON-RPC 2.0
// requests written to its standard input, one a line, and the responses
// and notifications it writes back.

/** A request the sidecar refused: its JSON-RPC error. */
export class SidecarError extends Error {
  /** @param {{code: number, message: string}} error */
  constructor(error) {
    super(error.message);
    this.code = error.code;
  }
}

export class Sidecar {
  #child = null;
  #nextId = 1;
  /** @type {Map<number, {resolve: Function, reject: Function}>} */
  #pending = new Map();
  /** @type {Map<string, Function[]>} */
  #listeners = new Map();

  /**
   * Starts `espresso-client --serve` through Tauri's shell plugin.
   * @returns {Promise<Sidecar>}
   */
  static async start() {
    const { Command } = window.__TAURI__.shell;
    const command = Command.sidecar("binaries/espresso-client", ["--serve"]);
    const sidecar = new Sidecar();
    command.stdout.on("data", (line) => sidecar.#receive(line));
    command.stderr.on("data", (line) => console.warn("sidecar:", line));
    command.on("close", ({ code, signal }) => sidecar.#close(`exited with status ${code}, signal ${signal}`));
    command.on("error", (error) => sidecar.#close(String(error)));
    sidecar.#child = await command.spawn();
    return sidecar;
  }

  /**
   * Sends a request; resolves with its result, or rejects with a SidecarError.
   * @param {string} method
   * @param {object} [params]
   */
  request(method, params = {}) {
    if (!this.#child) return Promise.reject(new SidecarError({ code: -1, message: "the sidecar has stopped" }));
    const id = this.#nextId++;
    const answer = new Promise((resolve, reject) => this.#pending.set(id, { resolve, reject }));
    this.#child.write(JSON.stringify({ jsonrpc: "2.0", id, method, params }) + "\n");
    return answer;
  }

  /**
   * Calls `fn(params)` for each notification named `method` (§4.6), and
   * `fn(reason)` for "close" when the sidecar stops.
   * @param {string} method
   * @param {Function} fn
   */
  on(method, fn) {
    if (!this.#listeners.has(method)) this.#listeners.set(method, []);
    this.#listeners.get(method).push(fn);
  }

  #emit(method, value) {
    for (const fn of this.#listeners.get(method) ?? []) fn(value);
  }

  #receive(line) {
    let message;
    try {
      message = JSON.parse(line);
    } catch {
      console.warn("sidecar: not JSON:", line);
      return;
    }
    if (!("id" in message)) {
      this.#emit(message.method, message.params);
      return;
    }
    const waiting = this.#pending.get(message.id);
    if (!waiting) return;
    this.#pending.delete(message.id);
    if ("error" in message) waiting.reject(new SidecarError(message.error));
    else waiting.resolve(message.result);
  }

  #close(reason) {
    this.#child = null;
    for (const { reject } of this.#pending.values()) reject(new SidecarError({ code: -1, message: reason }));
    this.#pending.clear();
    this.#emit("close", reason);
  }
}
