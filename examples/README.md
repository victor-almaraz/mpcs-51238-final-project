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
