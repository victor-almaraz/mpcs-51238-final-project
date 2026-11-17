# Examples

Each example is a piece in the Lisp dialect (`NAME.lisp`) and the IR it compiles to (`NAME.ir.json`). The test suite compiles every `.lisp` file here and compares the result with its `.ir.json` file, and plays each IR file on the engine to check that it behaves as described.

## Running

Compile and play an example on the default audio device:

```bash
build/bin/espresso-client --compile examples/misc/monosynth.lisp | build/bin/espresso-server --play -
```

Or open the example in the editor, with a server running, and press Play:

```bash
build/bin/espresso-server
```

```bash
build/bin/espresso-client examples/misc monosynth.lisp
```

The reverb, delay, and pedalboard examples process the audio input. Use headphones: with a microphone and speakers they feed back.

## monosynth

A monophonic synthesizer for any MIDI keyboard.

| Control | Effect |
|---|---|
| Keys | Play notes. The pitch glides to each new note over 40 ms |
| Pitch bend | ±2 semitones |
| Controller 74 | Filter cutoff, 300 Hz to 8 kHz |
| Controller 71 | Filter resonance, 0 to 0.9 |

A sawtooth and a slightly sharp pulse feed a resonant ladder filter. A filter envelope adds up to 3 kHz to the cutoff at each note; an amplitude envelope (4 ms attack, 250 ms release) is gated by the keys. Before controller 74 moves, the cutoff rests at 300 Hz.

The piece shows mappers: pitch bend becomes a frequency ratio with `exp`, and controller values become cutoff and resonance ranges with `scale-exp` and `scale`.

## reverb

A stereo reverb on audio input 1. Each side is a Schroeder reverberator with Freeverb's tunings: four damped feedback combs in parallel (25 to 36 ms, feedback 0.84), then two allpass diffusers in series. The right side's delays are 0.52 ms longer, which widens a mono input. The output mixes the dry signal at 0.7 with the reverberation at 0.2.

The piece shows a function that builds a subgraph, `reverb-side`, called once per side.

## delay

A stereo echo on audio inputs 1 and 2. A feedback comb repeats the input every 375 ms (a dotted eighth at 120 BPM) at 45% feedback; its damping darkens each repeat. An LFO moves the delay time by ±2 ms at 0.3 Hz. The echoes pass a 3 kHz low-pass filter and are mixed under the dry signal at 0.63.

## orbit

A tone you can see. One slow LFO bends a quiet sine's pitch, moves a circle around the center of the frame, and blends its color from orange to blue; a line joins the center to the circle, inside a gray frame.

The piece shows the visual layer: video pipes chain `background`, `rect`, `line`, and `circle` into one picture for `video-out`; the circle's position follows the LFO through mappers, and its fill follows `lerp-color`'s 4-channel control pipe. Run it with the server's window, or render frames offline:

```bash
build/bin/espresso-client --compile examples/misc/orbit.lisp > orbit.ir.json
build/bin/espresso-server --render orbit.ir.json --out orbit.wav --seconds 4 --frames frames --fps 30
```

## landscape

A still picture: a light blue sky, a yellow sun, two mountains, and green ground. The piece makes no sound.

The piece shows layering: each call draws over the frame of the call before it, so the sky comes first and the ground last, and the ground covers the foot of both mountains. Its colors are written with `hsb`; the two mountains share a hue and differ in saturation and brightness.

## spin

An orange square with a white outline turns about the center of the frame once every four seconds. A quiet triangle wave rises from 220 Hz to 440 Hz with each turn.

The piece shows `transform`: it moves the origin to the center of the frame and turns everything drawn after it, so the square is drawn about the origin with `:mode :center`. One saw LFO sets both the angle, through a mapper to 0 to 2π, and the pitch, through `scale-exp`.

## meter

A 330 Hz tone swells and fades every two seconds, and a green bar rises and falls with it.

The piece shows an image parameter that follows an audio pipe. A 0.5 Hz sine on an audio pipe sets the tone's gain, from 0 to 0.3, and the top of the bar, from y 660 to y 60; the bar reads the pipe's latest sample at each frame. The bar is a `rect` with `:mode :corners`, from its foot at (560, 660) to (720, top).

Render any of the three to frames:

```bash
build/bin/espresso-client --compile examples/spin.lisp > spin.ir.json
build/bin/espresso-server --render spin.ir.json --out spin.wav --seconds 4 --frames frames --fps 30
```

## ensemble

A sequenced trio at 100 BPM, built from the standard library (`docs/library.md`): no device input is needed.

| Part | Instrument | Notes | Effects |
|---|---|---|---|
| Bass | `subtractive`, 2 voices | A, A, C, G, a beat each | None |
| Arpeggio | `pluck`, 6 voices | An A minor seventh arpeggio up and down, a quarter beat a note | `chorus` |
| Bells | `fm`, ratio 3.5, 3 voices | A, E, G, C, two beats each | `echo`, three quarters of a beat |

The three parts are mixed, through a `reverb`, to both channels. Each part is a `metro` stepping a `sequencer`, whose values pitch the notes `trigger-notes` makes; the `melody` function builds that chain. Each instrument is a class instance, and its `play` method splits the notes among its voices.

```bash
build/bin/espresso-client --compile examples/misc/ensemble.lisp > ensemble.ir.json
build/bin/espresso-server --render ensemble.ir.json --out ensemble.wav --seconds 20
```

## keys

A polyphonic synthesizer for any MIDI keyboard, built from the standard library (`docs/library.md`).

| Stage | Library class | Settings |
|---|---|---|
| Voices | `subtractive`, 8 voices | Sawtooth; cutoff 600 Hz, opened by up to 2 kHz at each note; resonance 0.35; 10 ms attack, 600 ms release; -6 dB |
| Chorus | `chorus` | 35% wet |
| Reverb | `reverb` | Size 0.88, 25% wet |

The output goes to both channels. Each key takes a voice; a ninth held key takes the voice of the oldest note. Note velocity sets each voice's level.

## chimes

Aleatory wind chimes at 72 BPM; no device input is needed.

On each sixteenth note a `chance` node passes the tick with a probability of 0.33. A `sequencer` in `:random` order gives each passed tick a pitch from a pentatonic scale on C (C5 to E6), and `trigger-notes` turns it into a note a beat long. Six `fm` voices, with a modulator ratio of 3.5 and no sustain, ring like bells through an `echo` three quarters of a beat long and a `reverb`.

The draws of `chance` and of the random order come from the project seed, so every render of the piece is identical.

```bash
build/bin/espresso-client --compile examples/misc/chimes.lisp > chimes.ir.json
build/bin/espresso-server --render chimes.ir.json --out chimes.wav --seconds 30
```

## pedalboard

A guitar pedalboard on audio input 1, built from the effects of the standard library and chained in the order of a pedalboard.

| Pedal | Library class | Settings |
|---|---|---|
| Overdrive | `overdrive` | Drive 6, tone 4 kHz, -9 dB, fully wet |
| Chorus | `chorus` | 0.6 Hz, 40% wet |
| Echo | `echo` | Three quarters of a beat (375 ms at 120 BPM), feedback 0.35, 30% wet |
| Reverb | `reverb` | Size 0.8, 20% wet |

The output goes to both channels. The echo's time is in beats, so it follows the tempo.

## organ

A drawbar organ written as a new instrument: the class `organ` is a subclass of the standard library's `instrument` and `enveloped`. Its `voice` method adds sines at the first four harmonics of each note, at the levels in its `drawbars` slot (1, 0.5, 0.3, and 0.2 by default), and applies the library's `amplitude` envelope. The `play` method, inherited from `instrument`, gives it four voices; no device input is needed.

Three sequencers, stepped every two beats, play the chords C major, A minor, F major, and G major, one note of each chord apiece. The piece shows the extension point of the instrument classes: a subclass with a `voice` method is an instrument.

```bash
build/bin/espresso-client --compile examples/misc/organ.lisp > organ.ir.json
build/bin/espresso-server --render organ.ir.json --out organ.wav --seconds 8
```

## hectodrone

A polyphonic synthesizer of four voices, each of one hundred sawtooth oscillators, for any MIDI keyboard, written as a project of two files: `hectodrone/hectodrone.lisp` defines the instrument, a subclass of the standard library's `instrument` with its own `play` method, and `hectodrone/main.lisp` plays it from MIDI input through a reverb on each channel.

| Control | Effect |
|---|---|
| Keys | Play notes, at their velocity. Each note takes a voice, and all one hundred of the voice's oscillators play it. With all four voices sounding, a new note takes the voice of the oldest note at once and restarts its envelope (10 ms attack, 1 s decay to 80%, 400 ms release); releasing a note whose voice a newer note has taken does nothing |
| Pitch bend | The standard deviation of the detuning: none at full bend down, 10 cents at rest, 200 cents at full bend up. The notes' pitch does not change |
| Modulation wheel (controller 1) | The cutoff of a resonant low-pass ladder filter on each channel of each voice, from 300 Hz with the wheel down to 12 kHz with it up. Before the wheel moves, the cutoff rests at 300 Hz |

The oscillators of each voice are spaced around its note's pitch in a normal distribution. Each oscillator's place is a fixed quantile of the standard normal distribution, from about -2.58 to 2.58, and its frequency is the note's frequency times 2 to the power of its place times the standard deviation in octaves. The voices share these one hundred ratios, which the compiler builds once. The oscillators alternate, in order of place, between the left channel and the right, so both channels are spread like the whole. Their starting phases are unrelated, as those of free-running oscillators are.

At full bend, the outermost oscillators lie about five semitones from the note.

The `glide` slot sets a portamento time, 0 by default; the envelope, the detuning, and the filter's range and resonance are slots too, set with `make`: `(make hectodrone :glide 60ms :widest 1200ct)`.

```bash
build/bin/espresso-client --compile examples/hectodrone/main.lisp | build/bin/espresso-server --play -
```

```bash
build/bin/espresso-client examples/hectodrone main.lisp
```
