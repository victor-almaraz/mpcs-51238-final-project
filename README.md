# proj

A Linux-first workstation for acousmatic, aleatory, and automatic music. Pieces are written in a Lisp dialect, compiled to an intermediate representation (IR), and played by a native C++ engine.

`proj` is a placeholder name.

## Requirements

- CMake 3.25 or later
- A C++20 compiler (GCC 12+, Clang 15+)
- Git, for dependency downloads
- Linux: JACK headers (`pipewire-jack` or `libjack-jackd2-dev`), ALSA headers, and the JUCE Linux dependencies
- Optional: Ninja, Doxygen, Valgrind

All libraries (JUCE, FTXUI, nlohmann/json, json-schema-validator, Catch2) are downloaded by CMake into `third_party/` at configure time.

## Build

```bash
cmake -B build -S . -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The build produces `build/bin/proj-server`, `build/bin/proj-client`, and `build/bin/proj-stub-server`. Test executables are in `build/tests/`.

| Option | Default | Effect |
|---|---|---|
| `PROJ_BUILD_SERVER` | `ON` | Builds the server app and downloads JUCE |
| `PROJ_BUILD_CLIENT` | `ON` | Builds the client app |
| `PROJ_BUILD_TESTS` | `ON` | Builds the Catch2 suite |
| `PROJ_WARNINGS_AS_ERRORS` | `OFF` | Adds `-Werror` |

## Editor

```bash
build/bin/proj-client [--settings FILE] [--port N] [PROJECT_DIR [FILE...]]
```

The editor opens `PROJECT_DIR` (default: the current directory) and its `main.lisp`, if present. Every command is reachable from the toolbar menus with the mouse. Key bindings follow the scheme selected in `$XDG_CONFIG_HOME/proj/settings.lisp` (default `~/.config/proj/settings.lisp`):

```lisp
(settings
  :keys            :emacs   ; :emacs, :cua, :vim, or :nano
  :file-list       #t
  :console         #t
  :fade            50ms
  :quant           :none    ; :none, :beat, or :bar
  :rebuild-on-save #f)
```

In the `emacs` scheme, `C-x C-c` quits. In every scheme, F5 plays, F6 stops, and F8 evaluates the form before the cursor.

## Stub server

`proj-stub-server` implements the client-server protocol without an audio engine. It accepts IR, schedules swaps on a 120 BPM 4/4 clock with the requested quantization, reports `/plan.active` and `/plan.retired`, sends zeroed counters ten times per second, and logs every message. It produces no sound.

```bash
build/bin/proj-stub-server [--port N] [--quiet]
```

The server listens on `127.0.0.1:57190` by default. The client connects on startup and again on Play if the server was not reachable. Until the compiler exists, Play reads the project, reports read errors, and submits a graph with no nodes.

## Documentation

```bash
cmake --build build --target docs
```

## License

AGPLv3. See `LICENSE`.
