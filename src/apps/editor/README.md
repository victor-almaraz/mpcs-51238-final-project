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

The build fetches the web packages, writes `web/index.html`, and places the client and the server in `src-tauri/binaries/`. `cargo tauri dev` builds the Rust and opens the window, which starts `espresso-server` and then the client as its sidecar. Closing the window stops the server; so do Ctrl+C in the terminal and `kill`. When another `espresso-server` already listens on the default port, the editor uses it and leaves it running. When the server stops, a banner under the top bar says why, and Retry starts it again.

A piece whose project names a page (`:page-port` in `project.lisp`) shows it in a tab, "Page · NAME", the first time a plan with a page plays: the page served on `127.0.0.1` by the server, with Reload, Open in Window, and Open in Browser. Show Page, from the main menu or Show All Commands, opens it again.

`cargo test` in `src-tauri/` checks the starting and stopping of the server against the build tree's `build/bin/espresso-server`, or the one `ESPRESSO_SERVER` names, and headless mode against the client and server Tauri places beside the built executable.

## Command line

The application's one executable, `espresso` in the bundle (`target/debug/espresso-editor` from `cargo build`), chooses by its first argument:

| Command | Runs |
|---|---|
| `espresso [PROJECT]` | The editor, on PROJECT when one is named, with a server of its own |
| `espresso --headless [server options]` | The server alone, in headless mode, with `espresso-server`'s options |
| `espresso --headless --play PIECE [options]` | A piece live, without the editor, until Ctrl+C |
| `espresso --headless --render PIECE --out FILE.wav [options]` | A piece offline to a WAV file |
| `espresso --tui [client options]` | The terminal editor, with `espresso-client`'s options |

PIECE is IR (an `.ir.json` file, or `-` for standard input), a `.lisp` entry file, or a project directory, whose entry is `main.lisp`, else `NAME.lisp` in a directory `NAME`, else its only `.lisp` file other than `project.lisp`. A `.lisp` piece is compiled by the bundled client and given to the server on standard input, with the project as `--root` unless one is named. Ctrl+C and SIGTERM pass to the server or the terminal editor, and the command ends with its status. Open Folder… (Ctrl+O, or ⌘O on macOS) opens a project; the window opens the last one again when it starts.

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
| Go to File: the project's files; `name:12` opens at line 12, `@` lists definitions | Ctrl+P (⌘P) | C-x b | `:b` | M-B |
| Show All Commands, each with its keys | Ctrl+Shift+P (⌘⇧P) | M-x | | |
| Go to Line | Ctrl+G (⌃G) | M-g | `:12` | M-G |
| Find | Ctrl+F (⌘F) | C-s | `/` | ^W |
| Replace | Ctrl+H (⌘H) | | | |
| Keyboard Shortcuts | F1 | F1 | F1 | F1 |
