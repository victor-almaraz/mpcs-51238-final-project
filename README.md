# proj

![tests](docs/badges/tests.svg)

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

The test badge records the last local run of the suite. The `test-badge` target builds the tests, runs them, counts the results from a JUnit report (`build/Testing/test-badge.xml`), rewrites `docs/badges/tests.svg`, and fails if any test fails:

```bash
cmake --build build --target test-badge
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

The editor opens `PROJECT_DIR` (default: the current directory) and its `main.lisp`, if present. Every command is reachable with the mouse: run controls in the header bar, file actions and dialogs in the main menu (≡, or F10), and editing actions in the context menu (right-click, or Shift+F10). F1 lists the keyboard shortcuts of the selected scheme. Key bindings, fade, quantization, and pane visibility are set in Preferences and apply immediately. Key bindings follow the scheme selected in `$XDG_CONFIG_HOME/proj/settings.lisp` (default `~/.config/proj/settings.lisp`):

```lisp
(settings
  :keys            :emacs   ; :emacs, :cua, :vim, or :nano
  :file-list       #t
  :console         #t
  :fade            50ms
  :quant           :none    ; :none, :beat, or :bar
  :rebuild-on-save #f
  :theme           :terminal)
```

### Themes

| Theme | Setting |
|---|---|
| Terminal (default): the terminal's own palette | `:terminal` |
| Adwaita Dark | `:adwaita-dark` |
| Adwaita Light | `:adwaita-light` |
| Solarized Dark | `:solarized-dark` |
| Solarized Light | `:solarized-light` |
| Nord | `:nord` |
| High Contrast | `:high-contrast` |

Every theme assigns colors to the same roles: window, secondary text, rules, raised surfaces, fields, accent, destructive actions, selection, status, and the ten syntax categories. Truecolor themes keep at least 4.5:1 contrast for body text and 3:1 for secondary text, syntax, and status colors (7:1 in High Contrast), and fall back to 256 or 16 colors on terminals without truecolor support. The theme can be changed in Preferences; the change applies immediately.

The × button in the header bar quits; in the `emacs` scheme, so does `C-x C-c`. In every scheme, F5 plays, F6 stops, and F8 evaluates the form before the cursor.

Play compiles the project's `main.lisp` with unsaved edits (without one, the file being edited), submits the result to the server, and reports compile errors in the console; clicking an error moves the cursor to it. Evaluate Form compiles the project the same way. A failed compilation leaves the running piece unchanged.

## Compiler

```bash
build/bin/proj-client --compile FILE.lisp [--root DIR] [--entry NAME] [--seed N]
```

Compiles the piece named `NAME` (default `main`) in `FILE.lisp` and writes its IR to standard output; errors go to standard error as `file:line:column: message`, and the exit status is 1. `--seed N` takes a whole number from 0 to 2⁶⁴ − 1; any other value, or an unknown option, exits with status 2. Warnings about likely audio hazards (loops that grow, very large gains, aliasing frequencies) go to standard error and do not change the exit status. `docs/language.md` describes the supported language. The IR can be piped to the server:

```bash
build/bin/proj-client --compile examples/delay.lisp | build/bin/proj-server --play -
```

## Examples

`examples/` holds pieces and the IR each compiles to; the test suite checks that they match. `examples/README.md` describes them.

| Example | Input | Description |
|---|---|---|
| `monosynth.lisp` | Any MIDI keyboard | Two gliding oscillators, ladder filter on controllers 74 and 71, filter and amplitude envelopes |
| `reverb.lisp` | Audio input 1 | Stereo Schroeder reverb with Freeverb tunings |
| `delay.lisp` | Audio inputs 1 and 2 | 375 ms feedback echo with damping and tape wow |

## Server

`proj-server` runs the audio engine. It listens for the client on `127.0.0.1:57190`, validates submitted IR against `schema/ir.schema.json` and the primitive tables, builds plans, and swaps them in with state adoption, crossfades, and beat or bar quantization.

```bash
build/bin/proj-server [--port N] [--device NAME] [--sample-rate R] [--block-size B]
                      [--inputs N] [--outputs N] [--no-audio] [--verbose] [--play IR_FILE]
build/bin/proj-server --list-devices
```

| Option | Default | Effect |
|---|---|---|
| `--device NAME` | System default | Audio device, as listed by `--list-devices` |
| `--sample-rate R` | `48000` | Requested sample rate; the device may choose another |
| `--block-size B` | `256` | Requested buffer size, in samples |
| `--inputs N`, `--outputs N` | `2`, `2` | Device channels. Without input permission, the server continues with no inputs |
| `--no-audio` | Off | Runs the engine on a timer instead of a device |
| `--verbose` | Off | Logs every message except `/counters`, and the socket receive buffer size |
| `--play IR_FILE` | None | Plays an IR file at startup as plan −1, until a client swaps in another plan. `-` reads standard input |

Numeric options take decimal numbers in the stated ranges; `--port`, `--block-size`, `--inputs`, and `--outputs` take whole numbers. Any other value exits with status 2. The first SIGINT or SIGTERM stops the server in order; a second one during shutdown exits at once with status 130.

Every `audioOut` limits its signal to its `:ceiling` (default full scale) and mutes itself if it receives infinite or NaN samples, reporting the node in the editor console. MIDI inputs are opened as they appear; `midiIn` and `midiOut` select devices by a case-insensitive substring of the device name. `oscIn` and `oscOut` use UDP ports on `127.0.0.1`.

### Offline render

```bash
build/bin/proj-server --render piece.ir.json --out piece.wav [--seconds S] [--sample-rate R]
                      [--block-size B] [--outputs N]
```

Plays an IR file (`-` for standard input) without a device and writes a 32-bit float WAV file (default 10 seconds, 48 kHz, 2 channels). The same IR and seed produce identical files. `docs/ir.md` describes the IR format.

## Stub server

`proj-stub-server` implements the client-server protocol without an audio engine. It accepts IR, schedules swaps on a 120 BPM 4/4 clock with the requested quantization, reports `/plan.active` and `/plan.retired`, sends zeroed counters ten times per second, and logs every message. It produces no sound.

```bash
build/bin/proj-stub-server [--port N] [--quiet]
```

The server listens on `127.0.0.1:57190` by default. The client connects on startup and again on Play if the server was not reachable.

## Documentation

```bash
cmake --build build --target docs
```

## License

AGPLv3. See `LICENSE`.
