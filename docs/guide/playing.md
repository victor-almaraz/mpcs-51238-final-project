# Playing

## Compiling

```bash
build/bin/espresso-client --compile FILE.lisp [--root DIR] [--entry NAME] [--seed N]
```

Compiles a piece in `FILE.lisp` and writes its IR to standard output. The piece and the seed are those the root directory's `project.lisp` names, by default `main` and 0; `--entry NAME` and `--seed N` override them. Errors go to standard error as `file:line:column: message`, and the exit status is 1. `--seed N` takes a whole number from 0 to 2⁶⁴ − 1; any other value, or an unknown option, exits with status 2. Warnings about likely audio hazards (loops that grow, very large gains, aliasing frequencies) go to standard error and do not change the exit status. The [language reference](../language.md) describes the supported language, and [Standard library](../library.md) the standard library of instruments and effects. Imports resolve against `--root`, by default FILE's directory. The IR can be piped to the server:

```bash
build/bin/espresso-client --compile examples/wavetable/wavetable.lisp | build/bin/espresso-server --root examples/wavetable --play -
```

## The server

`espresso-server` runs the audio engine. Run on its own, without the GUI editor, it is in headless mode: it plays the pieces a client sends, a piece given with `--play`, or renders one offline. In the bundled application, `espresso --headless` runs it with the same options, and its `--play` and `--render` also take a `.lisp` file or a project directory ([The GUI editor](gui-editor.md)). It listens for the client on `127.0.0.1:57190`, validates submitted IR against `schema/ir.schema.json` and the primitive tables, builds plans, and swaps them in with state adoption, crossfades, and beat or bar quantization.

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
| `--root DIR` | `.` | Project root that buffer files resolve against and that buffer saves are written under |
| `--buffer-memory MB` | `1024` | Bound on the memory of the buffers pieces declare, in MiB; a plan that needs more is refused |
| `--threads N` | The physical cores the process may run on, at most 16 | Threads that run each block, the audio thread included, from 1 to 64. With more than one, workers run the plan's independent nodes beside the audio thread. The output is the same at every count. The server warns when N exceeds the processors the process may use |
| `--worker-spin US` | `50` | How long a worker with no node to run spins before it sleeps, in microseconds, from 0 to 1000000. A longer spin wakes the worker sooner for the next block and uses more processor time |
| `--page-lan` | Off | Serves a project's pages on every network interface, not only `127.0.0.1`, to browsers that open the pairing link the server prints. Plain HTTP: use it only on a trusted network |

On Linux the server opens its audio device through JACK: PipeWire's JACK interface (`pipewire-jack`) or a JACK server, which runs the device callback on a real-time thread. When no JACK server lists a device, the server reports it and opens the device through ALSA, whose callback thread runs without real-time priority. `--list-devices` lists the devices of each type, and the line the server prints at startup names the type and the device it opened.

Buffers load WAV and AIFF (8-, 16-, 24-, and 32-bit integer, 32- and 64-bit float), FLAC, MP3, and Ogg Vorbis files, resampled to the server's sample rate. The server keeps the 16 files it loaded last, so resubmitting a piece decodes only files that changed.

Numeric options take decimal numbers in these ranges: `--port` 1 to 65535, `--sample-rate` 8000 to 384000, `--block-size` 16 to 8192, `--inputs` and `--outputs` 0 to 64, `--seconds` 0 to 86400, `--buffer-memory` 1 to 1048576, `--threads` 1 to 64, and `--worker-spin` 0 to 1000000. All but `--sample-rate` and `--seconds` take whole numbers. Any other value, or an unknown option, exits with status 2. The first SIGINT or SIGTERM stops the server in order; a second one during shutdown exits at once with status 130.

Every `audioOut` limits its signal to its `:ceiling` (default full scale) and mutes itself if it receives infinite or NaN samples, reporting the node in the editor console. MIDI inputs are opened as they appear; `midiIn` and `midiOut` select devices by a case-insensitive substring of the device name. `oscIn` and `oscOut` use UDP ports on `127.0.0.1`.

`serial-in` and `serial-out` open the serial devices pieces name, rescanning every second, and open a device again when it returns. On Linux, opening `/dev/ttyACM*` or `/dev/ttyUSB*` needs membership in the `dialout` group (`uucp` on some distributions); log in again after joining it. `web-in`, `web-stream`, `web-socket`, and `web-out` reach HTTP, HTTPS, WS, and WSS endpoints through libcurl, with TLS from the system's OpenSSL. Failures appear in the editor console, at most once a second for each node.

A project with `:page-port P` in `project.lisp` has pages: while it plays, the server serves at `http://localhost:P/` the page drawn from the controls and layout the piece declares, or the project's own `ui/index.html` in its place, with the rest of `ui/`, and carries OSC between its pages and the piece, over `POST /osc`, server-sent events on `GET /osc`, and a WebSocket on `/osc`. Pages load `/espresso.js` for `espresso.send`, `espresso.on`, `espresso.onOpen`, and `espresso.onClose`, and `/espresso-ui.js` for page controls: knobs, sliders, toggles, buttons, lists, a keyboard, an xy pad, meters, lamps, and readouts, bound to the piece's addresses. The listener answers only its own host names and takes messages only from its own pages. With `--page-lan` it also answers other machines on the network that opened the pairing link; its traffic, the pairing token included, is not encrypted, so use it only on a trusted network. [Pages](../language.md#pages) in the language reference describes pages and their controls.

## Without devices

```bash
build/bin/espresso-server --no-audio [--port N] [--verbose]
```

Runs the engine on a timer. The server answers the client-server protocol as it does with devices, so this mode tests the client and the protocol on a machine without audio. `--verbose` logs every message except `/counters`.

The client connects to the server on startup. While the server does not answer, the client sends `/hello` again in the background one second after each failed attempt. The console reports the first failure and the connection, and nothing for the attempts between them. A server that goes silent for 1.5 seconds is reported lost, and the client tries it again the same way.

## Offline render

```bash
build/bin/espresso-server --render piece.ir.json --out piece.wav [--seconds S] [--sample-rate R]
                      [--block-size B] [--outputs N] [--root DIR] [--buffer-memory MB] [--profile]
                      [--threads N]
```

Plays an IR file (`-` for standard input) without a device and writes a 32-bit float WAV file (default 10 seconds, 48 kHz, 2 channels). With `--profile`, it prints to standard error the time the nodes took in their blocks: the total as a share of the audio's length, the time by primitive type, and the ten slowest nodes by ID; on several threads, the total is the sum of every thread's. The same IR and seed produce identical files, at every `--threads` count. [IR format](../ir.md) describes the IR.
