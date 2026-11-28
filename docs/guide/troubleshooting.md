# Troubleshooting

Each section names a symptom, the messages that accompany it, and its remedies. The editors show the client's and the server's messages in their consoles; the server also writes its own to standard error.

## A piece does not compile

The console lists each error with the file, line, and column of the form at fault, and the calls that led to it. The playing piece, if any, plays on unchanged.

```text
main.lisp:8:15: sine-osc has no parameter or port :frequency
  called from main.lisp:5:1
```

```text
main.lisp:2:19: parameter :freq expects a frequency, got a time
```

A click on the message, in either editor, opens the file at the place. [Errors](../language.md#errors) in the language reference describes the checks. A piece that compiles can carry warnings, of a feedback loop that grows, a very large gain, a frequency that aliases, or a buffer with two writers; they do not stop it ([Warnings](../language.md#warnings)).

## The editor does not connect to the server

The GUI editor's status bar reads "Not connected" or "espresso-server · No reply", and a banner offers Retry. The terminal editor's console reads:

```text
server: not reachable at 127.0.0.1:57190; the editor connects when espresso-server starts
```

- **No server runs.** The GUI editor starts its own; the terminal editor needs one started first (`build/bin/espresso-server`). The terminal editor connects as soon as the server answers.
- **Another port.** The client and the server use 57190 unless `--port` says otherwise; both take the same option.
- **Another version.** A server built from other sources refuses the client: `metadata version mismatch: client X, server Y`. Build both from one checkout. `--version` prints each program's version and metadata version.
- **The port is taken.** A second server cannot listen beside a first: `espresso-server: cannot listen on 127.0.0.1:57190: …`. Stop the first server, or give the second another `--port`.

## No sound

- **No device opens.** `espresso-server: cannot open an audio device: …` and the server exits. `espresso-server --list-devices` lists the devices; `--device NAME` chooses one, and `--no-audio` runs without one.
- **An output muted itself.** An `audio-out` that receives infinite or NaN samples mutes itself until the next swap, and the console reports the node: `non-finite audio samples; this output is muted until the next swap`. Correct the piece, a division by a signal that reaches 0 or a feedback loop that grows for instance, and play it again.
- **No channel is written.** A piece sounds only through its `audio-out` nodes, each on the device outputs from its `:channel`, counted from 0. The status bar's plan shows whether a plan plays.
- **No input on macOS.** Without microphone permission for the terminal application, the server runs with no inputs (System Settings › Privacy & Security › Microphone).

## Clicks, dropouts, and missed deadlines

The status bar counts missed deadlines (blocks the engine did not finish in time) and xruns (gaps the device reported), and shows the busiest thread's load.

- **A Debug build.** The default build type is Debug, whose engine is compiled without optimization. Play heavy pieces from a Release build ([Installing](installing.md#building)).
- **No real-time scheduling.** `espresso-server: workers run without real-time priority (…)`, or `… the no-audio timer runs without real-time priority (…)`. Give the user an rtprio limit ([Performance](performance.md#linux-settings)), or run with `--threads 1`.
- **ALSA in place of JACK.** `no JACK server (PipeWire's JACK) lists a device; using ALSA, whose audio thread runs without real-time priority`. Install PipeWire's JACK interface (`pipewire-jack`), or start a JACK server.
- **More threads than processors.** `espresso-server: --threads N exceeds the M processors this process may use`. Leave `--threads` at its default, or lower it.
- **A processor that sleeps.** The frequency governor and idle states delay a block's start ([Performance](performance.md#linux-settings)).

## A plan is refused

The server refuses a plan whose buffers would exceed its memory bound, and the playing piece plays on:

```text
the plan's buffers, frames, and spectral history need 1200 MB with those already playing; the server allows 1024 MB (--buffer-memory)
```

Raise the bound with `--buffer-memory MB`, or shorten the piece's buffers.

## The page does not open

- **No page port.** A project has a page only when its `project.lisp` names one: `(project :page-port 8000)`. The page is served while a piece of the project plays.
- **The port is taken.** `cannot listen for pages on 127.0.0.1:8000: …`. Another program uses the port: choose another `:page-port`.
- **Another machine.** The page answers only `localhost` unless the server runs with `--page-lan`, and then only browsers that open the pairing link it prints ([Inputs and outputs](io.md#pages)).

## A device of a piece does not open

- **Serial.** `cannot open serial device /dev/ttyACM0: …; add your user to the dialout group (uucp on some distributions) and log in again`. The server tries the device again every second, so a device plugged in later opens.
- **MIDI.** `midi-in` and `midi-out` match a device by a case-insensitive part of its name; an empty `:device` matches every device.
- **Web data.** A failing request, stream, or socket is reported in the console at most once a second for each node, with the reason from libcurl. HTTPS and WSS need the system's OpenSSL certificates.
