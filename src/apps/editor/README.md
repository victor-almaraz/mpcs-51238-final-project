# The GUI editor

A desktop editor built with Tauri: a web interface in the system's webview, over the client run as its sidecar (`espresso-client --serve`). The web interface edits text and draws the editor; the sidecar reads and writes files, compiles the project, and talks to the server.

| Path | Holds |
|---|---|
| `web/` | The web interface: `index.html.in`, from which CMake writes `index.html` with the import map, and its scripts and stylesheet. Plain ES modules: no npm, no bundler, no build step |
| `web/vendor/` | CodeMirror 6 and the packages it imports, and the fonts (Noto Sans, JetBrains Mono), fetched by CMake from the lists in `cmake/EditorWeb.cmake` |
| `src-tauri/` | Tauri's entry point (`src/main.rs`), its configuration (`tauri.conf.json`), and the window's permissions (`capabilities/default.json`) |
| `src-tauri/binaries/` | The client as the sidecar, `espresso-client-<target triple>`, placed by CMake |

## Requirements

- What the C++ build needs, and CMake configured with `ESPRESSO_BUILD_EDITOR`, which is on with the client
- Rust 1.90 or newer, with Cargo
- Tauri's command-line tool: `cargo install tauri-cli --locked`
- On Linux, the WebKitGTK and libsoup development packages: `libwebkit2gtk-4.1-dev` and `libsoup-3.0-dev`

## Running

```bash
cmake -B build -S .
cmake --build build
cd src/apps/editor/src-tauri
cargo tauri dev
```

The build fetches the web packages, writes `web/index.html`, and places the client in `src-tauri/binaries/`. `cargo tauri dev` builds the Rust and opens the window, which starts the sidecar. The sidecar reaches the server on `127.0.0.1` at the default port; start `espresso-server` (or `espresso-server --no-audio`) for the status bar to show it connected. Without it, a banner under the top bar says so, and Retry tries again. Open Folder… (Ctrl+O, or ⌘O on macOS) opens a project; the window opens the last one again when it starts.

The key scheme is chosen from the status bar: CUA (the default), Emacs, Vim, or nano. In every scheme, F5 plays or updates, F6 stops, F7 and F9 toggle the side bar and the panel, F8 evaluates the form before the cursor, and F10 opens the main menu.

| Command | CUA | Emacs | Vim | nano |
|---|---|---|---|---|
| Save | Ctrl+S (⌘S) | C-x C-s | `:w` | ^O |
| Open Folder… | Ctrl+O (⌘O) | C-x C-f | `:e` | ^R |
| Undo | Ctrl+Z (⌘Z) | C-/ | `u` | M-U |
| Quit | Ctrl+Q (⌘Q) | C-x C-c | `:q` | ^X |
| Close the tab | Ctrl+W (⌘W) | C-x k | `:bd` | M-K |
| Play or Update | F5 | C-c C-c | `:play` | F5 |
| Stop | Ctrl+. (⌘.) | C-c C-s | `:stop` | F6 |
| Evaluate Form: the top-level form at the cursor | Ctrl+Enter (⌘Enter) | C-M-x | | |
| Evaluate Last Form: the form before the cursor | Shift+Enter, Ctrl+E (⌘E) | C-x C-e | `:eval`, C-x C-e | M-R |
| Problems | Ctrl+Shift+M (⌘⇧M) | | | |
