# Examples

Each example is a folder: a piece in the Lisp dialect, `NAME/NAME.lisp`, and the IR it compiles to, `NAME/NAME.ir.json`, with its sample files in `samples/` and its page in `ui/` where it has them. `hectodrone`, `organ-pad`, and `phasing` are projects of several files that start at `main.lisp`. The test suite compiles every piece and compares the result with its IR file, and plays each IR file on the engine to check that it behaves as described.

## Running

Compile and play an example on the default audio device. Buffer files resolve against the server's `--root`:

```bash
build/bin/espresso-client --compile examples/wavetable/wavetable.lisp | build/bin/espresso-server --root examples/wavetable --play -
```

Or open the example in the editor, with a server running, and press Play:

```bash
build/bin/espresso-server --root examples/wavetable
```

```bash
build/bin/espresso-client examples/wavetable wavetable.lisp
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

## phasing

Generative loops in the manner of Steve Reich, for a MIDI keyboard with two modulation wheels, written as a project of three files: `phasing/loops.lisp` defines the pattern and the loops that play it, `phasing/marimba.lisp` the instrument, and `phasing/main.lisp` the piece.

| Control | Effect |
|---|---|
| Keys | The loops' pitches: up to five held notes, numbered in the order they were pressed. A pad also plays them as a chord |
| Wheel A (controller 1) | Phasing: the right marimba's clock runs up to 3% faster than the left's, so the two loops drift apart a step at a time. Returned to 0, the clocks run together again and keep the offset they reached |
| Wheel B (controller 2) | Fill: the pattern's steps join one at a time, in a fixed order, as the wheel rises. At 0 only the first step sounds |

The two controller numbers are `wheel-a` and `wheel-b` at the top of `main.lisp`.

Two marimbas, panned to the left and the right, play one pattern of twelve sixteenths at 6.6 steps a second. Each step names a held note by its number, an octave, and the fill at which it joins; a step whose note is not held rests, so holding more notes fills the pattern in. Each loop's clock is a sawtooth LFO on an audio pipe and a `threshold`, so its steps fall on their samples and its rate can follow a wheel. The marimba models a struck bar by its first three modes, near 1, 3.93, and 9.2 times the note's frequency, each shorter than the one before, and a 4 ms mallet of band-passed noise.

Under the loops, a pad holds the held notes and pulses in eighths of the left clock, swelling and fading over fourteen seconds. The marimbas and the pad share a reverb.

## organ-pad

An additive organ pad for a MIDI keyboard with two modulation wheels, with a tape loop to play along with, written as a project of three files: `organ-pad/organ.lisp` defines the instrument, `organ-pad/effects.lisp` the rotary speaker and the tape loop, and `organ-pad/main.lisp` the piece.

| Control | Effect |
|---|---|
| Keys | Six voices; velocity sets each note's level. Envelope: 400 ms attack, 1.8 s release |
| Wheel A (controller 1) | Registration: from a soft flute (down) to a full chorus (up) |
| Wheel B (controller 2) | The tape loop's feedback, from 0 (one repeat) to 0.96 (repeats that fade over minutes) |

The two controller numbers are `wheel-a` and `wheel-b` at the top of `main.lisp`.

Each voice adds nine sines at the footages of a drawbar organ: 16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', and 1'. The drawbar levels are control pipes shared by every voice: wheel A morphs them from one registration to the other, and each drawbar has an LFO of its own, at rates from 0.023 to 0.131 Hz, that moves its level by 35%, so the tone changes without repeating.

The organ feeds two chains, one for each side, each a rotary speaker, a tape loop, and the standard library's reverb:

| Effect | Behavior |
|---|---|
| `rotary` | A delay swept by the turning horn bends the pitch; the horn's direction moves the level by 25%. Its speed drifts on a slow LFO between 0.6 and 1.2 Hz. The two sides' horns turn half a turn apart |
| `tape-loop` | What is played comes back after 7.5 s on the left and 8.1 s on the right, and again at the feedback's level, each pass darker. The tape's speed wavers by 1.5 ms, so the repeats drift slightly in pitch. The input passes at full level, with its repeats added |
| `reverb` | Size 0.9, 35% wet |

The two tape lengths differ, so a phrase's repeats move between the speakers and fall out of step with each other.

Both pieces run on the server comfortably in a Release build. In a Debug build the organ uses most of a processor core.

```bash
build-release/bin/espresso-client --compile examples/phasing/main.lisp | build-release/bin/espresso-server --play -
```

```bash
build-release/bin/espresso-client --compile examples/organ-pad/main.lisp | build-release/bin/espresso-server --play -
```

## straintest

The piece of the v2 load test: eight instruments of eight voices, each through two effects; no device input is needed. Each instrument plays a note every quarter beat that lasts two beats, from a sequence of eight pitches of its own, so all 64 voices sound once the first two beats have passed.

| Class | Voice |
|---|---|
| `stack`, a subclass of `subtractive` | A sawtooth or pulse oscillator and a ladder filter for each layer, the filters opened by one envelope |
| `fm-stack`, a subclass of `fm` | A modulator and a carrier for each layer, their depth set by one envelope |
| `strings`, a subclass of `pluck` | A comb for each layer, all excited by one burst of noise, as the courses of a twelve-string guitar are struck together |

Each voice is a stack of detuned layers, spread 0.3% apart about the note, as a unison voice of a polyphonic synthesizer is. `(define layers 8)` sets the number of layers, and with it the weight of the whole piece: at eight, the piece builds 2776 nodes, and one thread of an Intel Core i5-7360U cannot play it in real time. Each instrument passes two of the standard library's effects (`chorus`, `echo`, `reverb`, and `overdrive`), and the eight are mixed to the center of both channels.

Play it from a Release build on several threads, or render it with a profile of where its time goes:

```bash
build-release/bin/espresso-server --threads 4 --play examples/straintest/straintest.ir.json
```

```bash
build-release/bin/espresso-server --render examples/straintest/straintest.ir.json --out straintest.wav --seconds 10 --threads 4 --profile
```

`scripts/straintest.sh` builds the Release server and renders the piece on 1, 2, 4, 8, and 16 threads, alternating the counts round by round. For each count it prints the mean, median, fastest, and slowest render time, the speedup over one thread, and the render's speed against real time, and it checks that every render is the same. `scripts/straintest.sh --help` lists its options:

```bash
scripts/straintest.sh -r 5 -s 10
```

## Samples and spectra

Five pieces play buffers and spectra. Each lives in a folder of its own with its sample files in `samples/`, which is the project root its buffers resolve against: pass the folder to the server with `--root`.

```bash
build/bin/espresso-client --compile examples/grain-cloud/grain-cloud.lisp | build/bin/espresso-server --root examples/grain-cloud --play -
```

```bash
build/bin/espresso-server --render examples/spectral-audioscape/spectral-audioscape.ir.json --out audioscape.wav --seconds 120 --root examples/spectral-audioscape
```

The sample files are synthesized by `make-samples.py` in this folder, from fixed seeds, and are dedicated to the public domain (CC0 1.0). Running it from `examples/` writes them again:

| File | Sound |
|---|---|
| `spectral-audioscape/samples/field.wav` | Six seconds of wind, rain, and a few birds, mono, 22.05 kHz |
| `sampler-keys/samples/c4.wav` | A struck middle C with slightly stretched partials, two seconds |
| `grain-cloud/samples/glass.wav` | Struck glass at five pitches, three seconds |
| `wavetable/samples/tables.wav` | Eight tables of 2048 samples, from a sine to a bright saw to a hollow square, at 48 kHz |

Files load resampled to the server's rate. The wavetable piece declares its buffer with `:resample #f`, so `tables.wav` loads as it is and its tables keep their 2048-sample frames at any server rate.

### spectral-audioscape

A generative audioscape. A 20-second stereo ring, preloaded with the field recording, is read by three heads, a third of the ring apart, that play it at nearly its own speed and drift apart over minutes. Their mix passes:

| Stage | Behavior |
|---|---|
| `fft`, `pv-delay` | 4096-point frames; each bin delayed by a random time up to 2 s, with feedback 0.5. Every 8 beats, a coin toss redraws the delays |
| `pv-freeze` | Every 16 beats, another coin toss turns the freeze on or off; each change fades over 2 s |
| `ifft`, `reverb` | Back to audio, through the standard library's reverb (size 0.9, 40% wet) and a 40 Hz high-pass |

The result is played and also recorded back into the ring with a `:keep` of 0.7, through a saturation: quiet material grows with each pass and fills the ring past the end of the recording, and loud material is held to a level the ring keeps. Over the first minutes the sound grows from the recording into a self-sustaining texture that keeps changing. The same seed renders the same output.

### sampler-keys

A sampled piano for any MIDI keyboard: the `sampler` instrument plays one recording of middle C across the keyboard, an octave up at twice the speed. Eight voices; each note releases over 400 ms.

### freeze-pedal

A granular delay on audio input 1, frozen by the sustain pedal. Grains of 120 ms, 25 a second, read the input 300 ms late, each up to 40 ms later still. While the pedal (controller 64) is down, the delay stops recording and its grains keep playing the last two seconds it heard. Use headphones.

### grain-cloud

A cloud of grains over struck glass. One slow LFO moves where the grains read, another how many start each second (6 to 30); each grain is jittered in time and pitch, reversed one time in five, and spread across the stereo field.

### wavetable

A drone of three wavetable oscillators, a fifth and an octave apart, sharing one position: a triangle LFO sweeps it from the sine table to the square and back every ten seconds. Each oscillator plays band-limited versions of its tables, so the bright tables do not alias.

## Serial devices, web data, and pages

These examples read a serial device, web data, or a page in a browser. Their tests feed each source through the engine as its bridge would, so they open no device and make no request; played on a server, they do. An offline render opens none of them: its sources stay at their initial values.

### serial-knob

A drone whose brightness follows a potentiometer. Two sawtooths 0.4 Hz apart pass a ladder filter whose cutoff, 200 Hz to 4 kHz, follows the reading from the first USB serial device at 115200 baud, smoothed over 30 ms. A board with a potentiometer on its first analog input prints the readings with this sketch:

```cpp
void setup() { Serial.begin(115200); }
void loop() { Serial.println(analogRead(A0)); delay(10); }
```

On Linux, opening the device needs membership in the `dialout` group.

### weather

The weather in Chicago, from Open-Meteo, polled once a minute. Two `web-in` nodes read the temperature and the wind speed from one URL and share each request. The temperature, -20 to 40 degrees Celsius, sets a sine and a triangle an octave up from 110 Hz to 880 Hz; the wind, 0 to 60 km/h, raises a pink noise from silence to about the tone's level. Both glide with a 2 s time constant.

### edits

Wikipedia, edit by edit. `web-stream` holds open Wikimedia's stream of recent changes, a server-sent event for each edit to a Wikimedia wiki, tens a second. Each edit plucks a note of a pentatonic scale drawn at random; the page's new length sets the velocity on a log scale, soft for a stub and full for a long article. Wikimedia asks clients to name themselves with a contact in `User-Agent`; add one to the header before sharing a piece that uses the stream.

### firehose

Bluesky, post by post. `web-socket` reads Jetstream, which carries every public post as a JSON message, tens a second; each post plays a short FM note through a reverb. The messages carry no number for a post's length, and a field path reads numbers, not the length of a text, so each note's pitch and duration are drawn at random from a scale and a set of lengths.

### report

A piece that reports its loudness. Three detuned sines swell under an LFO; at the start and every 4 beats, `web-out` posts `{"loudness": x}`, the output's magnitude averaged over 300 ms, to `http://localhost:8080/loudness`. A listener that prints what it receives:

```bash
python3 -c 'import http.server as h
class R(h.BaseHTTPRequestHandler):
    def do_POST(s):
        print(s.rfile.read(int(s.headers["Content-Length"])).decode()); s.send_response(204); s.end_headers()
h.HTTPServer(("127.0.0.1", 8080), R).serve_forever()'
```

### page-keys

A synthesizer played from a page. The project's `project.lisp` gives it the page port 8000: while it plays, open `http://localhost:8000/`. The page's keyboard plays eight subtractive voices through `osc-notes`, its slider sweeps a low-pass filter after them from 200 Hz to 12 kHz, and its meter shows the piece's level.

```bash
build/bin/espresso-client --compile examples/page-keys/page-keys.lisp > page-keys.ir.json
build/bin/espresso-server --root examples/page-keys --play page-keys.ir.json
```

With `--page-lan`, the server also prints a pairing link for a phone or tablet on the same network.

### mini-clone

A monophonic synthesizer laid out after the Minimoog Model D, played from its panel at `http://localhost:8000/`: the M6 acceptance piece. Run it as `page-keys` runs, with `--root examples/mini-clone`.

| Section | Controls |
|---|---|
| Controllers | Tune (±2 semitones), glide, and modulation mix (oscillator 3 to noise) |
| Oscillator bank | Three oscillators, each with a range (LO, 32', 16', 8', 4', 2') and six waveforms (triangle, triangle and sawtooth, reverse sawtooth, square, wide and narrow pulse); oscillators 2 and 3 tune ±7 semitones. Oscillator modulation sends the modulation to the first two; oscillator 3's control switch takes it off the keyboard, for a slow modulation source |
| Mixer | Volumes and switches for the three oscillators, the external input (device input 1), and white or pink noise |
| Modifiers | A ladder filter: cutoff (±5 octaves around 440 Hz), emphasis, and amount of contour (up to 5 octaves), with filter modulation and keyboard control 1 and 2 (a third and two thirds of the keyboard's octaves); filter and loudness contours of attack, decay, and sustain |
| Output | Volume, the main output switch, and an A-440 reference tone |
| Left of the keys | Glide and decay switches, the pitch wheel (±2 semitones), and the mod wheel, which scales the modulation |

The contours release over their decay while the decay switch is on, and at once while it is off. Glide blends glides of 60 ms, 250 ms, and 1 s, as the knob turns. The panel sends every control's value when it connects, so the piece starts where the panel is; until then the piece is silent.
