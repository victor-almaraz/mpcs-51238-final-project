# Examples

Each example is a folder: a piece in the Lisp dialect, `NAME/NAME.lisp`, with its sample files in `samples/` where it has them. The server serves its page at `http://localhost:8000/` while the piece plays, and the editor shows it in a tab. An instrument's page is a synthesizer's panel, with a pedal for each effect in its chain; a piece of effects alone is a pedalboard.

Every page but two is drawn from the piece's layout: the piece declares its controls, displays, panels, and pedals in the language (`docs/language.md`, Controls in the language), and each control starts where the piece starts it, so a piece plays as written without its page. Such an example's `ui/` holds only what the language does not draw: `ui/script.js` adds its screens (a recording's waveform, a stream's rate, a pattern's steps) and units the language does not name, and `ui/style.css` styles them. `mini-clone` and `rack` have custom pages, not drawn from a layout: each is its own `ui/index.html`, a panel drawn in SVG with the page controls placed over the drawing, and its piece reads the page with `osc-in`.

`hectodrone`, `organ-pad`, and `phasing` are projects of several files that start at `main.lisp`. The test suite compiles every example and checks that it compiles to the same valid IR every time, and plays each piece of one file on the engine to check that it behaves as described. `espresso-client --compile` writes a piece's IR; the server plays it from a file or from standard input (`-`).

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
| Keys | Play notes, at their velocity. Each note takes a voice, and all one hundred of the voice's oscillators play it. With four notes held, a new note takes the voice of the oldest note at once and restarts its envelope (10 ms attack, 1 s decay to 80%, 400 ms release); releasing a note whose voice a newer note has taken does nothing |
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

Its page is the synthesizer's panel above a reverb pedal: the detuning, which adds to the pitch bend, over a screen of the hundred oscillators' spread; the filter's cutoff with the modulation wheel down and up, and its resonance; attack, decay, sustain, and release; the volume, with a level meter; and a keyboard that plays the voices as a MIDI keyboard does. The pedal holds the reverb's mix and damping, and a footswitch that bypasses it.

## phasing

Generative loops in the manner of Steve Reich, for a MIDI keyboard with two modulation wheels, written as a project of three files: `phasing/loops.lisp` defines the pattern and the loops that play it, `phasing/marimba.lisp` the instrument, and `phasing/main.lisp` the piece.

| Control | Effect |
|---|---|
| Keys | The loops' pitches: up to five held notes, numbered as they are pressed: a new note takes the lowest number no held note has. A pad also plays them as a chord |
| Wheel A (controller 1) | Phasing: the right marimba's clock runs up to 3% faster than the left's, so the two loops drift apart a step at a time. Returned to 0, the clocks run together again and keep the offset they reached |
| Wheel B (controller 2) | Fill: the pattern's steps join one at a time, in a fixed order, as the wheel rises. At 0 only the first step sounds |

The two controller numbers are `wheel-a` and `wheel-b` at the top of `main.lisp`.

Two marimbas, panned to the left and the right, play one pattern of twelve sixteenths at 6.6 steps a second. Each step names a held note by its number, an octave, and the fill at which it joins; a step whose note is not held rests, so holding more notes fills the pattern in. Each loop's clock is a sawtooth LFO on an audio pipe and a `threshold`, so its steps fall on their samples and its rate can follow a wheel. The marimba models a struck bar by its first three modes, near 1, 3.93, and 9.2 times the note's frequency, each shorter than the one before, and a 4 ms mallet of band-passed noise.

Under the loops, a pad holds the held notes and pulses in eighths of the left clock, swelling and fading over fourteen seconds. The marimbas and the pad share a reverb.

Its page is the loops' panel above a reverb pedal: the tempo, the phasing, and the fill (the last two also on wheels A and B, the higher counting), over a screen of the twelve steps that shows each marimba's place, each step's held note, and the steps the fill has joined; the pad's cutoff, resonance, attack, and release; the breath's rate and depth; the marimbas' and pad's levels, with a level meter; and a keyboard that holds the loops' notes. The pedal holds the reverb's mix and damping, and a footswitch that silences its return.

## organ-pad

An additive organ pad for a MIDI keyboard with two modulation wheels, with a tape loop to play along with, written as a project of three files: `organ-pad/organ.lisp` defines the instrument, `organ-pad/effects.lisp` the rotary speaker and the tape loop, and `organ-pad/main.lisp` the piece.

| Control | Effect |
|---|---|
| Keys | Six voices; velocity sets each note's level. Envelope: 400 ms attack, 1.8 s release |
| Wheel A (controller 1) | Registration: from a soft flute (down) to a full chorus (up) |
| Wheel B (controller 2) | The tape loop's feedback, from 0 (one repeat) to 0.96 (repeats that fade over minutes) |

The two controller numbers are `wheel-a` and `wheel-b` at the top of `main.lisp`.

Each voice adds nine sines at the footages of a drawbar organ: 16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', and 1'. The drawbar levels are control pipes shared by every voice: wheel A morphs them from one registration to the other, and each drawbar has an LFO of its own, at rates from 0.023 to 0.131 Hz, that moves its level by 35%, so the tone changes without repeating.

The organ feeds one `chain` of a rotary speaker, a tape loop, and the standard library's reverb. The rotary widens the mono organ to two sides, and the tape loop and the reverb run on both:

| Effect | Behavior |
|---|---|
| `rotary` | A delay swept by the turning horn bends the pitch; the horn's direction moves the swept signal's level by up to 25%, and the rotary mixes 70% swept signal with 30% dry. Its speed drifts on a slow LFO between 0.6 and 1.2 Hz. The two sides' horns turn half a turn apart |
| `tape-loop` | What is played comes back after 7.5 s on the left and 8.1 s on the right, and again at the feedback's level (its Repeats knob, or wheel B where that is higher), each pass darker. The loop's length wavers by ±1.5 ms, so the repeats drift slightly in pitch. The input passes at full level, with its repeats added |
| `reverb` | Size 0.9, 35% wet |

The two tape lengths differ, so a phrase's repeats move between the speakers and fall out of step with each other.

Both pieces run on the server comfortably in a Release build. In a Debug build the organ uses most of a processor core.

```bash
build-release/bin/espresso-client --compile examples/phasing/main.lisp | build-release/bin/espresso-server --play -
```

```bash
build-release/bin/espresso-client --compile examples/organ-pad/main.lisp | build-release/bin/espresso-server --play -
```

Its page is the organ's panel above three pedals: the nine drawbars, 0 to 8 each, starting at the soft flute; the registration, which morphs from them to the full chorus and adds to wheel A (the higher of the two counts); and the drawbars' drift; the attack and release; the volume, with a level meter; and a keyboard. The pedals are the chain's: the rotary's mix; the tape loop's mix, its repeats, which add to wheel B as the registration does to wheel A, and its damping; and the reverb's damping and mix. Each pedal's footswitch bypasses it.

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
build/bin/espresso-client --compile examples/straintest/straintest.lisp | build-release/bin/espresso-server --threads 4 --play -
```

```bash
build/bin/espresso-client --compile examples/straintest/straintest.lisp | build-release/bin/espresso-server --render - --out straintest.wav --seconds 10 --threads 4 --profile
```

`scripts/straintest.sh` builds the Release server and client, compiles the piece, and renders it on 1, 2, 4, 8, and 16 threads, alternating the counts round by round. For each count it prints the mean, median, fastest, and slowest render time, the mean less the setup time (the process's start, reading the IR, and building the plan, timed each round by a render of 0.01 s on one thread), the speedup over one thread, and the render's speed against real time, and it checks that every render is the same. `scripts/straintest.sh --help` lists its options:

```bash
scripts/straintest.sh -r 5 -s 10
```

Its page is the load test's rig: a strip for each of the eight instruments, named by its number and instrument, with the note its sequencer plays, a light at each note, and the instrument's level before its effects; each strip sets the instrument's velocity and the wet mix of both its effects. A master meter shows the whole piece's level. The page adds two inputs and three outputs an instrument and nothing a voice, so the test's cost stays the piece's own.

## Samples and spectra

Five pieces play buffers and spectra. Each lives in a folder of its own, the project root its buffers resolve against: pass the folder to the server with `--root`. The four that read files keep them in `samples/`; `freeze-pedal` records only its input.

```bash
build/bin/espresso-client --compile examples/grain-cloud/grain-cloud.lisp | build/bin/espresso-server --root examples/grain-cloud --play -
```

```bash
build/bin/espresso-client --compile examples/spectral-audioscape/spectral-audioscape.lisp | build/bin/espresso-server --render - --out audioscape.wav --seconds 120 --root examples/spectral-audioscape
```

The sample files are synthesized by `make-samples.py` in this folder, from fixed seeds, and are dedicated to the public domain (CC0 1.0). Running it from `examples/` writes them again:

| File | Sound |
|---|---|
| `spectral-audioscape/samples/arpeggio.wav` | Six seconds of an FM piano arpeggio in the impressionist manner: an Fmaj7 chord rising over two octaves, F3 to F5, and falling back to A3; sixteen notes 0.3125 s apart, left to ring; mono, 44.1 kHz |
| `sampler-keys/samples/c4.wav` | A struck middle C with slightly stretched partials, two seconds |
| `grain-cloud/samples/glass.wav` | Struck glass, eight strikes 0.4 s apart at four pitches, three seconds |
| `wavetable/samples/tables.wav` | Eight tables of 2048 samples, from a sine to a bright saw to a hollow square, at 48 kHz |

Files load resampled to the server's rate. The wavetable piece declares its buffer with `:resample #f`, so `tables.wav` loads as it is and its tables keep their 2048-sample frames at any server rate.

### spectral-audioscape

A generative audioscape. One head loops a short FM piano arpeggio from a buffer, at its own speed, through:

| Stage | Behavior |
|---|---|
| Spectral delay | Eight bands, two thirds of an octave apart from 250 Hz to 6.3 kHz, each a band-pass `svf` into a feedback delay (`comb`). Each band's delay, from 0.5 to 5 s, and feedback, from 0.3 to 0.8, were drawn at random once and are fixed in the piece |
| Freeze | The standard library's `freeze` (4096-point `fft`, `pv-freeze`, `ifft`). Every 16 beats, a coin toss: the first heads leaves it off, and each later one turns it on or off; each change fades over 2 s |
| Reverb | The standard library's reverb, size 0.9, 40% wet |

The same seed renders the same output.

Its page is a pedalboard of the piece's chain. The loop pedal sets the head's speed through the arpeggio. The spectral delay sets the bands' Q and damping, its LED following the level. The freeze sets the chance of the coin that engages it every 16 beats, and its footswitch holds it as the coin does, its LED lit while either holds it. The reverb sets its mix and tone. A panel under the board holds a feedback slider for each of the spectral delay's eight bands, the volume, and a level meter.

### sampler-keys

A sampled piano for any MIDI keyboard: the `sampler` instrument plays one recording of middle C across the keyboard, an octave up at twice the speed. Eight voices; each note releases over 400 ms.

Its page is the sampler's panel: where in the recording each note starts, marked on a screen of the recording; the attack and release; the volume, with a level meter; and a keyboard set into the panel below them that plays the voices as a MIDI keyboard does.

### freeze-pedal

A granular delay on audio input 1, frozen by the sustain pedal. Grains of 120 ms, 25 a second, read the input 260 to 300 ms late. While the pedal (controller 64) is down, the delay stops recording and its grains keep replaying what it heard from 300 ms to 140 ms before the pedal went down. Use headphones.

Its page is the pedal: Mix, Density, Grain, and Spread knobs, and a Hold footswitch that freezes the delay as the sustain pedal does; the LED, a lamp of the piece's, is lit while either holds it.

### grain-cloud

A cloud of grains over struck glass. One slow LFO moves where the grains read, another how many start each second (6 to 30); each grain is jittered in time and pitch, reversed one time in five, and spread across the stereo field.

Its page is the granulator's panel: the rate, centre, and depth of the LFO that moves where the grains read, over a screen of the glass recording that shades the scanned span and marks the read head; the rate, centre, and depth of the LFO that sets how many grains start each second, with that count; each grain's length, timing and pitch jitter, and chance of reversing; and the stereo spread and the volume, with a level meter.

### wavetable

A drone of three wavetable oscillators, a fifth and an octave apart, sharing one position: a triangle LFO sweeps it from the sine table to the square and back every ten seconds. Each oscillator plays band-limited versions of its tables, so the bright tables do not alias.

Its page is the synthesizer's panel: the lowest oscillator's pitch; the sweep's rate, centre, and depth, under a screen of the eight tables that marks where the sweep is; and the volume, with a level meter.

## Serial devices, web data, and pages

These examples read a serial device, web data, or a page in a browser. Their tests feed each source through the engine as its bridge would, so they open no device and make no request; played on a server, they do. An offline render opens none of them: its sources stay at their initial values.

### serial-knob

A drone whose brightness follows a potentiometer. Two sawtooths 0.4 Hz apart pass a ladder filter whose cutoff, 200 Hz to 4 kHz, follows the reading from the first USB serial device at 115200 baud, smoothed over 30 ms. A board with a potentiometer on its first analog input prints the readings with this sketch:

```cpp
void setup() { Serial.begin(115200); }
void loop() { Serial.println(analogRead(A0)); delay(10); }
```

On Linux, opening the device needs membership in the `dialout` group.

Its page is the drone's panel: the sawtooths' pitch, the ladder filter's resonance, and the volume, with a level meter, beside the board's reading, the cutoff it sets, and a meter of the cutoff's place on its travel from 200 Hz to 4 kHz. It has no files of its own in `ui/`. Only the board turns the cutoff: a page writing the same control pipe would replace the board's readings.

### weather

The weather in Chicago, from Open-Meteo, polled once a minute. Two `web-in` nodes read the temperature and the wind speed from one URL and share each request. The temperature, -20 to 40 degrees Celsius, sets a sine and a triangle an octave up from 110 Hz to 880 Hz; the wind, 0 to 60 km/h, raises a pink noise from silence to about the tone's level. Both glide with a 2 s time constant.

Its page is the synthesizer's panel: a screen of Chicago's temperature and wind as the piece glides to them, each on its scale; the tone's volume, with the pitch the temperature gives it; and meters of the wind's level and the output's level.

### edits

Wikipedia, edit by edit. `web-stream` holds open Wikimedia's stream of recent changes, a server-sent event for each edit to a Wikimedia wiki, tens a second. Each edit plucks a note of a pentatonic scale drawn at random; the page's new length sets the velocity on a log scale, soft for a stub and full for a long article. Wikimedia asks clients to name themselves with a contact in `User-Agent`; add one to the header before sharing a piece that uses the stream.

Its page is the string synthesizer's panel and its reverb pedal: a screen of the edits a second and the last edit's size, with a light at each edit; the strings' volume, with the velocity the last edit played; and a level meter. The reverb pedal below it holds the reverb's damping and mix and a footswitch that bypasses it.

### firehose

Bluesky, post by post. `web-socket` reads Jetstream, which carries every public post as a JSON message, tens a second; each post plays a short FM note through a reverb. The messages carry no number for a post's length, and a field path reads numbers, not the length of a text, so each note's pitch and duration are drawn at random from a scale and a set of lengths.

Its page is the FM synthesizer's panel and its reverb pedal: a screen of the posts a second and the last post's note, with a light at each post; the modulation index and the notes' velocity; the envelope's attack, sustain, and release; and the volume, with a level meter. The reverb pedal below it holds the reverb's damping and mix and a footswitch that bypasses it.

### report

A piece that reports its loudness. Three detuned sines swell under an LFO; at the start and every 4 beats, `web-out` posts `{"loudness": x}`, the output's magnitude averaged over 300 ms, to `http://localhost:8080/loudness`. A listener that prints what it receives:

```bash
python3 -c 'import http.server as h
class R(h.BaseHTTPRequestHandler):
    def do_POST(s):
        print(s.rfile.read(int(s.headers["Content-Length"])).decode()); s.send_response(204); s.end_headers()
h.HTTPServer(("127.0.0.1", 8080), R).serve_forever()'
```

Its page is the synthesizer's panel: the three sines' pitches; the swell's rate, centre, and depth, over a screen that draws the swell's last 20 seconds; and the report, a screen of the request it posts with the loudness posted last, a light at each report, and the requests dropped, over a loudness meter.

### page-keys

A synthesizer and its pedals played from a page drawn from the instrument and the effects themselves. The project's `project.lisp` gives it the page port 8000: while it plays, open `http://localhost:8000/`. `synth-panel` draws a knob for each of the subtractive synthesizer's `:control` slots (cutoff, envelope amount, resonance, the envelope, and the level), starting where `make` sets it; `chain` draws the chorus, echo, and reverb as pedals with footswitches that bypass them. The page's keyboard plays the eight voices, and its meter shows the piece's level.

```bash
build/bin/espresso-client --compile examples/page-keys/page-keys.lisp > page-keys.ir.json
build/bin/espresso-server --root examples/page-keys --play page-keys.ir.json
```

With `--page-lan`, the server also prints a pairing link for a phone or tablet on the same network.

### mini-clone

A monophonic synthesizer laid out after the Minimoog Model D, set from its panel at `http://localhost:8000/` and played from a MIDI keyboard: the M6 acceptance piece. Run it as `page-keys` runs, with `--root examples/mini-clone`. Its page is a custom one, its own `ui/index.html`, not drawn from the language: the panel is drawn in SVG and the page controls sit over it.

| Section | Controls |
|---|---|
| Controllers | Tune (±2 semitones), glide, modulation mix (oscillator 3 to noise), and the glide and decay switches |
| Oscillator bank | Three oscillators, each with a range (LO, 32', 16', 8', 4', 2') and six waveforms (triangle, triangle and sawtooth, sawtooth, square, wide and narrow pulse; on oscillator 3, reverse sawtooth in place of triangle and sawtooth); oscillators 2 and 3 tune up to a major sixth (±9 semitones) either way. Oscillator modulation sends the modulation to the first two; oscillator 3's control switch takes it off the keyboard, for a slow modulation source, and its frequency knob then sweeps three octaves either way |
| Mixer | Volumes and switches for the three oscillators, the external input (device input 1), and white or pink noise |
| Modifiers | A ladder filter: cutoff (±5 octaves around 440 Hz), emphasis, and amount of contour (up to 5 octaves), with filter modulation and keyboard control 1 and 2 (a third and two thirds of the keyboard's octaves); filter and loudness contours of attack, decay, and sustain |
| Output | Volume, the main output switch, and an A-440 reference tone |
| MIDI keyboard | The keys, the pitch wheel (±6 semitones, half an octave), and the mod wheel (controller 1), which scales the modulation |

The contours release over their decay while the decay switch is on, and at once while it is off. Glide blends glides of 60 ms, 250 ms, and 1 s, as the knob turns. The panel sends every control's value each time it connects, so the piece starts where the panel is; until then the piece is silent.

### rack

A fixed modular synthesizer, patched with cables on its page at `http://localhost:8000/` and played from a MIDI keyboard. Run it as `page-keys` runs, with `--root examples/rack`. Its page is a custom one, its own `ui/index.html`, not drawn from the language: the modules and cables are drawn in SVG and the page controls sit over them.

Thirteen modules stand in the rack in this order: the keyboard, an LFO, noise, two envelopes, sample and hold, two VCOs, a mixer, a VCF, a VCA, a delay, and the output. Every cable that can exist is built, as a gain from an output into an input that the page sets to 1 while the cable is plugged and to 0 while it is pulled; an input hears the sum of its cables. A cable into the same module or an earlier one passes through a feedback pipe and arrives a block later. Gate cables carry events: the keyboard's notes and the LFO's clock reach the envelopes and the sample and hold, and a gate from the clock starts an envelope that does not sustain.

A new page starts from a two-oscillator voice through the filter, the amplifier, and the delay. The page keeps its patch and knobs in the browser's local storage, so a reload starts where it left off. Its level lamp shows the level at the output module's inputs, before its volume.
