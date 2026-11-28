# The GUI editor

A desktop editor built with Tauri: a web interface in the system's webview, over the client run as its sidecar (`espresso-client --serve`). The web interface edits text and draws the editor; the sidecar reads and writes files, compiles the project, and talks to the server.

| Path | Holds |
|---|---|
| `web/` | The web interface: `index.html.in`, from which CMake writes `index.html` with the import map, and its scripts and stylesheet. Plain ES modules: no npm, no bundler, no build step |
| `web/vendor/` | CodeMirror 6 and the packages it imports, marked, and the fonts (Noto Sans, JetBrains Mono), fetched by CMake from the lists in `cmake/EditorWeb.cmake` |
| `web/docs/` | The references Help opens, `language.md` and `library.md`, copied by CMake from `docs/` |
| `templates/` | The projects New Project… starts from, described in `templates/README.md` |
| `src-tauri/` | Tauri's entry point (`src/main.rs`), its configuration (`tauri.conf.json`), and the window's permissions (`capabilities/default.json`) |
| `src-tauri/binaries/` | The client as the sidecar, `espresso-client-<target triple>`, and the server, `espresso-server-<target triple>`, placed by CMake |
| `src-tauri/resources/` | The templates and the examples the application bundles for New Project… and Open Example…, copied by CMake from `templates/` and `examples/` |

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

The build fetches the web packages, writes `web/index.html`, and places the client and the server in `src-tauri/binaries/`. `cargo tauri dev` builds the Rust and opens the window. `cargo tauri build` builds `target/release/espresso-editor`, and `cargo build` builds `target/debug/espresso-editor`. The committed `tauri.conf.json` sets `bundle.active` to false, so `cargo tauri build` makes no installable bundle.

`cargo test` in `src-tauri/` checks the starting and stopping of the server against the build tree's `build/bin/espresso-server`, or the one `ESPRESSO_SERVER` names, and headless mode against the client and server Tauri places beside the built executable.

[The GUI editor](../../../docs/guide/gui-editor.md) in the guide describes the editor's use: its window, projects, playing, pages, keys, preferences, and command line.
