# Standard library

The standard library is a set of files written in the language and compiled into the client. `(import "std/name")` loads one. Its node IDs and errors name the file as `std/name.lisp`.

| File | Contents |
|---|---|
| `std/voices` | `voices`: polyphony over `voice-split` |
| `std/instruments` | `subtractive`, `fm`, `pluck`, and `sampler` instruments, and the `play` and `voice` generic functions |
| `std/effects` | `echo`, `chorus`, `reverb`, `overdrive`, `granular-delay`, `spectral-delay`, and `freeze` effects, and the `process` generic function |

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

`(voices count notes build)` splits the notes on the event pipe `notes` among `count` voices with `voice-split` and calls `(build voice-notes)` once for each voice, with an event pipe that carries that voice's notes. When every voice is sounding, a new note takes the voice of the oldest note, which receives a note-off first. The calls to `build` are numbered in voice order, so each voice keeps its node IDs, and its state, when the piece is replaced. `voices` returns the list of `build`'s results. Voices that write the same audio pipe sum there.

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

An instrument is an instance of a subclass of `instrument`. `(play instrument notes out)` plays the notes on the event pipe `notes` into the audio pipe `out`: it calls `voices` with the instrument's voice count and builds each voice with `(voice instrument voice-notes out)`. The voices sum, and the sum is scaled by the instrument's `level` into `out`. A new instrument is a subclass with a `voice` method that writes one voice, at full scale, to the pipe it is given; it inherits `play`.

Each voice's amplitude follows its note's velocity. For writing voices, the file also defines `enveloped`, a class with the `:attack`, `:decay`, `:sustain`, and `:release` slots that `subtractive` and `fm` inherit; `(amplitude instrument notes in out)`, which applies an `enveloped` instrument's envelope to `in`, gated by `notes` and scaled by each note's velocity; and `(note-velocity notes)`, a control pipe of each note's velocity from 0 to 1.

| Class | Initarg | Default | Meaning |
|---|---|---|---|
| `instrument` | `:voices` | 8 | Voices; a note beyond them takes the oldest voice |
| | `:level` | -12 dB | Scale of the voices' sum |
| `subtractive`, `fm` | `:attack`, `:decay`, `:sustain`, `:release` | 5 ms, 300 ms, 0.5, 300 ms | Amplitude envelope |
| `subtractive` | `:wave` | `':saw` | `':pulse` for a 40% pulse wave; any other value gives a sawtooth |
| | `:cutoff` | 400 Hz | Cutoff of the low-pass ladder filter between notes |
| | `:env-amount` | 3 kHz | How far each note's filter envelope opens the cutoff |
| | `:resonance` | 0.3 | Filter resonance, 0 to 1 |
| `fm` | `:ratio` | 2 | Modulator frequency, as a multiple of the note's frequency |
| | `:index` | 3 | Largest frequency deviation, as a multiple of the note's frequency; it decays with the modulation envelope |
| `pluck` | `:feedback` | 0.99 | How long the string rings, 0 to 1 |
| | `:damping` | 0.3 | How fast its high partials fade, 0 to 1 |
| `sampler` | `:buffer` | | The buffer to play; required |
| | `:root` | 60 | The note that plays the buffer at its own speed |
| | `:start` | 0 s | Where each note starts in the buffer |
| | `:loop` | `':off` | `':forward` or `':ping-pong` to loop the whole buffer while the note sounds |
| | `:attack`, `:decay`, `:sustain`, `:release` | 5 ms, 300 ms, 1, 300 ms | Amplitude envelope |

`subtractive` passes a sawtooth or pulse wave through a resonant ladder low-pass filter, which a second envelope opens from `cutoff` by up to `env-amount` at each note. `fm` is two-operator frequency modulation of a sine carrier by a sine modulator. `pluck` is a Karplus-Strong string: a burst of noise at each note excites a feedback comb whose delay is the note's period. `sampler` plays a buffer at each note: a note a semitone above `root` plays it a semitone higher and faster, and its envelope's release ends the note. Its voices have the buffer's channels, so a stereo buffer plays in stereo. A sampler plays one sample across the keyboard:

```lisp
(let ((piano (buffer :channels 2 :file "samples/c4.wav"))
      (notes (pipe :event))
      (out   (pipe :audio :channels 2)))
  (midi-in :out notes)
  (play (make sampler :buffer piano :root 60) notes out)
  (audio-out :in out :channel 0))
```

A keyword value is quoted in a slot's initarg: `:wave ':pulse`. Unquoted, `:pulse` would read as the next keyword argument.

`(synth-panel instrument)` draws an instrument's `:control` slots as knobs, each starting at the slot's value, and sets each slot to its knob's pipe, so it comes before `play`:

```lisp
(let ((synth (make subtractive :cutoff 1200hz :level -9db))
      (notes (keys "Keys")))
  (define panel (synth-panel "PAD" synth (section "Keys" notes)))
  (play synth notes out)
  (page panel))
```

The library's controls: every instrument's `:level`; `subtractive`'s, `fm`'s, and `sampler`'s `:attack`, `:sustain`, and `:release`; `subtractive`'s `:cutoff`, `:env-amount`, `:resonance`, and `:decay`; `fm`'s `:ratio` and `:index`; `pluck`'s `:feedback` and `:damping`. The other slots are fixed when the voices are built, so they have none: `:voices`, `:wave`, and `sampler`'s `:buffer`, `:root`, `:start`, and `:loop`. `enveloped`'s `:decay` has none, as `fm` doubles it in arithmetic that takes no pipe; `subtractive` gives its own a control.

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
| `spectral-delay` | `:time`, `:max-time` | 500 ms, 4 s | Each bin's delay before shaping, and the longest |
| | `:shape`, `:spread` | `':random`, 1 | How the delay varies across the spectrum (`':flat`, `':rising`, `':falling`, `':random`), and how far |
| | `:feedback` | 0.5 | Each bin's repeats, relative to the last |
| | `:size` | 2048 | FFT size; the wet signal is this many samples late |
| `freeze` | `:hold` | 0 | Above 0.5, the spectrum it caught sustains; a constant or a control pipe |
| | `:fade`, `:size` | 50 ms, 2048 | Crossfade into and out of the hold; FFT size |
| | `:mix` | 1 | As every effect's, with a default of 1 |
| `granular-delay` | `:time` | 250 ms | How far behind the input the grains read |
| | `:length` | 2 s | The ring the input is recorded into; `:time` and `:jitter` fit inside it |
| | `:density`, `:duration` | 20 Hz, 80 ms | Grains a second, and each grain's length |
| | `:jitter` | 20 ms | A random delay, up to this amount, taken off each grain's time |
| | `:pitch`, `:pitch-jitter` | 0, 0 | Transposition of the grains in semitones, and a random transposition up to this amount either way |
| | `:record` | 1 | 0 freezes the ring, so the grains keep playing what it holds; a constant or a control pipe |

`echo` is a damped feedback comb. `chorus` is a delay line whose delay an LFO sweeps. `reverb` is Schroeder's reverberator with Freeverb's tunings: four damped feedback combs in parallel, then two allpass diffusers in series. Its input is scaled by `1 - size`, so the gain of the combs, which grows as `1 / (1 - size)`, does not make a larger reverb louder. `overdrive` drives the input into `tanh` saturation and low-pass filters the result. `spectral-delay` passes its input through `fft`, `pv-delay`, and `ifft`, so each frequency band repeats at its own time. `freeze` passes it through `fft`, `pv-freeze`, and `ifft`. Both delay their wet signal by the FFT's size, which their dry signal does not have. `granular-delay` records its input into a ring with `buffer-write` and plays it back with `grains` at `time` behind the writer's head; its `:record` set to 0, by a sustain pedal mapped to a control pipe for instance, freezes the ring into a sustained cloud of the last moments recorded.

### Pedals and chains

`(chain in out effect ...)` runs the effects in series from the audio pipe `in` to the audio pipe `out`, each as `(stomp effect from to)`, and returns a `pedalboard` of their pedals, in the order of the chain, for `page`:

```lisp
(defpiece main ()
  (let ((guitar (pipe :audio)) (out (pipe :audio)))
    (audio-in :channel 0 :out guitar)
    (page (chain guitar out (make overdrive) (make echo :time 300ms) (make reverb)))
    (audio-out :in out :channel 0)))
```

`(stomp effect in out)` draws the effect as a `pedal` of its `:control` slots with a `footswitch` labelled On, scales its mix by the footswitch, so a bypassed effect passes its dry input, processes it from `in` to `out`, and returns the pedal. A new effect class with `:control` slots is a pedal on the board as the library's are:

```lisp
(defclass fuzz (effect)
  ((drive :initarg :drive :initform 0.6 :accessor fuzz-drive :control (knob :unit :percent))
   (tone  :initarg :tone  :initform 3khz :accessor tone :control (knob :min 500hz :max 8khz))))
```

The library's controls: every effect's `:mix`; `echo`'s `:feedback` and `:damping`; `chorus`'s `:rate`; `reverb`'s `:damping`; `overdrive`'s `:drive`, `:tone`, and `:level`; `spectral-delay`'s `:time`, `:spread`, and `:feedback`; `freeze`'s `:hold`, as a toggle; `granular-delay`'s `:density`, `:duration`, `:jitter`, `:pitch`, and `:pitch-jitter`. The other slots are fixed when the effect is built or are used in arithmetic that takes no pipe, so they have none: `echo`'s `:time`, `chorus`'s `:delay` and `:depth`, `reverb`'s `:size`, lengths, and FFT sizes.

Each slot of the library classes has an accessor named after its initarg, except `effect`'s `:mix`, whose accessor is `wet-mix`, as `mix` is a primitive; `instrument`'s `:voices` has `voice-count`. Accessors are generic functions shared by the classes that have a slot of the name: `(cutoff s)` reads a `subtractive`'s cutoff, `(damping e)` an `echo`'s or a `reverb`'s damping, and `(set! (cutoff s) 600hz)` changes a cutoff before `play` builds the voices.
