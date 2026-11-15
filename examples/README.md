# Examples

Each example is a piece in the Lisp dialect (`NAME.lisp`) and the IR it compiles to (`NAME.ir.json`). The test suite compiles every `.lisp` file here and compares the result with its `.ir.json` file, and plays each IR file on the engine to check that it behaves as described.

## Running

Compile and play an example on the default audio device:

```bash
build/bin/proj-client --compile examples/monosynth.lisp | build/bin/proj-server --play -
```

Or open the example in the editor, with a server running, and press Play:

```bash
build/bin/proj-server
```

```bash
build/bin/proj-client examples monosynth.lisp
```

The reverb and delay process the audio input. Use headphones: with a microphone and speakers they feed back.

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
build/bin/proj-client --compile examples/orbit.lisp > orbit.ir.json
build/bin/proj-server --render orbit.ir.json --out orbit.wav --seconds 4 --frames frames --fps 30
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
build/bin/proj-client --compile examples/spin.lisp > spin.ir.json
build/bin/proj-server --render spin.ir.json --out spin.wav --seconds 4 --frames frames --fps 30
```
