# A MIDI instrument with pedals

This tutorial builds a piece that plays a standard-library synthesizer from a MIDI keyboard, draws it on a page with a keyboard of its own, sends it through a pedalboard of effects, moves its filter with a modulator, and adds an effect written in the piece. It assumes [A first piece](../guide/first-piece.md) and a MIDI keyboard connected to the machine.

## A synthesizer from a MIDI keyboard

Create a folder `pedals` holding `main.lisp`:

```lisp
;; A subtractive synthesizer played from a MIDI keyboard.

(module main)

(import "std/instruments")

(defpiece main ()
  (let ((synth (make subtractive :voices 8 :cutoff 800hz :resonance 0.4
                                 :release 400ms :level -9db))
        (notes (pipe :event))
        (out   (pipe :audio)))
    (midi-in :device "" :out notes)
    (play synth notes out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
```

- `(import "std/instruments")` loads the instruments of the [standard library](../library.md#stdinstruments).
- `(make subtractive ...)` makes an instrument: a sawtooth through a resonant low-pass ladder filter, with eight voices. Each initarg sets a slot; the slots not given keep the defaults the library lists.
- `midi-in` writes the notes of every MIDI input into the event pipe `notes`. `:device` selects inputs by a case-insensitive substring of their names; the empty string matches every device ([Inputs and outputs](../guide/io.md#midi)).
- `(play synth notes out)` builds the voices and plays the notes into the audio pipe `out`. Each voice's amplitude follows its note's velocity.

Play the piece as [Playing](../guide/first-piece.md#playing) describes, or from the command line:

```bash
build/bin/espresso-client --compile pedals/main.lisp | build/bin/espresso-server --root pedals --play -
```

A keyboard plugged in while the piece plays is opened at once. The synthesizer's settings are the constants written in `make`; an offline render of the piece is silent, as its only notes come from the keyboard.

## The page and the pedals

A page draws the synthesizer's knobs, a keyboard that plays it from the browser, and the effects it passes through. Add `project.lisp` to `pedals`:

```lisp
(project :name "pedals" :page-port 8000)
```

and replace `main.lisp` with:

```lisp
;; The synthesizer on a page, with a keyboard, through three pedals.

(module main)

(import "std/instruments")
(import "std/effects")

(defpiece main ()
  (let ((synth (make subtractive :voices 8 :cutoff 800hz :resonance 0.4
                                 :release 400ms :level -9db))
        (notes (keys "Keys" :low 48 :high 71))
        (dry   (pipe :audio))
        (out   (pipe :audio)))
    (midi-in :device "" :out notes)
    (define panel (synth-panel "pedals" synth (section "Keys" notes)))
    (play synth notes dry)
    (define board (chain dry out
                         (make chorus :mix 0.3)
                         (make echo :time 0.75beat :feedback 0.35 :mix 0.25)
                         (make reverb :mix 0.2)))
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)
    (page panel board)))
```

- `(keys "Keys" :low 48 :high 71)` adds a keyboard to the page and returns the event pipe of its notes. `midi-in` writes the same pipe: writers to an event pipe merge, so the MIDI keyboard and the page keyboard play the same voices.
- `(synth-panel "pedals" synth ...)` draws the instrument's `:control` slots as knobs, each starting at the slot's value: Cutoff, Env amount, Resonance, Decay, Level, Attack, Sustain, and Release. It sets each slot to its knob's pipe, so it comes before `play`, which builds the voices from those pipes. `:voices` and `:wave` are fixed when the voices are built and have no knob. The section that holds the keys is the panel's keybed, under the knobs.
- `(import "std/effects")` loads the [effects](../library.md#stdeffects). `(chain dry out ...)` runs the effects in series from `dry` to `out` and returns a pedalboard: each effect is a pedal of its `:control` slots, with a footswitch that bypasses it ([Pedals and chains](../library.md#pedals-and-chains)). The echo's `:time` in beats follows the tempo and has no knob.
- `(page panel board)` places the synth panel and the pedalboard on the page.

The keyboard, the knobs, and the pedals are page controls, so the piece needs `:page-port` in `project.lisp`; without it, the compile fails at the first control. Play the piece. The page is at `http://localhost:8000/`, and the GUI editor shows it in a tab, "Page · pedals". Until the page sends a value, each knob's pipe holds the value written in `make`, so the piece plays as written without the page. [Controls in the language](../language.md#controls-in-the-language) describes the controls and groups a page holds.

## Movement

A modulator moves a knob while the piece plays. Replace `main.lisp` with:

```lisp
;; An LFO sweeps the synthesizer's cutoff and the chorus's mix.

(module main)

(import "std/instruments")
(import "std/effects")
(import "std/modulators")

(defpiece main ()
  (let ((synth  (make subtractive :voices 8 :cutoff 800hz :resonance 0.4
                                  :release 400ms :level -9db))
        (wobble (make lfo-mod :rate 0.2hz :shape ':triangle))
        (notes  (keys "Keys" :low 48 :high 71))
        (dry    (pipe :audio))
        (out    (pipe :audio)))
    (midi-in :device "" :out notes)
    (modulate synth 'cutoff wobble 0.3)
    (define panel (synth-panel "pedals" synth
                               (section "LFO" wobble)
                               (section "Keys" notes)))
    (play synth notes dry)
    (define board (chain dry out
                         (modulate (make chorus :mix 0.3) 'mix wobble 0.2)
                         (make echo :time 0.75beat :feedback 0.35 :mix 0.25)
                         (make reverb :mix 0.2)))
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)
    (page panel board)))
```

- `(make lfo-mod :rate 0.2hz :shape ':triangle)` makes a [modulator](../library.md#stdmodulators): a triangle wave from 0 to 1, one cycle every five seconds. The shape is a keyword value, so it is quoted.
- `(modulate synth 'cutoff wobble 0.3)` records that the LFO moves the Cutoff knob by 0.3 of its travel. An LFO is bipolar, so the cutoff sweeps half the amount either side of the knob's value: on a knob from 50 Hz to 8 kHz, on an exponential scale, from about 370 Hz to 1.7 kHz around 800 Hz. `play` applies the modulation, so the slot is drawn first, by `synth-panel`, and modulated before `play`.
- `(section "LFO" wobble)` draws the modulator's Rate knob in the synth panel. Its shape is fixed when its signal is built and has no knob.
- `modulate` returns the instance, so it stands where `make` does: the chorus's mix follows the same LFO, by 0.2 of its travel.

A moved knob shows, around its track, the span its modulations reach. Turning the Cutoff knob moves the centre of the sweep; turning the LFO's Rate knob changes its speed.

## An effect of the piece's own

An effect is a class. A subclass of `effect` whose slots have `:control` options is a pedal on the board as the library's effects are. Replace `main.lisp` with:

```lisp
;; A tremolo of the piece's own joins the pedalboard.

(module main)

(import "std/instruments")
(import "std/effects")
(import "std/modulators")

;; A tremolo: the input scaled by an LFO from 0 to 1. The mix sets its depth.
(defclass tremolo (effect)
  ((rate  :initarg :rate  :initform 5hz :accessor rate
          :control (knob :min 0.5hz :max 15hz))))

(defmethod process ((tr tremolo) in out)
  (let ((swing (pipe :control)))
    (lfo :shape :sine :rate (rate tr) :depth 0.5 :offset 0.5 :out swing)
    (gain :in in :amount swing :out out)))

(defpiece main ()
  (let ((synth  (make subtractive :voices 8 :cutoff 800hz :resonance 0.4
                                  :release 400ms :level -9db))
        (wobble (make lfo-mod :rate 0.2hz :shape ':triangle))
        (notes  (keys "Keys" :low 48 :high 71))
        (dry    (pipe :audio))
        (out    (pipe :audio)))
    (midi-in :device "" :out notes)
    (modulate synth 'cutoff wobble 0.3)
    (define panel (synth-panel "pedals" synth
                               (section "LFO" wobble)
                               (section "Keys" notes)))
    (play synth notes dry)
    (define board (chain dry out
                         (make tremolo :rate 6hz :mix 0.6)
                         (modulate (make chorus :mix 0.3) 'mix wobble 0.2)
                         (make echo :time 0.75beat :feedback 0.35 :mix 0.25)
                         (make reverb :mix 0.2)))
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)
    (page panel board)))
```

- `(defclass tremolo (effect) ...)` defines the effect. Its `rate` slot's `:control` names the knob its pedal draws, labelled Rate after the slot and starting at the slot's value, 6 Hz here. It inherits `effect`'s `mix` slot and that slot's knob. The accessor `rate` is a generic function, shared with the other classes that have a slot of the name, such as `chorus` and `lfo-mod`.
- The `process` method writes the wet signal: the `lfo` primitive swings from 0 to 1 at the knob's rate, and `gain` scales the input by it. The `:around` method on `effect` crossfades the dry input and the wet signal by the mix, so the Mix knob sets the tremolo's depth: at 0.6, the level falls to 40% at the bottom of each cycle.
- `(make tremolo :rate 6hz :mix 0.6)` in `chain` puts the tremolo first on the pedalboard, as a pedal labelled Tremolo with a Rate knob, a Mix knob, and a footswitch. The footswitch scales the mix, so a bypassed tremolo passes its dry input.

[Classes and generic functions](../language.md#classes-and-generic-functions) describes `defclass`, `defmethod`, and the order in which methods run.

## Next

- The [standard library](../library.md) lists the other instruments (`fm`, `pluck`, `sampler`, `drum-kit`), effects, and modulators, with every slot and its knob.
- [Pages](../language.md#pages) describes the page's controls, its layout, tabs, themes, and how a project restyles a drawn page or replaces it with its own `ui/index.html`.
- `examples/page-keys` is a piece of this kind with an output meter; `examples/autoband` plays four instruments from clip loops, each with its own pedalboard, on a page of tabs.
- [Playing](../guide/playing.md) describes the compiler's and the server's options.
