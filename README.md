# espresso

![tests](docs/badges/tests.svg)
![coverage](docs/badges/coverage.svg)

espresso is a Linux-first workstation for acousmatic, aleatory, and automatic music, and the Lisp dialect its pieces are written in. Pieces compile to an intermediate representation (IR), which a native C++ engine plays.

## Requirements

- CMake 3.25 or later
- A C++20 compiler (GCC 12+, Clang 15+)
- Git, for dependency downloads
- Linux: JACK headers (`pipewire-jack` or `libjack-jackd2-dev`), ALSA headers, and the JUCE Linux dependencies
- Optional: Ninja, Doxygen, Valgrind

All libraries (JUCE, FTXUI, nlohmann/json, json-schema-validator, Catch2, stb, dr_libs, PFFFT, libcurl) are downloaded by CMake into `third_party/` at configure time. libcurl takes TLS from the system's OpenSSL: `libssl-dev` on Debian and Ubuntu, `openssl@3` from Homebrew on macOS.

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
build/bin/espresso-client --compile examples/wavetable/wavetable.lisp | build/bin/espresso-server --root examples/wavetable --play -
```

## Sidecar

```bash
build/bin/espresso-client --serve [--port N]
```

Runs the client without the terminal editor, for a graphical editor that starts it as a child process. The client reads JSON-RPC 2.0 requests from standard input and writes responses and notifications to standard output, one message per line, and reaches the server on `127.0.0.1:N` as the terminal editor does. The methods are `hello`, `project.open`, `file.read`, `file.write`, `file.create` (an empty file, and the folders above it), `file.rename`, `run.play`, `run.eval`, `run.stop`, `server.connect`, `highlight`, `language`, and `shutdown`; the notifications are `console` and `status`. Paths are relative to the project root that `project.open` names. After `shutdown`, or at the end of standard input, the client finishes the run requests already read and exits with status 0.

## Examples

Each example is a folder in `examples/`. A piece is `NAME/NAME.lisp`, with sample files in `samples/` where it has them. Its page is drawn from the controls and layout the piece declares in the language, with `ui/script.js` and `ui/style.css` for what the language does not draw; `mini-clone` and `rack` have custom pages of their own, `ui/index.html`. `hectodrone`, `organ-pad`, and `phasing` are projects of several files that start at `main.lisp`. The test suite checks that every example compiles to the same valid IR every time. `examples/README.md` describes each example and how to run it.

| Example | Input | Description |
|---|---|---|
| `edits` | Wikimedia's stream of recent changes | A plucked note for each edit to a Wikimedia wiki, its velocity set by the page's length |
| `firehose` | Bluesky's Jetstream | A short FM note for each public Bluesky post, on a pitch and length drawn at random |
| `freeze-pedal` | Audio input 1, sustain pedal | A granular delay that the sustain pedal freezes on what it heard just before the pedal went down |
| `grain-cloud` | None | A cloud of grains over struck glass, jittered in time and pitch and spread across the stereo field |
| `hectodrone` | Any MIDI keyboard | Four voices of one hundred sawtooths, their detuning on the pitch-bend wheel and their filter on the modulation wheel |
| `mini-clone` | Its page, a MIDI keyboard, audio input 1 | A monophonic synthesizer laid out after the Minimoog Model D, set from a panel in a web page |
| `organ-pad` | A MIDI keyboard with two modulation wheels | A drawbar organ through a rotary speaker, a tape loop, and a reverb on each side |
| `page-keys` | Its page | Eight subtractive voices and a chain of three pedals, the page drawn from the instrument's and the effects' controls |
| `phasing` | A MIDI keyboard with two modulation wheels | Two marimbas that play one pattern on clocks that drift apart, over a pad of the held notes |
| `rack` | Its page, a MIDI keyboard | A fixed modular synthesizer of thirteen modules, patched with cables in a web page |
| `report` | None | Three detuned sines whose loudness the piece posts to a web endpoint every four beats |
| `sampler-keys` | Any MIDI keyboard | One recording of middle C played across the keyboard by eight voices |
| `serial-knob` | A potentiometer on a USB serial board | A drone whose filter cutoff follows the potentiometer |
| `spectral-audioscape` | None | A short FM piano arpeggio looped from a buffer through an eight-band spectral delay, a spectral freeze, and a reverb |
| `straintest` | None | The piece of the v2 load test: eight instruments of eight voices, each through two effects, each voice a stack of eight detuned layers |
| `wavetable` | None | Three wavetable oscillators a fifth and an octave apart, swept from a sine table to a square and back |
| `weather` | Open-Meteo, polled once a minute | A tone pitched by Chicago's temperature, and a noise raised by its wind |

## Server

`espresso-server` runs the audio engine. Run on its own, without the GUI editor, it is in headless mode: it plays the pieces a client sends, a piece given with `--play`, or renders one offline. In the bundled application, `espresso --headless` runs it with the same options, and its `--play` and `--render` also take a `.lisp` file or a project directory (`src/apps/editor/README.md`). It listens for the client on `127.0.0.1:57190`, validates submitted IR against `schema/ir.schema.json` and the primitive tables, builds plans, and swaps them in with state adoption, crossfades, and beat or bar quantization.

```bash
build/bin/espresso-server [--port N] [--device NAME] [--sample-rate R] [--block-size B]
                      [--inputs N] [--outputs N] [--no-audio] [--verbose] [--play IR_FILE]
                      [--root DIR] [--buffer-memory MB] [--page-lan] [--threads N] [--worker-spin US]
build/bin/espresso-server --list-devices
```

| Option | Default | Effect |
|---|---|---|
| `--device NAME` | System default | Audio device, as listed by `--list-devices`. On Linux, a name that JACK does not list selects a device of the default type, ALSA |
| `--sample-rate R` | `48000` | Requested sample rate; the device may choose another |
| `--block-size B` | `256` | Requested buffer size, in samples |
| `--inputs N`, `--outputs N` | `2`, `2` | Device channels. Without input permission, the server continues with no inputs. `--outputs 0` runs the engine on the device's clock and writes silence |
| `--no-audio` | Off | Runs the engine on a timer instead of a device. The timer's thread asks for real-time scheduling, as a device's audio thread has; on Linux this needs an rtprio limit (the `audio` group's, on most distributions), and the server reports when it is refused |
| `--verbose` | Off | Logs every message except `/counters`, and the socket receive buffer size |
| `--play IR_FILE` | None | Plays an IR file at startup as plan −1, until a client swaps in another plan. `-` reads standard input |
| `--root DIR` | `.` | Project root that buffer files resolve against |
| `--buffer-memory MB` | `1024` | Bound on the memory of the buffers pieces declare, in MiB; a plan that needs more is refused |
| `--threads N` | The physical cores the process may run on, at most 16 | Threads that run each block, the audio thread included, from 1 to 64. With more than one, workers run the plan's independent nodes beside the audio thread. The output is the same at every count. The server warns when N exceeds the processors the process may use |
| `--worker-spin US` | `50` | How long a worker with no node to run spins before it sleeps, in microseconds, from 0 to 1000000. A longer spin wakes the worker sooner for the next block and uses more processor time |
| `--page-lan` | Off | Serves a project's pages on every network interface, not only `127.0.0.1`, to browsers that open the pairing link the server prints. Plain HTTP: use it only on a network you trust |

On Linux the server opens its audio device through JACK: PipeWire's JACK interface (`pipewire-jack`) or a JACK server, which runs the device callback on a real-time thread. When no JACK server lists a device, the server reports it and opens the device through ALSA, whose callback thread runs without real-time priority. `--list-devices` lists the devices of each type, and the line the server prints at startup names the type and the device it opened.

Buffers load WAV and AIFF (8-, 16-, 24-, and 32-bit integer, 32- and 64-bit float), FLAC, MP3, and Ogg Vorbis files, resampled to the server's sample rate. The server keeps the 16 files it loaded last, so resubmitting a piece decodes only files that changed.

Numeric options take decimal numbers in the stated ranges; `--port`, `--block-size`, `--inputs`, `--outputs`, `--buffer-memory`, `--threads`, and `--worker-spin` take whole numbers. Any other value exits with status 2. The first SIGINT or SIGTERM stops the server in order; a second one during shutdown exits at once with status 130.

Every `audioOut` limits its signal to its `:ceiling` (default full scale) and mutes itself if it receives infinite or NaN samples, reporting the node in the editor console. MIDI inputs are opened as they appear; `midiIn` and `midiOut` select devices by a case-insensitive substring of the device name. `oscIn` and `oscOut` use UDP ports on `127.0.0.1`.

`serial-in` and `serial-out` open the serial devices pieces name, rescanning every second, and open a device again when it returns. On Linux, opening `/dev/ttyACM*` or `/dev/ttyUSB*` needs membership in the `dialout` group (`uucp` on some distributions); log in again after joining it. `web-in`, `web-stream`, `web-socket`, and `web-out` reach HTTP, HTTPS, WS, and WSS endpoints through libcurl, with TLS from the system's OpenSSL. Failures appear in the editor console, at most once a second for each node.

A project with `:page-port P` in `project.lisp` has pages: while it plays, the server serves at `http://localhost:P/` the page drawn from the controls and layout the piece declares, or the project's own `ui/index.html` in its place, with the rest of `ui/`, and carries OSC between its pages and the piece, over `POST /osc`, server-sent events on `GET /osc`, and a WebSocket on `/osc`. Pages load `/espresso.js` for `espresso.send`, `espresso.on`, `espresso.onOpen`, and `espresso.onClose`, and `/espresso-ui.js` for page controls: knobs, sliders, toggles, buttons, lists, a keyboard, an xy pad, meters, lamps, and readouts, bound to the piece's addresses. The listener answers only its own host names and takes messages only from its own pages. With `--page-lan` it also answers other machines on the network that opened the pairing link; its traffic, the pairing token included, is not encrypted, so use it only on a trusted network. `docs/language.md` describes pages and their controls.

### Threads

The server runs each block on `--threads` threads: the audio thread and workers. When a plan is built, the server groups its nodes into tasks and orders the tasks by what they share: two nodes keep their array order when they use one pipe, buffer, I/O port, or the device's outputs and at least one writes it. A thread runs a task once every task it waits for has finished, and the audio thread runs tasks too, so a late worker delays a block by at most the task it is running. A plan whose tasks are estimated to save less than 4 µs a block on `--threads` threads, counting the cost of passing data between processors, runs every node on the audio thread in array order, as with `--threads 1`. When fewer threads are estimated to save within 4 µs a block of what all of them save, the tasks run on the fewest such threads, and the other workers sleep. Writers of one audio pipe write private pipes that the server adds in array order, so the output is bit-identical at every thread count, offline and live. With `--threads 1` the server runs every node on the audio thread in array order.

In a live server, workers ask for real-time scheduling, as the audio thread has; on Linux this needs an rtprio limit (the `audio` group's, on most distributions), and the server reports when it is refused. A worker without real-time priority shares its processor with ordinary threads, so a busy system can delay it past a block's deadline; `--threads 1` runs every node on the audio thread instead. A worker with no node to run spins for `--worker-spin` microseconds, then sleeps until the next block.

On Linux, three settings of the system decide whether blocks meet their deadlines:

- **Real-time scheduling.** Workers run at real-time priority only within the rtprio limit (`ulimit -r`), and the device callback only through JACK.
- **The `performance` frequency governor.** It keeps each processor at its highest frequency. Other governors lower the frequency of a processor that idles between blocks and raise it only after load returns, which lengthens the blocks that follow. `cpupower frequency-set -g performance` selects it until the next boot.
- **A CPU latency request.** A worker that sleeps between blocks leaves its processor idle, and the deeper the idle state, the later the worker starts the next block. While a process holds `/dev/cpu_dma_latency` open with a latency in microseconds written to it, the kernel keeps every processor out of idle states that take longer to leave. The file is writable only by root by default: `sudo sh -c 'exec 3>/dev/cpu_dma_latency; printf 0 >&3; exec sleep infinity'` holds a request of 0 until the command is stopped. The server makes no request.

`scripts/straintest.sh` times offline renders of `examples/straintest`, the piece of the v2 load test, on 1, 2, 4, 8, and 16 threads, and prints each count's mean render time and speedup over one thread.

`/counters` carries the thread count, the blocks in which the audio thread waited for a worker, and each thread's load: its share of the block period spent running nodes. The editor's status bar shows the busiest thread's load and, past one thread, the thread count.

### Without devices

```bash
build/bin/espresso-server --no-audio [--port N] [--verbose]
```

Runs the engine on a timer. The server answers the client-server protocol as it does with devices, so this mode tests the client and the protocol on a machine without audio. `--verbose` logs every message except `/counters`.

The client connects to the server on startup. While the server does not answer, the client sends `/hello` again in the background one second after each failed attempt. The console reports the first failure and the connection, and nothing for the attempts between them. A server that goes silent for 1.5 seconds is reported lost, and the client tries it again the same way.

### Offline render

```bash
build/bin/espresso-server --render piece.ir.json --out piece.wav [--seconds S] [--sample-rate R]
                      [--block-size B] [--outputs N] [--root DIR] [--buffer-memory MB] [--profile]
                      [--threads N]
```

Plays an IR file (`-` for standard input) without a device and writes a 32-bit float WAV file (default 10 seconds, 48 kHz, 2 channels). With `--profile`, it prints to standard error the time the nodes took in their blocks: the total as a share of the audio's length, the time by primitive type, and the ten slowest nodes by ID; on several threads, the total is the sum of every thread's. The same IR and seed produce identical files, at every `--threads` count. `docs/ir.md` describes the IR format.

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
