# A first piece

This chapter builds a sine tone, plays it, changes it while it plays, and gives it two knobs on a page. It assumes a build from [Installing](installing.md) and an audio output.

## A project

A project is a folder. Its entry file, `main.lisp`, defines the piece the editor plays. In the GUI editor, File → New Project… → Sine tone makes one in a new folder, under the folder used last or `~/Projects`, and opens it. To make it by hand, create a folder `drift` holding `main.lisp`:

```lisp
;; A sine tone of 440 Hz at -18 dB on both channels.

(module main)

(defpiece main ()
  (let ((osc (pipe :audio))
        (out (pipe :audio)))
    (sine-osc :freq 440hz :out osc)
    (gain :in osc :amount -18db :out out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
```

- `(module main)` names the file's module.
- `(defpiece main () ...)` defines the piece named `main`, which the compiler builds.
- `(pipe :audio)` makes an audio pipe: a connection between primitives that carries an audio signal. `let` binds two of them, `osc` and `out`.
- `sine-osc` is a primitive that writes a sine wave of `:freq` into the pipe given as `:out`. `440hz` is a frequency; the compiler converts unit literals to canonical units and checks each against the parameter it is given to.
- `gain` scales the signal on `:in` by `:amount`, a level in decibels, into `out`.
- Each `audio-out` plays `out` on one output channel of the audio device: 0 is the left, 1 the right.

The piece builds a graph of four primitives connected by two pipes. The [language reference](../language.md) describes every form and primitive.

## Playing

In the GUI editor, Play, in the top bar and the Play menu, compiles the project and plays it: Ctrl+Shift+Space in every key scheme. Stop (Ctrl+. in the CUA scheme) stops it. The editor starts a server of its own.

The terminal editor connects to a server started first, in a terminal of its own:

```bash
build/bin/espresso-server
```

```bash
build/bin/espresso-client drift
```

F5 plays and F6 stops, in every key scheme.

Without an editor, the client compiles the piece to its intermediate representation (IR) and the server plays it, until Ctrl+C:

```bash
build/bin/espresso-client --compile drift/main.lisp | build/bin/espresso-server --root drift --play -
```

A compile error names the file, line, and column of the form at fault, in the editor's console or on standard error. Misspelling `:freq` as `:frequency` gives:

```text
main.lisp:8:15: sine-osc has no parameter or port :frequency
  called from main.lisp:5:1
```

## Updating

Change `440hz` to `330hz` while the piece plays, and play it again: Play in the GUI editor, which reads Update while a piece plays, or F5 in the terminal editor. The client compiles the edited project, including unsaved edits, and the server swaps the new graph in without stopping the sound. A node that is unchanged in the new graph keeps its state and plays on; a changed node crossfades from its old version to its new one over the crossfade, 50 ms by default (Preferences → Crossfade in the GUI editor, `:fade` in the terminal editor's settings). A compile that fails leaves the playing piece unchanged.

Every node has an ID derived from its place in the source, so a node keeps its ID, and its state, across edits elsewhere in the file. An update can also wait for the next beat or bar of the server's beat clock (Preferences → Quantize, `:quant`).

## A control

A control sets a value while the piece plays. Controls live on the project's page, which the server serves while the piece plays. A project has a page when its `project.lisp` names a port for it. Add `project.lisp` to `drift`:

```lisp
(project :name "drift" :page-port 8000)
```

and replace `main.lisp` with:

```lisp
;; A sine tone whose pitch and level are knobs on the project's page.

(module main)

(defpiece main ()
  (let ((pitch (knob "Pitch" :min 110hz :max 880hz :initial 440hz))
        (level (knob "Level" :min -40db :max 0db :initial -18db))
        (osc   (pipe :audio))
        (out   (pipe :audio)))
    (sine-osc :freq pitch :out osc)
    (gain :in osc :amount level :out out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
```

Each `knob` adds a knob to the page and returns a control pipe that holds its value, which a parameter takes in place of a constant. The knob's range and starting value are written once, in any unit of one kind: a frequency knob turns on an exponential scale and shows hertz, a level knob turns on a decibel scale and shows decibels. Until the page sends a value, the pipe holds `:initial`, so the piece plays as written without the page.

Play the piece. The page is at `http://localhost:8000/` in a browser, and the GUI editor shows it in a tab, "Page · drift". The compiler lays the page out: the two knobs in one section of one panel. Each knob's address comes from its label, `/pitch` and `/level`, and stays the same across compiles, so an update keeps the values the knobs were turned to. [Pages](../language.md#pages) in the language reference describes the other controls, the displays, and the groups that lay them out.

## Rendering offline

The server also renders a piece to a WAV file, without a device:

```bash
build/bin/espresso-client --compile drift/main.lisp | build/bin/espresso-server --render - --out drift.wav --seconds 5
```

The same IR and seed render the same file every time. [Playing](playing.md#offline-render) lists the render's options.

## Next

- The [standard library](../library.md) holds instruments, effects, and modulators written in the language; the GUI editor's MIDI instrument template plays one from a MIDI keyboard.
- The examples, in `examples/`, are complete pieces, from drones to a band of clip loops; `examples/README.md` describes each and how to run it, and the GUI editor's File → Open Example… copies one to a folder of its own.
- [Playing](playing.md) describes the compiler's and the server's options.
