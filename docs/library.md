# Standard library

The standard library is a set of files written in the language and compiled into the client. `(import "std/name")` loads one. Its node IDs and errors name the file as `std/name.lisp`.

| File | Contents |
|---|---|
| `std/voices` | `voices`: polyphony over `voice-split` |
| `std/instruments` | `subtractive`, `fm`, and `pluck` instruments, and the `play` and `voice` generic functions |
| `std/effects` | `echo`, `chorus`, `reverb`, and `overdrive` effects, and the `process` generic function |

## std/voices

```lisp
(import "std/voices")

(defpiece main ()
  (let ((notes (pipe :event))
        (out   (pipe :audio)))
    (midi-in :device "" :out notes)
    (voices 8 notes
            (fn (n)
              (let ((freq (pipe :control))
                    (osc  (pipe :audio)))
                (note-to-freq :in n :out freq)
                (saw-osc :freq freq :out osc)
                (adsr :gate n :in osc :gain -18db :out out))))
    (audio-out :in out :channel 0)))
```

`(voices count notes build)` splits the notes on the event pipe `notes` among `count` voices with `voice-split` and calls `(build voice-notes)` once for each voice, with an event pipe that carries that voice's notes. When every voice is sounding, a new note takes the voice of the oldest note, which receives a note-off first. Each call to `build` is keyed by its voice number, so each voice keeps its node IDs, and its state, when the piece is replaced. `voices` returns the list of `build`'s results. Voices that write the same audio pipe sum there.

## std/instruments

```lisp
(import "std/instruments")

(defpiece main ()
  (let ((clock (pipe :event))
        (steps (pipe :event))
        (notes (pipe :event))
        (out   (pipe :audio)))
    (metro :every 0.5beat :out clock)
    (sequencer :in clock :values [57 60 64 67] :events steps)
    (trigger-notes :in steps :length 0.4beat :out notes)
    (play (make subtractive :voices 4 :cutoff 300hz :resonance 0.5) notes out)
    (audio-out :in out :channel 0)))
```

An instrument is an instance of a subclass of `instrument`. `(play instrument notes out)` plays the notes on the event pipe `notes` into the audio pipe `out`: it calls `voices` with the instrument's voice count and builds each voice with `(voice instrument voice-notes out)`. The voices sum in `out`. A new instrument is a subclass with a `voice` method; it inherits `play`.

Each voice's amplitude follows its note's velocity: a note at full velocity sounds at the instrument's `level`.

| Class | Initarg | Default | Meaning |
|---|---|---|---|
| `instrument` | `:voices` | 8 | Voices; a note beyond them takes the oldest voice |
| | `:level` | -12 dB | Level of a voice at full velocity |
| `subtractive`, `fm` | `:attack`, `:decay`, `:sustain`, `:release` | 5 ms, 300 ms, 0.5, 300 ms | Amplitude envelope |
| `subtractive` | `:wave` | `:saw` | `':saw` or `':pulse` (a 40% pulse) |
| | `:cutoff` | 400 Hz | Cutoff of the low-pass ladder filter between notes |
| | `:env-amount` | 3 kHz | How far each note's filter envelope opens the cutoff |
| | `:resonance` | 0.3 | Filter resonance, 0 to 1 |
| `fm` | `:ratio` | 2 | Modulator frequency, as a multiple of the note's frequency |
| | `:index` | 3 | Largest frequency deviation, as a multiple of the note's frequency; it decays with the modulation envelope |
| `pluck` | `:feedback` | 0.99 | How long the string rings, 0 to 1 |
| | `:damping` | 0.3 | How fast its high partials fade, 0 to 1 |

`subtractive` passes a sawtooth or pulse wave through a resonant ladder low-pass filter, which a second envelope opens from `cutoff` by up to `env-amount` at each note. `fm` is two-operator frequency modulation of a sine carrier by a sine modulator. `pluck` is a Karplus-Strong string: a burst of noise at each note excites a feedback comb whose delay is the note's period.

A keyword value is quoted in a slot's initarg: `:wave ':pulse`. Unquoted, `:pulse` would read as the next keyword argument.

## std/effects

```lisp
(import "std/effects")

(defpiece main ()
  (let ((in     (pipe :audio))
        (driven (pipe :audio))
        (out    (pipe :audio)))
    (audio-in :channel 0 :out in)
    (process (make overdrive :drive 8) in driven)
    (process (make echo :time 0.75beat :mix 0.3) driven out)
    (audio-out :in out :channel 0)))
```

An effect is an instance of a subclass of `effect`. `(process effect in out)` writes the audio pipe `in`, processed, to the audio pipe `out`. Each effect's primary method writes its wet signal; an `:around` method on `effect` passes it a pipe for the wet signal and crossfades the dry input and the wet signal into `out`, linearly, by the effect's `mix`. A new effect is a subclass with a `process` method that writes its wet signal; it inherits the mix.

| Class | Initarg | Default | Meaning |
|---|---|---|---|
| `effect` | `:mix` | 0.5 | Wet proportion, 0 (dry) to 1 (wet) |
| `echo` | `:time` | 0.5 beat | Time between repeats, at most 4 s; a time in beats follows the tempo |
| | `:feedback` | 0.4 | Level of each repeat relative to the one before, -1 to 1 |
| | `:damping` | 0.3 | How much darker each repeat is, 0 to 1 |
| `chorus` | `:rate` | 0.8 Hz | Rate of the delay's sweep |
| | `:delay` | 15 ms | Centre of the swept delay |
| | `:depth` | 4 ms | How far the delay sweeps each way |
| `reverb` | `:size` | 0.84 | Feedback of the four combs: longer decays as it nears 1 |
| | `:damping` | 0.2 | How fast high frequencies decay, 0 to 1 |
| `overdrive` | `:drive` | 4 | Gain into the `tanh` saturation |
| | `:tone` | 3 kHz | Cutoff of the low-pass filter after it |
| | `:level` | -6 dB | Level after the saturation |

`echo` is a damped feedback comb. `chorus` is a delay line whose delay an LFO sweeps. `reverb` is Schroeder's reverberator with Freeverb's tunings: four damped feedback combs in parallel, then two allpass diffusers in series. `overdrive` drives the input into `tanh` saturation and low-pass filters the result.

Accessors of the library classes are generic functions: `(cutoff s)` reads a `subtractive`'s cutoff, and `(set! (cutoff s) 600hz)` changes it before `play` builds the voices.
