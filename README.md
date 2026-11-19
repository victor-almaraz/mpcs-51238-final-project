# espresso

![tests](docs/badges/tests.svg)
![coverage](docs/badges/coverage.svg)

espresso is a Linux-first workstation for acousmatic, aleatory, and automatic music, and the Lisp dialect its pieces are written in. Pieces compile to an intermediate representation (IR), which a native C++ engine plays.

## Requirements

- CMake 3.25 or later
- A C++20 compiler (GCC 12+, Clang 15+)
- Git, for dependency downloads
- Linux: JACK headers (`pipewire-jack` or `libjack-jackd2-dev`), ALSA headers, OpenGL headers (`libgl-dev`), and the JUCE Linux dependencies
- Optional: Ninja, Doxygen, Valgrind

All libraries (JUCE, FTXUI, nlohmann/json, json-schema-validator, Catch2, Blend2D, stb) are downloaded by CMake into `third_party/` at configure time.

## Build

```bash
cmake -B build -S . -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The badges record the last local run of the suite. `scripts/badges.sh` builds the project, runs the suite, rewrites both badges, and adds any missing badge line under this file's title:

```bash
scripts/badges.sh
```

| Badge | Build tree | Target | Content |
|---|---|---|---|
| `docs/badges/tests.svg` | `build/` | `test-badge` | Passed tests, counted from a JUnit report (`build/Testing/test-badge.xml`) |
| `docs/badges/coverage.svg` | `build-coverage/` | `coverage-badge` | Line coverage of `src/` and `include/`, from LLVM source-based coverage |

The coverage tree is configured with Clang and `-DESPRESSO_COVERAGE=ON` on first use; `LLVM_BIN` selects the directory of `clang++`, `llvm-profdata`, and `llvm-cov`. `--tests` and `--coverage` regenerate one badge; `--help` lists every option. The script exits non-zero when a build or a test fails.

The build produces `build/bin/espresso-server` and `build/bin/espresso-client`. Test executables are in `build/tests/`.

| Option | Default | Effect |
|---|---|---|
| `ESPRESSO_BUILD_SERVER` | `ON` | Builds the server app and downloads JUCE |
| `ESPRESSO_BUILD_CLIENT` | `ON` | Builds the client app |
| `ESPRESSO_BUILD_TESTS` | `ON` | Builds the Catch2 suite |
| `ESPRESSO_WARNINGS_AS_ERRORS` | `OFF` | Adds `-Werror` |
| `ESPRESSO_COVERAGE` | `OFF` | Instruments the project's targets for LLVM source-based coverage (Clang only) and adds the `coverage-badge` target |

### macOS

Linux is the release platform; macOS builds and runs the same three programs from the build directory, without an application bundle or code signing.

Requirements:

- Xcode Command Line Tools (`xcode-select --install`), which provide Apple clang 15 or later and the macOS SDK
- CMake and Ninja, for example from Homebrew: `brew install cmake ninja`
- Optional: Homebrew LLVM (`brew install llvm`) for clang-format, clang-tidy, and RealtimeSanitizer, which Apple's toolchain does not include

The build commands are the same as on Linux:

```bash
cmake -B build -S . -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

On macOS the server uses Core Audio through JUCE; JACK and ALSA are not used. `build/bin/espresso-server --list-devices` lists the Core Audio devices that `--device` accepts. Audio input needs microphone permission for the terminal application (System Settings › Privacy & Security › Microphone); without it, the server continues with no inputs.

The LLVM tools need the SDK path. clang-tidy takes it as an extra argument, and the RealtimeSanitizer tree takes it in its compiler flags, with Homebrew's C and C++ compilers:

```bash
run-clang-tidy -p build -quiet -extra-arg=-isysroot$(xcrun --show-sdk-path) 'src/|include/|tests/'
```

```bash
cmake -B build-rtsan -S . -G Ninja -DFETCHCONTENT_BASE_DIR="$PWD/build-rtsan/_deps" -DCMAKE_C_COMPILER="$(brew --prefix llvm)/bin/clang" -DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++" -DCMAKE_CXX_FLAGS="-fsanitize=realtime -Wfunction-effects -isysroot $(xcrun --show-sdk-path)" -DESPRESSO_BUILD_SERVER=OFF -DESPRESSO_BUILD_CLIENT=OFF
```

AddressSanitizer runs without leak detection on macOS, which LeakSanitizer does not support: set `ASAN_OPTIONS=detect_leaks=0` when running the test executables. Valgrind is not available on current macOS; leak checks run on Linux.

## Editor

```bash
build/bin/espresso-client [--settings FILE] [--port N] [PROJECT_DIR [FILE...]]
```

The editor opens `PROJECT_DIR` (default: the current directory) and its `main.lisp`, if present. The file list shows the project's files, except hidden files and `build` directories. A symbolic link is listed under its own name, and a linked directory is listed like a directory; a link to a directory that contains it is not followed. Saving a linked file replaces the file the link names and keeps the link. Every command is reachable with the mouse: run controls in the header bar, file actions and dialogs in the main menu (≡, or F10), and editing actions in the context menu (right-click, or Shift+F10). F1 lists the keyboard shortcuts of the selected scheme. Key bindings, fade, quantization, and pane visibility are set in Preferences and apply immediately. Key bindings follow the scheme selected in `$XDG_CONFIG_HOME/espresso/settings.lisp` (default `~/.config/espresso/settings.lisp`):

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

Every theme assigns colors to the same roles: window, secondary text, rules, raised surfaces, fields, accent, destructive actions, selection, control states, status, and the ten syntax categories. Truecolor themes keep at least 4.5:1 contrast for body text and 3:1 for secondary text, syntax, and status colors (7:1 in High Contrast), and fall back to 256 or 16 colors on terminals without truecolor support. The theme can be changed in Preferences; the change applies immediately.

Controls show their state. A control under the pointer is drawn in its hover color, and a control held down by the pointer in its pressed color. A divider under the pointer or being dragged is drawn in the accent color. In a menu, the item under the pointer takes the focus. A control that has nothing to act on, such as Stop before a plan plays, or Undo with no change to undo, is drawn in the secondary color and ignores presses. Its tooltip still names it.

The × button in the header bar quits; in the `emacs` scheme, so does `C-x C-c`. In every scheme, F5 plays, F6 stops, and F8 evaluates the form before the cursor.

Play compiles the project's `main.lisp` with unsaved edits (without one, the file being edited), submits the result to the server, and reports compile errors in the console; clicking an error moves the cursor to it. A console message longer than the pane's width wraps onto further rows, indented under its first, and the wheel scrolls the console by rows. Evaluate Form compiles the project the same way. A failed compilation leaves the running piece unchanged.

The status bar shows the server connection, the tempo of the server's beat clock to one decimal place, the beat position, the active plan, and the missed-deadline, xrun, and drop counters. The beat position is the bar and the beat within it, both counted from 1: `Beat 9.3` is the third beat of bar 9, with bars of the active plan's meter. The server reports the tempo and the beat position with its counters, ten times per second. The beat position reads `—` while no plan is active, and the tempo and the beat position read `—` while the server is not connected.

## Compiler

```bash
build/bin/espresso-client --compile FILE.lisp [--root DIR] [--entry NAME] [--seed N]
```

Compiles the piece named `NAME` (default `main`) in `FILE.lisp` and writes its IR to standard output; errors go to standard error as `file:line:column: message`, and the exit status is 1. `--seed N` takes a whole number from 0 to 2⁶⁴ − 1; any other value, or an unknown option, exits with status 2. Warnings about likely audio hazards (loops that grow, very large gains, aliasing frequencies) go to standard error and do not change the exit status. `docs/language.md` describes the supported language, and `docs/library.md` the standard library of instruments and effects. Imports resolve against `--root`, by default FILE's directory. The IR can be piped to the server:

```bash
build/bin/espresso-client --compile examples/misc/delay.lisp | build/bin/espresso-server --play -
```

## Examples

`examples/` holds pieces and the IR each compiles to; the test suite checks that they match. `examples/README.md` describes them.

| Example | Input | Description |
|---|---|---|
| `monosynth.lisp` | Any MIDI keyboard | Two gliding oscillators, ladder filter on controllers 74 and 71, filter and amplitude envelopes |
| `reverb.lisp` | Audio input 1 | Stereo Schroeder reverb with Freeverb tunings |
| `delay.lisp` | Audio inputs 1 and 2 | 375 ms feedback echo with damping and tape wow |
| `orbit.lisp` | None | A sine and a circle that orbits with it, both driven by one LFO |
| `landscape.lisp` | None | A still picture of sky, sun, mountains, and ground, layered by call order |
| `spin.lisp` | None | A square turned by `transform` and a tone whose pitch rises with each turn |
| `meter.lisp` | None | A tone that swells and a bar whose height follows the same audio pipe |
| `ensemble.lisp` | None | A sequenced trio of standard-library instruments and effects: bass, plucked arpeggio, and FM bells |
| `keys.lisp` | Any MIDI keyboard | Eight subtractive voices from the standard library through a chorus and a reverb |
| `chimes.lisp` | None | Aleatory FM chimes: notes passed by chance, on random pentatonic pitches, through an echo and a reverb |
| `pedalboard.lisp` | Audio input 1 | Overdrive, chorus, echo, and reverb from the standard library, in pedalboard order |
| `organ.lisp` | None | A drawbar organ written as a subclass of the library's instrument, playing a chord progression |

## Server

`espresso-server` runs the audio engine. It listens for the client on `127.0.0.1:57190`, validates submitted IR against `schema/ir.schema.json` and the primitive tables, builds plans, and swaps them in with state adoption, crossfades, and beat or bar quantization.

```bash
build/bin/espresso-server [--port N] [--device NAME] [--sample-rate R] [--block-size B]
                      [--inputs N] [--outputs N] [--no-audio] [--verbose] [--play IR_FILE]
                      [--root DIR] [--buffer-memory MB]
build/bin/espresso-server --list-devices
```

| Option | Default | Effect |
|---|---|---|
| `--device NAME` | System default | Audio device, as listed by `--list-devices` |
| `--sample-rate R` | `48000` | Requested sample rate; the device may choose another |
| `--block-size B` | `256` | Requested buffer size, in samples |
| `--inputs N`, `--outputs N` | `2`, `2` | Device channels. Without input permission, the server continues with no inputs. `--outputs 0` runs the engine on the device's clock and writes silence |
| `--no-audio` | Off | Runs the engine on a timer instead of a device. The timer's thread asks for real-time scheduling, as a device's audio thread has; on Linux this needs an rtprio limit (the `audio` group's, on most distributions), and the server reports when it is refused |
| `--verbose` | Off | Logs every message except `/counters`, and the socket receive buffer size |
| `--play IR_FILE` | None | Plays an IR file at startup as plan −1, until a client swaps in another plan. `-` reads standard input |
| `--width W`, `--height H` | `1280`, `720` | Frame size of the visual layer, in pixels |
| `--no-video` | Off | Opens no window; the visual layer is not shown |
| `--root DIR` | `.` | Project root that `image-player` files and buffer files resolve against |
| `--buffer-memory MB` | `1024` | Bound on the memory of the buffers pieces declare, in MiB; a plan that needs more is refused |

Buffers load WAV and AIFF (8-, 16-, 24-, and 32-bit integer, 32- and 64-bit float), FLAC, MP3, and Ogg Vorbis files, resampled to the server's sample rate. The server keeps the 16 files it loaded last, so resubmitting a piece decodes only files that changed.

Unless `--no-video` is given, the server opens a window that shows the visual layer: the frame on the input of the piece's first `video-out`. It renders one frame per display refresh. While no plan draws to a `video-out`, the window shows a splash screen with the program name, the version, and the address the server listens on; the plan's frames replace it as soon as a plan draws.

Numeric options take decimal numbers in the stated ranges; `--port`, `--block-size`, `--inputs`, `--outputs`, and `--buffer-memory` take whole numbers. Any other value exits with status 2. The first SIGINT or SIGTERM stops the server in order; a second one during shutdown exits at once with status 130.

Every `audioOut` limits its signal to its `:ceiling` (default full scale) and mutes itself if it receives infinite or NaN samples, reporting the node in the editor console. MIDI inputs are opened as they appear; `midiIn` and `midiOut` select devices by a case-insensitive substring of the device name. `oscIn` and `oscOut` use UDP ports on `127.0.0.1`.

### Without devices

```bash
build/bin/espresso-server --no-audio --no-video [--port N] [--verbose]
```

Runs the engine on a timer and opens no window. The server answers the client-server protocol as it does with devices, so this mode tests the client and the protocol on a machine without audio or display. `--verbose` logs every message except `/counters`.

The client connects to the server on startup. While the server does not answer, the client sends `/hello` again in the background one second after each failed attempt. The console reports the first failure and the connection, and nothing for the attempts between them. A server that goes silent for 1.5 seconds is reported lost, and the client tries it again the same way.

### Offline render

```bash
build/bin/espresso-server --render piece.ir.json --out piece.wav [--seconds S] [--sample-rate R]
                      [--block-size B] [--outputs N] [--frames DIR] [--fps F]
                      [--width W] [--height H] [--root DIR] [--buffer-memory MB]
```

Plays an IR file (`-` for standard input) without a device and writes a 32-bit float WAV file (default 10 seconds, 48 kHz, 2 channels). With `--frames`, it also writes the visual layer as `DIR/frame-00000.png` onward, `F` frames a second (default 60). The same IR and seed produce identical files and frames. `docs/ir.md` describes the IR format.

## Documentation

| Document | Contents |
|---|---|
| `docs/language.md` | The Lisp dialect |
| `docs/library.md` | The standard library: voices, instruments, and effects |
| `docs/ir.md` | The IR format |
| `docs/api/index.html` | The C++ API reference, generated by Doxygen from `include/espresso/` |

The API reference is regenerated after a change to a public header:

```bash
cmake --build build --target docs
```

Doxygen writes the HTML into `docs/api/`, without timestamps, so unchanged headers leave its files unchanged. `docs/api.md` is its main page.

## License

AGPLv3. See `LICENSE`.
