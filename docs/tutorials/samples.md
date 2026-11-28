# Samples and grains

This tutorial builds a piece from one sound file: the file played on the beat, a cloud of grains read from it, a spectral freeze and delay on the cloud, and grains of a live input. It assumes the piece of [A first piece](../guide/first-piece.md) plays, and an audio output.

## A sample on the beat

Create a folder `grains` holding `main.lisp` and the sound file `samples/glass.wav`; `examples/grain-cloud/samples/glass.wav` serves. Any WAV, AIFF, FLAC, MP3, or Ogg Vorbis file serves in its place: the server resamples it to its own sample rate, and a mono buffer takes the average of a file's channels.

```lisp
;; Struck glass, played from a buffer every two beats at four speeds.

(module main)

(defpiece main ()
  (let ((glass (buffer :file "samples/glass.wav"))
        (beat  (pipe :event))
        (rate  (pipe :control))
        (hit   (pipe :audio))
        (out   (pipe :audio)))
    (tempo 80bpm)
    (metro :every 2beat :out beat)
    (sequencer :in beat :values [1 0.75 1.5 0.5] :out rate)
    (sample-player :buffer glass :trigger beat :rate rate :out hit)
    (gain :in hit :amount -6db :out out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
```

- `buffer` loads the file into a buffer, audio memory that the piece's primitives share. Its path is relative to the project root.
- `tempo` sets the beat clock to 80 BPM, and `metro` writes an event to the event pipe `beat` every two beats, 1.5 s.
- `sequencer` moves to the next of four rates at each event and holds it in the control pipe `rate`.
- `sample-player` plays the buffer from its start at each event on `:trigger`, at `:rate` times its own speed: 0.5 plays it an octave lower and twice as long. An event during playback crossfades over 2 ms to the start.

Compile the piece and render 10 seconds of it from the folder that holds `grains`:

```bash
build/bin/espresso-client --compile grains/main.lisp > grains.ir.json
build/bin/espresso-server --render grains.ir.json --out grains.wav --seconds 10 --root grains
```

The two programs take a project root each. The client's `--root`, by default the compiled file's folder, holds `project.lisp` and the files `import` loads. The server's `--root`, by default its working directory, is where buffer files load from. Without `--root grains`, the server refuses the plan: `cannot load samples/glass.wav: no such file, or it cannot be read`. The GUI editor starts its server with the project as its root.

## A cloud of grains

`grains` plays short windowed grains read from a buffer. This stage reads the same buffer, and moves where the grains read with an LFO.

```lisp
;; Struck glass every two beats, over a cloud of grains read from the same buffer.

(module main)

(defpiece main ()
  (let ((glass (buffer :file "samples/glass.wav"))
        (beat  (pipe :event))
        (rate  (pipe :control))
        (hit   (pipe :audio))
        (where (pipe :control))
        (bus   (pipe :audio :channels 2))
        (out   (pipe :audio :channels 2)))
    (tempo 80bpm)
    (metro :every 2beat :out beat)
    (sequencer :in beat :values [1 0.75 1.5 0.5] :out rate)
    (sample-player :buffer glass :trigger beat :rate rate :out hit)
    (pan :in hit :position 0 :out bus)
    ;; Where the grains read: 0.3 s to 2.7 s into the file, and back, every 20 s.
    (lfo :shape :triangle :rate 0.05hz :depth 1.2 :offset 1.5 :out where)
    (grains :buffer glass :position where :density 20hz :duration 150ms
            :jitter 80ms :pitch-jitter 0.5st :reverse 0.2 :spread 0.8 :out bus)
    (gain :in bus :amount -6db :out out)
    (audio-out :in out :channel 0)))
```

- `lfo` sweeps the control pipe `where` along a triangle from 0.3 s to 2.7 s and back every 20 s. Its `:depth` and `:offset` are plain numbers, here seconds, the unit of a buffer position; a time such as `1.2s` is an error there.
- `grains` starts 20 grains a second, each 150 ms long, so about three overlap. Each starts at `:position` plus a random offset up to `:jitter`, is transposed by up to `:pitch-jitter` either way, plays backward with probability `:reverse`, and is panned at random within `:spread` of the centre. The random values come from a generator seeded from the project seed and the node ID, so each render is the same.
- `bus` has two channels, for the panned grains. `pan` writes the hits to the same pipe: writers to an audio pipe sum.
- `audio-out` plays the channels of its pipe from `:channel` on: the left on 0 and the right on 1.

## A spectral freeze

`fft` analyzes a signal into spectral frames, and `ifft` resynthesizes a signal from them; spectral processors between the two change the frames. This stage freezes the cloud's spectrum from a toggle on the project's page and delays each frequency bin on its own. Add `project.lisp` to `grains`:

```lisp
(project :name "grains" :page-port 8000)
```

and replace `main.lisp` with:

```lisp
;; Glass over its cloud of grains, which a spectral freeze holds and a
;; spectral delay smears; Freeze is a toggle on the page.

(module main)

(defpiece main ()
  (let ((glass   (buffer :file "samples/glass.wav"))
        (beat    (pipe :event))
        (rate    (pipe :control))
        (hit     (pipe :audio))
        (where   (pipe :control))
        (cloud   (pipe :audio :channels 2))
        (frames  (pipe :event))
        (held    (pipe :event))
        (smeared (pipe :event))
        (bus     (pipe :audio :channels 2))
        (out     (pipe :audio :channels 2))
        (hold    (toggle "Freeze"))
        (time    (knob "Delay" :min 50ms :max 4s :initial 1s :scale :exp)))
    (tempo 80bpm)
    (metro :every 2beat :out beat)
    (sequencer :in beat :values [1 0.75 1.5 0.5] :out rate)
    (sample-player :buffer glass :trigger beat :rate rate :out hit)
    (pan :in hit :position 0 :out bus)
    (lfo :shape :triangle :rate 0.05hz :depth 1.2 :offset 1.5 :out where)
    (grains :buffer glass :position where :density 20hz :duration 150ms
            :jitter 80ms :pitch-jitter 0.5st :reverse 0.2 :spread 0.8 :out cloud)
    ;; The cloud's spectrum: held while the toggle is on, each bin delayed on its own.
    (fft :in cloud :size 4096 :out frames)
    (pv-freeze :in frames :hold hold :fade 1s :out held)
    (pv-delay :in held :time time :shape :random :feedback 0.5 :mix 0.5 :out smeared)
    (ifft :in smeared :out bus)
    (gain :in bus :amount -6db :out out)
    (audio-out :in out :channel 0)
    (page (section "Spectrum" hold time))))
```

- `toggle` and `knob` add controls to the page and return control pipes. The toggle holds 0 until the page turns it on, so the piece renders unfrozen offline.
- `fft` writes a frame of the cloud's last 4096 samples every 1024 samples, for each channel, to the event pipe `frames`. A frame pipe connects only spectral ports and has one writer.
- `pv-freeze` catches each bin when `:hold` rises above 0.5 and sustains that spectrum while it stays there, crossfading in and out over `:fade`.
- `pv-delay` delays each bin by `:time` times a random draw from 0 to 1, feeds half of each repeat back, and mixes the delayed spectrum with its input.
- `ifft` writes the cloud back into `bus`, 4096 samples, about 85 ms at 48 kHz, after it entered `fft`.
- `page` lays out the controls: a section in a panel, at `http://localhost:8000/` while the piece plays. Their addresses are `/spectrum/freeze` and `/spectrum/delay`.

The `freeze` and `spectral-delay` effects of [std/effects](../library.md#stdeffects) wrap the same chains. With `(import "std/effects")` after `module` and a two-channel audio pipe `frozen`, these lines replace the four spectral calls; the effects mix the dry signal in the time domain, where `pv-delay`'s `:mix` mixes spectra:

```lisp
(process (make freeze :hold hold :fade 1s :size 4096) cloud frozen)
(process (make spectral-delay :time time :feedback 0.5 :mix 0.5 :size 4096) frozen bus)
```

## A ring of live input

`buffer-write` records into a buffer, and a reader at a delay behind its head plays what it recorded, as the `granular-delay` effect in `examples/freeze-pedal` does. This stage records audio input 1 into a ring and plays grains of it; the Freeze toggle also stops the recording.

```lisp
;; Glass, its spectral cloud, and grains of audio input 1 half a second late;
;; Freeze also stops recording the input.

(module main)

(defpiece main ()
  (let ((glass   (buffer :file "samples/glass.wav"))
        (ring    (buffer :seconds 4s))
        (beat    (pipe :event))
        (rate    (pipe :control))
        (hit     (pipe :audio))
        (where   (pipe :control))
        (cloud   (pipe :audio :channels 2))
        (frames  (pipe :event))
        (held    (pipe :event))
        (smeared (pipe :event))
        (input   (pipe :audio))
        (head    (pipe :control))
        (behind  (pipe :control))
        (bus     (pipe :audio :channels 2))
        (out     (pipe :audio :channels 2))
        (hold    (toggle "Freeze"))
        (time    (knob "Delay" :min 50ms :max 4s :initial 1s :scale :exp)))
    (tempo 80bpm)
    (metro :every 2beat :out beat)
    (sequencer :in beat :values [1 0.75 1.5 0.5] :out rate)
    (sample-player :buffer glass :trigger beat :rate rate :out hit)
    (pan :in hit :position 0 :out bus)
    (lfo :shape :triangle :rate 0.05hz :depth 1.2 :offset 1.5 :out where)
    (grains :buffer glass :position where :density 20hz :duration 150ms
            :jitter 80ms :pitch-jitter 0.5st :reverse 0.2 :spread 0.8 :out cloud)
    (fft :in cloud :size 4096 :out frames)
    (pv-freeze :in frames :hold hold :fade 1s :out held)
    (pv-delay :in held :time time :shape :random :feedback 0.5 :mix 0.5 :out smeared)
    (ifft :in smeared :out bus)
    ;; The input, recorded into a ring of 4 s while the toggle is off.
    (audio-in :channel 0 :out input)
    (buffer-write :buffer ring :in input :record (mapper hold (fn (h) (- 1 h)))
                  :position head)
    ;; Grains of the ring, 0.5 s to 0.2 s behind its head.
    (subtract :in head :b 0.5 :out behind)
    (grains :buffer ring :position behind :density 30hz :duration 100ms
            :jitter 300ms :pitch-jitter 0.1st :spread 0.8 :out bus)
    (gain :in bus :amount -6db :out out)
    (audio-out :in out :channel 0)
    (page (section "Spectrum" hold time))))
```

- `(buffer :seconds 4s)` makes a buffer of 4 s of silence, which `buffer-write` records into from `audio-in`, wrapping to its start.
- `:record` is 1 minus the toggle. At 0 it holds the buffer's contents and its head, so the grains keep replaying the last moments recorded.
- `buffer-write` writes its head, in seconds, to `head`. `subtract` takes 0.5 s off it, and a random offset up to `:jitter`, 300 ms, starts each grain between 0.5 s and 0.2 s behind the head.

An offline render opens no device, so the ring records silence there. Play the piece with an input device:

```bash
build/bin/espresso-client --compile grains/main.lisp | build/bin/espresso-server --root grains --play -
```

The `granular-delay` effect of std/effects records a ring and reads grains from it the same way, with its grains at the centre of the stereo field. In place of the `buffer-write`, `subtract`, and second `grains` calls:

```lisp
(process (make granular-delay :time 500ms :length 4s :density 30hz :duration 100ms
               :jitter 300ms :pitch-jitter 0.1st :mix 1
               :record (mapper hold (fn (h) (- 1 h))))
         input bus)
```

## Next

- [Buffers](../language.md#buffers) and [Spectra](../language.md#spectra) in the language reference describe looping, band-limited reads, `buffer-read`, `buffer-save`, `wavetable-osc`, and `pv-blur`.
- [Primitives](../reference/primitives.md) lists every parameter's range and default.
- `examples/grain-cloud` moves the grains' density with a second LFO and draws its controls in a synth panel; `examples/spectral-audioscape` loops a buffer through a delay of eight bands and a freeze; `examples/sampler-keys` plays a buffer from a MIDI keyboard with the `sampler` instrument of [std/instruments](../library.md#stdinstruments).
