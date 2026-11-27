# Language reference

A piece is a Lisp file whose entry piece, defined with `defpiece`, builds a graph of primitives. The compiler evaluates the file, builds the entry piece, and emits IR (`docs/ir.md`). This reference covers the language the compiler implements.

## Files and pieces

```lisp
(module main)

(defpiece main ()
  (let ((osc (pipe :audio)))
    (sine-osc :freq 440hz :out osc)
    (audio-out :in osc :channel 0)))
```

| Form | Meaning |
|---|---|
| `(module name [(export names...)])` | Names the file's module and the bindings it exports |
| `(import "path" [:only (names...)] [:as prefix])` | Loads a file of the project and binds what it exports |
| `(defpiece name (params...) body...)` | Defines a piece: a function that builds the graph |

The editor plays `main.lisp`; without one, it plays the file being edited. The piece named `main` is built unless another entry is given (`espresso-client --compile FILE --entry NAME`).

A project is a directory of files. `import` paths are relative to the project root, with `.lisp` implied: `(import "util/scales")` loads `util/scales.lisp`. Paths that begin with `std/` name the standard library, which is compiled into the client and which no project file replaces; `docs/library.md` describes it. The editor imports its open buffers, saved or not; `espresso-client --compile` reads files under `--root`, by default the compiled file's directory. `module` and `import` are top-level forms.

A file with an `export` clause exports the names it lists; a file without one exports every top-level binding it defines, and none that it imports. `:only` binds the listed names, each of which must be exported. `:as` binds each name as `prefix/name`. Each file is evaluated once per compilation, at its first import; later imports bind its exports again. An import cycle is an error that lists the files in the cycle. Classes and generic functions are global, whichever file defines them.

```lisp
;; voices.lisp
(module voices (export pad))
(define (pad freq out) (saw-osc :freq freq :out out))

;; main.lisp
(module main)
(import "voices" :only (pad))
(defpiece main ()
  (let ((osc (pipe :audio)))
    (pad 110hz osc)
    (audio-out :in osc :channel 0)))
```

Errors name the file and position of the form at fault, in imported files too. Node IDs begin with the entry file's path, whichever file a node's call is in.

## Projects

`project.lisp` at the project root, when present, holds one form that describes the project. Every argument is optional.

```lisp
(project :name "drift" :root "." :entry main :seed 7 :page-port 8000)
```

| Argument | Meaning |
|---|---|
| `:name` | The project's name, a string |
| `:root` | The project root; only `"."`, the directory that holds `project.lisp` |
| `:entry` | The piece to build, a symbol; default `main` |
| `:seed` | The seed of random generators, a whole number from 0; default 0 |
| `:page-port` | The TCP port, from 1024 to 65535, of the project's pages (see Pages) |

The editor and `espresso-client --compile` read the project file of the project root; on the command line, `--entry` and `--seed` override it. `project.lisp` is not a piece: the editor never plays it. An argument the form does not take, a repeated argument, or a value of the wrong kind is an error at the offending text.

## Forms

| Form | Meaning |
|---|---|
| `(define name value)` | Binds `name` |
| `(define (name params...) body...)` | Defines a function |
| `(let ((name value)...) body...)` | Local bindings, evaluated in order; each may use the ones before it |
| `(fn (params...) body...)` | A function value |
| `(do body...)` | Evaluates in order; returns the last value |
| `(if test then [else])` | Conditional; only `nil` and `#f` are false |
| `(cond (test body...)... (else body...))` | First clause whose test is true |
| `(set! name value)` | Assigns an existing binding |
| `(set! (accessor object) value)`, `(set! (slot-value object 'slot) value)` | Assigns a slot |
| `(quote datum)`, `'datum` | The datum, unevaluated |

Parameter lists accept required parameters, then `&optional`, `&rest`, and `&key` sections. Calls pass positional arguments, then keyword arguments: `(voice 220hz out :level 0.5)`.

`[a b c]` is a vector of the evaluated elements.

## Units

A number with a unit suffix is a quantity. The compiler converts quantities to canonical units.

| Dimension | Suffixes | Canonical unit |
|---|---|---|
| Time | `s`, `ms`, `us`, `smp` | Seconds (`smp` at the server's sample rate) |
| Musical time | `beat`, `bar` | Beats (`1bar` is 4 beats) |
| Frequency | `hz`, `khz`, `bpm` | Hertz |
| Level | `db` | Linear amplitude |
| Interval | `st`, `ct` | Semitones |

| Operation | Result |
|---|---|
| `(+ 1s 250ms)` | Same dimension: `1.25s` |
| `(+ -6db -6db)` | Levels: decibels add, so linear amplitudes multiply |
| `(* 2 440hz)`, `(/ 1s 4)` | A quantity and a plain number: the quantity's dimension. On levels, the linear amplitude is scaled: `(* 2 -6db)` is about 1.0 |
| `(/ 1 2s)`, `(/ 1 4hz)` | One over a time is a frequency, and the reverse |
| `(/ 1s 250ms)` | Same dimension: a plain number |
| `(+ 1s 440hz)`, `(+ 1 1s)` | Error |
| `(- 6db)` | Error: write the level in decibels, `-6db` |

A parameter accepts a plain number in its canonical unit or a quantity of its dimension. A quantity of another dimension is an error, except a beat value for a time parameter that follows a pipe: the compiler adds a `beats-to-seconds` node, so the time follows the tempo, `0.5beat` being 0.25 s at 120 BPM. A beat value for a time parameter that takes only a constant, such as the `max-time` of `delay-line`, is an error.

## Builtins

| Builtin | Meaning |
|---|---|
| `+ - * /` | Arithmetic with dimensions; on pipes inside a mapper, generators |
| `= < > <= >=` | Comparisons of numbers of the same dimension; `=` also compares symbols, keywords, and strings |
| `not` | Logical negation |
| `list` | A vector of its arguments |
| `(map f vector)` | `f` applied to each element, in order; a vector of the results |
| `(range n)` | `[0 1 ... n-1]`, for an `n` from 0 to 100000 |
| `(nth vector i)` | Element `i`, from 0; an index outside the vector is an error |
| `(length vector)` | The number of elements; `nil` has none |
| `(channel-count v)` | The channel count of pipe or buffer `v` |
| `(control-range v)` | The range of the page control whose pipe is `v`, as `[min max scale step]`: `min` and `max` canonical plain numbers, `scale` `:linear`, `:exp`, or `:db`, and `step` 0 or the spacing of its values; a switch runs 0 to 1 in steps of 1, a select over its options, a knob of `:steps` over its steps; a pipe that is no control's is an error |
| `(pipe? v)` | Whether `v` is a pipe or a mapper's output |
| `(error part ...)` | Stops the compile with the parts joined, strings as they are and other values shown, at the call |
| `(seal instance)`, `(sealed? instance)` | Marks an instance's nodes built, and asks whether they are; a page draws no sealed instance's controls |
| `(route instance in out)` | Records that the instance reads pipe `in` (or `#f`) and writes pipe `out`, for the tabs of a page; an instance no group drew records nothing. `effect`'s `process` and `instrument`'s `play` call it |
| `(string-append s ...)` | The strings joined in order, such as an address built from parts: `(string-append "/cable/" name)`; with no arguments, the empty string |
| `(unit-value q 'unit)` | Quantity `q` as a plain number in `unit`: `(unit-value 250ms 's)` is 0.25 |
| `(tempo 96bpm)` | Sets the tempo when the piece starts |
| `neg abs exp log sin cos tanh floor round` | One argument |
| `pow min max quantize` | Two arguments: `(quantize x step)` |
| `clamp` | `(clamp x low high)` |
| `scale`, `scale-exp` | `(scale x in-low in-high out-low out-high)`: a linear or exponential map |

An index or a count, such as the `i` of `nth`, the `n` of `range`, and the `:channels` of `pipe`, is an integer or a real with an integer value. `/`, `floor`, and `round` return reals, so `(nth items (floor (/ k 2)))` is element `k / 2` rounded down. A real with a fractional part, such as `1.5`, is an error.

## Pipes and primitives

```lisp
(pipe :audio)                ; one channel
(pipe :audio :channels 2)
(pipe :control)
(pipe :event)
(pipe :audio :feedback #t)   ; delays its data by one block
```

Every primitive is called by its name in kebab-case (`sine-osc`, `delay-line`, `scale-exp`) with keyword arguments: its parameters and its ports. A parameter takes a constant or a pipe; a port takes a pipe or a vector of pipes.

```lisp
(svf :in osc :cutoff cutoff-pipe :q 0.7 :mode :low :out filtered)
(add :in [a b] :out sum)
```

Choice parameters take keywords (`:mode :low`); text parameters take strings (`:device "keyboard"`); list parameters take vectors (`:levels [1 0.5]`); text list parameters take vectors of strings (`:fields ["/main/temp" "/wind/speed"]`). `docs/ir.md` lists the primitive types; the tables in `src/metadata/primitiveSpec.cpp` list their parameters, ranges, defaults, and ports.

A `sequencer` moves to its next value at each event on `:in` other than a note-off, writes the current value to the control pipe on `:out`, and emits it as a value event on `:events`. `:order` is `:forward` (the default), `:backward`, `:ping-pong` (to the last value and back, without repeating either end), `:random` (drawn from a generator seeded from the project seed and the node ID), or `:once`. `:forward` and `:backward` wrap at the ends. `:once` stops on the last value: later events leave `:out` at the last value and emit nothing on `:events`, so the sequence plays one time. A node that adopts a sequencer's state across a plan swap keeps its position, including the end of a `:once` sequence.

A `clip-player` plays the MIDI files of a clip bank as notes on `:out`. The bank, `:bank`, is a folder of the project, `clips/<bank>/`, whose files `1.mid` to `64.mid`, or with a title after the number, `3 walking bass.mid`, are its slots, from 1; the engine reads a file when a plan that plays its bank is loaded, and again when a page saves one (Pages). A file is a Standard MIDI File of format 0 or 1: its tracks merged, a quarter note a beat, its tempo and meter events ignored. `:select` is the slot playing, from 0, or −1 for none, usually a page control's pipe; `:slots` (8 by default) is how many it has. A change of `:select` waits for the next grid line, a multiple of `:grid` beats (16 by default, four bars of four) on the beat clock: the clip playing plays until then, its notes held there end, and the chosen clip starts on the line. A `clip-player` new to the running plan starts the slot `:select` holds when it starts at once, at the place the clip would have reached had it started on the last grid line, so it keeps time with the clips already playing; it waits for that slot's file to be read. A clip loops at its length rounded up to whole bars of the meter it started in, from the line it started on, so a change of meter takes effect at its next start. A slot without a clip plays nothing, as −1 does. The control pipe on `:playing` holds the slot playing, or −1, and the one on `:queued` the slot it changes to at the next grid line, which is the slot playing when no change waits. One `clip-player` plays a bank. A `clip-player` of the same bank that adopts one's state across a plan swap keeps its clip, its place in it, and the notes it holds; one of another bank adopts nothing.

```lisp
(clip-player :bank "bass" :select slot :grid 16beat :out notes :playing now :queued next)
```

Pipes and primitives can be created only while a piece is built. The compiler orders the nodes by data flow, so calls may appear in any order. A loop, including a node that reads a pipe it writes, needs a feedback pipe.

## Buffers

A buffer is audio memory that a piece declares and its primitives share. `buffer` makes one, while a piece is built, with a channel count from 1 to 8 (default 1) and a length, a file, or both:

```lisp
(buffer :channels 2 :seconds 20s)                        ; 20 s of silence
(buffer :channels 2 :file "samples/rain.wav")            ; the file's length
(buffer :channels 2 :seconds 20s :file "samples/rain.wav")  ; the file, cut or padded to 20 s
(buffer :file "samples/tables.wav" :resample #f)         ; the file's samples, not resampled
```

`:seconds` takes a time or a plain number of seconds; a beat value is an error, as a buffer's length is fixed when it is made. A file path is relative to the project root. WAV, AIFF, FLAC, MP3, and Ogg Vorbis files load, resampled to the server's sample rate. `:resample #f` keeps a file's samples as they are, at its own rate, as a wavetable file needs so that its tables keep their length; its `:seconds` count at that rate. Readers play such a buffer at the file's rate: `sample-player` at `:rate` 1 plays the file at its own speed and pitch, and every time and position in a buffer (`:start`, `:loop-start`, `:loop-end`, `:position`, `:jitter`) counts in the file's seconds. A grain's `:duration` and `:density` stay in the server's time. `buffer-write` resamples what it records into such a buffer to the file's rate, band-limited as fast reads are, so a recording plays back at the speed and pitch it was recorded at; it lags its input by 11 server samples when the file's rate is at least the server's, and by more below it, 20 at half. Its `:position` counts in the buffer's seconds. A file's channels fill the buffer's in order; a mono file fills every channel, and a mono buffer takes the average of a file's channels. A file that is missing or cannot be decoded refuses the plan, and the console points at the `buffer` form.

A buffer is passed like a pipe, to the `:buffer` parameter of the primitives that use it. Its ID derives from its source location, as a node's does, and `:key` names buffers made at one call site. A buffer of the same ID, channel count, length, file, and resampling as one in the plan playing is the same buffer: what was recorded into it survives an edit to the piece.

| Primitive | Parameters and ports |
|---|---|
| `buffer-write` | `:buffer`, `:in` (audio), `:record` (0 to 1; default 1), `:keep` (0 to 1; default 0), `:mode` (`:loop` or `:once`), `:reset` (events), `:position` (out, control: the head in seconds) |
| `buffer-read` | `:buffer`, `:position` (time; a constant, a control pipe, or an audio pipe), `:interpolation` (`:cubic`, `:linear`, or `:none`), `:out` (audio) |
| `grains` | `:buffer`, `:voices` (1 to 128; default 32), `:density` (grains a second; default 10 Hz), `:trigger` (events), `:duration` (default 100 ms), `:position` (time), `:jitter` (time), `:pitch`, `:pitch-jitter` (semitones), `:reverse` (0 to 1), `:spread` (0 to 1), `:window` (`:hann`, `:tukey`, or `:triangle`), `:out` (audio) |
| `wavetable-osc` | `:buffer`, `:freq`, `:phase`, `:position` (0 to 1), `:table-size` (a power of two from 256 to 4096; default 2048), `:out` (audio) |
| `sample-player` | `:buffer`, `:trigger` (events), `:rate` (default 1), `:root` (a note), `:start` (time), `:loop` (`:off`, `:forward`, or `:ping-pong`), `:loop-start`, `:loop-end` (times; `:loop-end` 0 is the buffer's end), `:interpolation`, `:out` (audio), `:done` (out, events) |
| `buffer-save` | `:buffer`, `:trigger` (events), `:file` (a path under the project root; required) |

`buffer-write` records at its head, which advances a frame a sample while it records: each sample becomes the old sample times 1 − r + r × keep, plus the input times r, where r is `:record` ramped over 5 ms. `:record` 1 with `:keep` 0 replaces the contents; a `:keep` between 0 and 1 overdubs, fading older layers by `:keep` each pass; `:record` 0 holds the contents and the head, which freezes the buffer for its readers. In `:loop` mode the head wraps to the start; in `:once` mode it stops at the end. Each event on `:reset` moves the head to the start. While a plan fades out, its writers stop writing.

`buffer-read` reads at `:position`, wrapped to the buffer's length, with one output channel per buffer channel. A position bound to a control pipe is the head at the end of each block, and the reader moves to it a sample at a time along the shorter way around the buffer, so a reader at a writer's `:position` less a delay plays the input that much later:

```lisp
(let ((ring (buffer :seconds 4s))
      (head (pipe :control)) (behind (pipe :control)) (echo (pipe :audio)))
  (buffer-write :buffer ring :in input :keep 0.5 :position head)
  (subtract :in head :b 1.5 :out behind)              ; seconds
  (buffer-read :buffer ring :position behind :out echo))
```

A reader whose position jumps by more than a block crossfades over 10 ms from where it was.

A reader that moves through its buffer faster than a frame a sample, such as `sample-player` at a `:rate` above 1 or a grain pitched up, is band-limited: with `:cubic` or `:linear` interpolation it reads through a windowed-sinc filter cut off at 0.88 of the Nyquist frequency divided by its speed, so what would fold back below the Nyquist frequency is more than 60 dB down. The filter spans 8 zero crossings either side, widened by the speed, up to 8 times. A buffer that no `buffer-write` is bound to keeps levels for this: copies of it at half, a quarter, and so on, down to 1/256 of its rate, each filtered, made by the first reader that needs them. From a speed of 2 a read uses the level at which its speed is between 1 and 2, so a voice pitched up costs at most about five times as much as one at its own pitch, one pitched up by whole octaves no more than one at its own pitch, and the filter holds up to a speed of 512. The levels take about the buffer's own memory again, counted against the server's buffer memory; a buffer they do not fit is read without them. A buffer a writer is bound to has no levels: its reads cost more as they speed up, and past a speed of 8 the filter stays at that width, so what lies between the two cutoffs folds. `:none` reads are not filtered. `grains` reads with `:cubic`.

`buffer-save` writes its buffer to `:file` at each note-on, trigger, or value on `:trigger`, as a 32-bit float WAV file at the buffer's sample rate with the buffer's channels: the server's, or the file's own for a buffer declared `:resample #f`. A save replaces the file and makes the directories on its path. The buffer is copied in pieces of four blocks a block, so a long buffer takes several blocks to copy, and the server writes the file; the console then shows `saved buffer <id> to <file>`, or why the file could not be written. A trigger that arrives while a save is being copied or written is dropped and counted in the drops counter. A path that is absolute or leaves the project root is not written. An offline render writes its saves as a live server does.

```lisp
(let ((take (buffer :seconds 8s)) (every-bar (pipe :event)))
  (buffer-write :buffer take :in input)
  (metro :every 4beat :out every-bar)                  ; the ring, every bar
  (buffer-save :buffer take :trigger every-bar :file "takes/take.wav"))
```

`wavetable-osc` divides its buffer into tables of `:table-size` samples (a mono buffer, or the average of its channels) and plays them as one period of a wave at `:freq`; `:position` crossfades from the first table (0) to the last (1). When the plan is built, each table is resynthesized once per octave of bandwidth, from all its harmonics down to the fundamental, and the oscillator plays the two versions that its frequency allows, crossfaded, so no harmonic reaches half the sample rate and the tone does not alias. Tables are read when the plan is built: recording into the buffer later does not change them. Oscillators of one buffer and table size share their tables, which count once against the server's buffer memory.

`grains` plays short windowed grains read from its buffer. A grain starts `:density` times a second, and at each event on `:trigger`; a density of 0 leaves grains to the triggers. Each grain reads its position, length, and transposition when it starts and keeps them: it starts at `:position` plus a random offset up to `:jitter`, plays at `:pitch` plus a random transposition up to `:pitch-jitter` either way, backward with probability `:reverse`, and is panned at random within `:spread` of the centre. Grains come from a pool of `:voices`; one that starts when every voice is busy replaces the oldest, which fades out over 2 ms. Overlapping grains sum, so a density times a duration of about 2 keeps the level near the buffer's. A `:position` bound to a control pipe follows it as `buffer-read`'s does, so grains can read a writer's ring a delay behind its head. `:out` takes one channel, unpanned, or two, panned; past two, the others stay silent. Random values come from a generator seeded from the project seed and the node ID.

`sample-player` plays its buffer from `:start` at each trigger, value event, or note-on on `:trigger`; note-offs are ignored. `:rate` is its speed, 1 being the buffer's own and a negative rate playing backward. With `:root`, a note-on plays at `:rate` times 2^((note − root)/12), so a note an octave above the root plays at twice the speed. Without a loop, playback stops past either end of the buffer and sends a trigger on `:done` at the first silent sample; `:forward` loops from `:loop-end` back to `:loop-start`, and `:ping-pong` turns at both, playing neither end twice. A trigger during playback crossfades over 2 ms from the old position to the new one. A player that is not playing writes silence. The `sampler` instrument of `std/instruments` plays a buffer with a `sample-player` per voice. A buffer's writers run before its readers in each block, unless a reader feeds its own writer; then the reader runs first, a block behind. Buffers hold memory for as long as the piece plays; the server's `--buffer-memory` bounds the total, and a plan that would exceed it is refused.

## Spectra

`fft` analyzes an audio signal into spectral frames, and `ifft` resynthesizes a signal from them. Frames travel through an event pipe that only spectral ports connect, with one writer; several nodes may read it.

```lisp
(let ((frames (pipe :event)) (out (pipe :audio)))
  (fft :in input :size 2048 :overlap 4 :out frames)
  (ifft :in frames :out out))
```

| Primitive | Parameters and ports |
|---|---|
| `fft` | `:in` (audio), `:size` (a power of two from 256 to 16384; default 2048), `:overlap` (2 or 4; default 4), `:out` (frames) |
| `ifft` | `:in` (frames), `:out` (audio) |
| `pv-delay` | `:in`, `:out` (frames), `:max-time` (up to 60 s; default 4 s), `:time` (default 500 ms), `:shape` (`:flat`, `:rising`, `:falling`, or `:random`), `:spread` (0 to 1; default 1), `:feedback` (−0.99 to 0.99), `:mix` (0 to 1; default 1), `:redraw` (events) |
| `pv-freeze` | `:in`, `:out` (frames), `:hold` (above 0.5 holds), `:fade` (default 50 ms) |
| `pv-blur` | `:in`, `:out` (frames), `:amount` (0 to 1; default 0.5) |

`fft` produces a frame every `size / overlap` samples, the hop: the spectrum of the last `size` samples, windowed, `size / 2 + 1` bins from 0 Hz to half the sample rate, for every channel of its input. With an overlap of 4 the window is Hann; with 2, a sine window. `ifft` overlap-adds the frames it reads, windowed again, so `fft` followed directly by `ifft` gives back its input `size` samples later: 2048 samples, about 43 ms at 48 kHz, by default. A spectral chain between them adds no latency of its own.

The spectral processors read frames and write new ones, so one frame pipe can feed several. Each treats every channel, and every bin, on its own:

- `pv-delay` delays bin k of B by `time × ((1 − spread) + spread × s(k))`, rounded to whole hops, where s(k) is 1 for `:flat`, k / (B − 1) for `:rising` (high bins later), 1 − k / (B − 1) for `:falling`, and a random draw from 0 to 1 for `:random`, drawn again at each event on `:redraw`. `:feedback` adds each bin's delayed output back into its history, and `:mix` crossfades the input and the delayed spectrum. `:max-time` sizes the history, which the piece holds in memory.
- `pv-freeze` catches each bin's magnitude and frequency when `:hold` rises above 0.5 and, while it stays there, plays that spectrum with its phases advancing at the caught frequencies, which sustains the sound without a loop. It crossfades into and out of the hold over `:fade`, and otherwise passes its input.
- `pv-blur` smooths each bin's magnitude over frames by `:amount`, keeping the input's phases: 0 passes the input, and near 1 turns its changes into a slow wash. A silent input stays silent.

## Serial devices

`serial-in` reads lines of numbers from a serial device; `serial-out` writes them.

| Primitive | Parameters and ports |
|---|---|
| `serial-in` | `:device` (text), `:baud` (one of 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600; default 115200), `:value` (out, control), `:out` (out, events); at least one output |
| `serial-out` | `:device`, `:baud`, `:in` (events; required), `:value` (control) |

`:device` is a path, such as `/dev/ttyACM0`; a path that ends in `*` names the first device, in sorted order, whose path begins with the rest, such as `/dev/ttyACM*` or `/dev/cu.usbmodem*`; the empty default names the first USB serial device. Devices run at 8 data bits, no parity, 1 stop bit, and no flow control. Every node on a device shares one connection to it.

A line holds numbers separated by spaces, tabs, or commas, such as `512 0.25 1` or `512,0.25,1`, and ends with a line feed, with or without a carriage return before it. Number *i* of a line sets channel *i* of `:value`; channels the line lacks keep their values. Each line also sends a value event on `:out`, carrying its first number. A line with more than 8 numbers, a field that is not a decimal number, or more than 256 bytes is dropped and counted; an empty line is ignored. `serial-out` writes a line at each event on `:in`: the channels of `:value` when it is bound, otherwise the event's number, each with the fewest digits that read back to the same value. A control value alone writes nothing.

A device that is missing is opened when it appears; one that goes away is closed and opened again when it returns, and the nodes on it keep their values meanwhile. On Linux, a serial device opens only for members of the `dialout` group (`uucp` on some distributions).

```lisp
(let ((knob (pipe :control)) (cutoff (pipe :control)))
  (serial-in :device "/dev/ttyACM*" :value knob)
  (scale :in knob :in-low 0 :in-high 1023 :out-low 200 :out-high 4000 :out cutoff))  ; hertz
```

## Web data

| Primitive | Parameters and ports |
|---|---|
| `web-in` | `:url` (`http://` or `https://`; required), `:every` (seconds, at least 0.1; default 10 s), `:fields` (text list), `:headers` (text list), `:timeout` (seconds; default 5 s), `:value` (out, control), `:out` (out, events) |
| `web-stream` | `:url` (`http://` or `https://`; required), `:headers`, `:fields`, `:event` (text), `:value`, `:out` |
| `web-socket` | `:url` (`ws://` or `wss://`; required), `:headers`, `:fields`, `:hello` (text), `:value`, `:out` |
| `web-out` | `:url` (`http://` or `https://`; required), `:headers`, `:timeout`, `:method` (`:post`, `:put`, or `:get`), `:fields` (text list), `:in` (events; required), `:value` (control), `:dropped` (out, control) |

`web-in` requests `:url` when its plan becomes active and then every `:every` seconds of wall-clock time, counted from the start of the previous request; a beat value is an error. A request still running when the next is due skips that one. Nodes that request one URL with the same headers and interval share each request. `web-stream` holds a `text/event-stream` response open and reads each message, or only messages of `:event` when it is given. `web-socket` holds a WebSocket open, sends `:hello` after each connection opens when it is given, and reads each text message. A stream or socket that closes or fails connects again after 3 s, or after a stream's own retry time, doubling while failures repeat, up to 60 s; a stream sends the last event ID it received.

Each response or message is a JSON document of at most 1 MiB. Field *i* of `:fields`, a JSON Pointer such as `/main/temp` or `/data/0/value`, sets channel *i* of `:value`; with no fields, the whole document is read as one number. A number is read as it is, `true` and `false` as 1 and 0, and a string that holds a number as that number. A field that is absent or holds anything else leaves its channel unchanged. Each document read also sends a value event on `:out` carrying field 0, unless field 0 is absent. At most 8 fields are read; given fields must match `:value`'s channels.

`web-out` sends one request at each event on `:in`: a JSON object of `:fields` and the channels of `:value` (`{"temp": 21.5, "level": 0.25}`), or, with no `:value`, of `value` and the event's number. With `:method :get` the same pairs go in the query string. The engine does not limit how often a piece sends; a piece chooses, for example with `metro` or `threshold`. Up to 64 requests wait to be sent; one more is dropped, counted, and shown on `:dropped`. A failed request is counted and not sent again.

A header is written `"Name: value"`. Header values are part of the IR, which `espresso-client --compile` prints and the client sends to the server over the local socket; keep API keys in project files that stay out of version control.

```lisp
(let ((now (pipe :control :channels 2)))
  (web-in :url "https://api.open-meteo.com/v1/forecast?latitude=41.9&longitude=-87.6&current=temperature_2m,wind_speed_10m"
          :every 60s :fields ["/current/temperature_2m" "/current/wind_speed_10m"] :value now))
```

A request, stream, socket, or device that fails is reported in the console as `server: <what failed>`, at most once a second for each node, with how many failures occurred since the last report; the line opens the node's source. An offline render opens no device and makes no request: serial and web sources keep their initial values, and sinks send nothing.

## Pages

A page is a project's front-end in HTML and JavaScript, served by the server to a browser. It speaks OSC to the piece: a piece reads a page with `osc-in` and writes to it with `osc-out`, as it does any OSC controller. A project with `:page-port P` in `project.lisp` has pages: while one of its plans plays, the server serves the files of the project root's `ui/` directory at `http://localhost:P/`, `/` being `ui/index.html`. `:port :page` on `osc-in` and `osc-out` stands for P; in a project without a page port it is an error.

```lisp
(let ((cutoff (pipe :control)) (keys (pipe :event)) (notes (pipe :event))
      (level (pipe :control)))                          ; written by the instrument
  (osc-in :port :page :address "/cutoff" :value cutoff)
  (osc-in :port :page :out keys)
  (osc-notes :in keys :out notes)
  (osc-out :port :page :address "/level" :value level))
```

`:initial` gives an `osc-in`'s value pipe a value until the first message arrives, in any unit: `(osc-in :port :page :address "/cutoff" :initial 800hz :value cutoff)`. A page's controls send their values when it connects, so a piece whose controls start at the same values plays as written without its page and follows the page once one is open. A node that takes the place of another across a plan swap keeps the value its pipe holds.

Messages from a page reach the `osc-in` nodes on P, as datagrams sent to UDP port P do; messages that `osc-out` sends to P go to every open page instead of UDP. A message is a JSON array of an address and its arguments: `["/cutoff", 0.4]`, `["/note", 60, 100]`. From a page, a number becomes a float, a string a string, and `true` and `false` the integers 1 and 0. To a page, integers and floats become numbers, strings strings, `T` and `F` booleans, and other types `null`.

A page loads the helper with `<script src="/espresso.js"></script>`, which gives it four functions over one WebSocket:

```js
espresso.send("/cutoff", 0.4);
espresso.on("/level", (level) => { meter.value = level; });
espresso.onOpen(() => espresso.send("/cutoff", knob.value));
```

`espresso.send` drops a message while the socket is closed and counts it in `espresso.dropped`. The socket connects again after 1 s, doubling up to 30 s while failures repeat. `espresso.onOpen` calls its function each time the socket opens, and at once when it is already open, so a page can send its controls' values on every connection and the piece starts where the page is. `espresso.onClose(fn)` calls its function each time an open socket closes, as when the server stops.

| Request | Answer |
|---|---|
| `GET /`, `GET /<path>` | A file under `ui/`; a path outside it, or a symbolic link that leaves it, is 404 |
| `GET /espresso.js` | The helper |
| `GET /espresso-ui.js` | The page controls (Page controls) |
| `POST /osc` | A JSON array of messages, delivered in order; 204. A batch with a message that breaks a rule is refused whole with 400 |
| `GET /osc` | The messages sent to pages, as server-sent events: `data: ["/level", 0.25]` |
| `GET /clips/<bank>` | The titles of bank `<bank>`'s clips as a JSON object by slot, from 1: `{"1": "walking bass", "2": ""}` |
| `POST /clips/<bank>/<slot>?name=<title>` | Saves the body, a MIDI file of up to 1 MiB, as slot `<slot>` (from 1) of the bank, `clips/<bank>/<slot> <title>.mid`, in place of the slot's file before, gives it to the bank, and sends `/espresso/clips` with the bank's name to the open pages; 204. A body that is not a MIDI file `clip-player` reads is refused with 400 |

Both answer only for a bank a `clip-player` of the playing piece plays, and 404 for any other. A file of a bank that does not read is reported once, and read again each time the bank's files are read, until it does.
| `GET /osc` with `Upgrade: websocket` | A WebSocket: a text frame holds a JSON array of messages, a binary frame one encoded OSC message; with `?format=binary` the server sends binary frames |

A page whose stream or WebSocket opens receives first the last message the playing piece's `osc-out` nodes sent to each of their addresses on the page port, so it shows the values sent before it connected; the values a piece played before sent are not kept. The server answers only requests addressed to `127.0.0.1:P` or `localhost:P`, takes posts, clips, and WebSockets only from its own pages, and holds up to 16 connections. Messages over 1024 bytes, bodies over 64 KiB (1 MiB for a clip), and WebSocket messages over 64 KiB are refused, a refused body read to its end, up to 16 MiB and while its bytes keep arriving (2 s apart at most), so its sender reads the refusal; a page that reads too slowly loses its own messages, and no other page's.

`espresso-server --page-lan` also serves pages to other machines on the network, such as a phone. The server then prints a pairing link for each of the machine's addresses, `http://192.168.1.20:8000/?pair=<token>`; a browser on another machine opens it once, and the server answers that browser from then on. The token changes each time the server starts. The page port speaks plain HTTP, so anyone on the network can read its traffic; use `--page-lan` only on a network you trust.

`osc-notes` turns OSC messages into note events, so a keyboard on a page, or any OSC controller, plays an instrument through `voice-split`:

| Primitive | Parameters and ports |
|---|---|
| `osc-notes` | `:in` (events: OSC), `:address` (text; default `/note`), `:out` (events) |

A message `[address, note, velocity]`, with an optional channel from 1 to 16 as a fourth argument, becomes a note-on, or a note-off when the velocity is 0. Notes and velocities are rounded and kept within 0 to 127, and channels within 1 to 16. A message to the address with a missing or non-numeric argument is dropped and counted; messages to other addresses are ignored.

### Controls in the language

A piece declares its page's controls in the language. Each control adds the `osc-in` its page sends to and returns that node's pipe, so a control is written where its value is used; each display adds the `osc-out` that sends a pipe to the page; groups lay them out. The compiler writes the layout into the IR's page object, with every address filled in. A project without `ui/index.html` is served the page drawn from that layout at `/` and `/index.html`: the controls as Page controls and Page layout describe them, on the page grid. When an Update, or a run after a stop, changes the layout, every open page loads the new one, its controls keeping the values the player set where their addresses stay. A project's own `ui/index.html` takes the drawn page's place and is never asked to load again; an empty `ui/index.html` counts as none.

```lisp
(defpiece main ()
  (let ((pitch  (knob "Pitch" :min 55hz :max 440hz :initial 110hz :size :big))
        (rate   (knob "Rate" :min 0.01hz :max 2hz :initial 0.1hz))
        (volume (knob "Volume" :max 0db :initial -15db))
        (level  (pipe :control)))
    ...
    (page
      (panel "WAVETABLE"
        (section "Oscillators" pitch)
        (section "Sweep" rate)
        (section "Output" volume (meter "Level" level :max 0.25 :orientation :vertical))))))
```

| Function | Returns | Element |
|---|---|---|
| `knob`, `slider` | A control pipe | `es-knob`, `es-slider` |
| `toggle` | A control pipe, 1 when on and 0 when off | `es-toggle` |
| `button` | A control pipe, 1 while held | `es-button` |
| `select` | A control pipe, the index of the chosen `:options` entry | `es-select` |
| `xy` | A control pipe of two channels, x and y | `es-xy` |
| `keys` | An event pipe of notes, as `osc-notes` makes them | `es-keys` |
| `clips` | An event pipe of notes: those of the `clip-player` it makes for its bank, which plays the slot the page chooses | `es-clips` |
| `footswitch` | A control pipe, 1 while engaged (the default) and 0 while bypassed | `es-footswitch` |
| `drawbar` | A control pipe, 0 to 8 | `es-drawbar` |
| `meter`, `lamp`, `readout` | The display | `es-meter`, `es-lamp`, `es-value` |
| `page`, `panel`, `section` | The group | The page, `es-panel`, `es-section` |
| `synth-panel`, `pedalboard`, `pedal` | The group | `es-synth`, `es-pedalboard`, `es-pedal` |
| `screen` | An area of its `:span` with no address, which the project's `ui/script.js` draws in, finding it by its label | `es-screen` |
| `tab` | A tab of the page, with its label first, holding panels, synth panels, and pedalboards | `es-tab` |

Each takes its label first, a string, when it has one; a display takes the pipe it shows next, and a group the elements it holds. Keyword arguments follow: the element's attributes in Page controls and Page layout, written as keywords (`:scale :exp`, `:orientation :vertical`, `:size :big`, `:unit :percent` for `%`), with `:initial` for an element's starting value, and `:span` and `:at` as vectors of two numbers (`:span [4 4]`).

- **Ranges.** `:min`, `:max`, and `:initial` take numbers in any unit and hold them in the canonical one; those written with units are of one kind. A frequency shows in hertz on an exponential scale, a time in seconds, and a level in decibels on a decibel scale, unless `:unit` and `:scale` say otherwise. `:min` is 0 and `:max` 1 by default.
- **Starting values.** A control without `:initial` starts at `:min`. Its pipe holds the same value until the page sends one, so the piece plays as written without the page. A pad's `:initial` is one number for both axes.
- **Addresses.** A control or display takes its address from its label, lowercased, with other characters as hyphens, under the labels of the sections that hold it: `"Rate"` in section `"Sweep"` is `/sweep/rate`. A second section or element of a name takes `-2`, a third `-3`. `:address` sets an address by hand, for an outside OSC controller; a derived address never repeats it. An element without a label takes its function's name.
- **Groups.** A page holds panels, a panel sections, and a section controls and displays. A group given what it cannot hold wraps it in what it holds: `(page pitch rate)` is a page of one panel of one section. A vector of elements, such as one `map` makes, is placed as its items. Each element is placed once.
- **Loops.** `clips` makes a row of a clip bank's slots on the page and the `clip-player` that plays the bank, and returns its notes, for an instrument to play: `(play bass (clips "Bass") out)`. Its bank is its `:bank`, or its label as an address takes it (`"Bass"` is `bass`), which needs a letter or digit; `:slots` (8, a whole number from 1 to 64), `:grid` (`16beat`, in beats), and `:initial`, the slot it starts on, a whole number from 0 below `:slots` (−1, none, by default), are its `clip-player`'s. The `clip-player` sends the slot it plays and the slot it changes to under the row's address, at `/playing` and `/queued`; those addresses are the row's, and an address written by hand moves the row's address rather than repeat one. A section of rows of `clips` stacks them, one a line: a loop matrix.
- **Kits.** `synth-panel` holds sections under a model's name, with `:maker`; `pedalboard` holds pedals; `pedal` holds controls and a `footswitch`, and a `lamp`, when it holds one, is its LED. `pedal` and `synth-panel` given an instance draw its `:control` slots (Classes and generic functions), named after its class unless they give a label, in a section of no title: `(pedal (make fuzz))`, `(synth-panel (make subtractive))`. A slot's value outside its control's range widens the range to start there. A `section` given an instance draws them in itself, as a modulator's are: `(section "LFO" wobble)`. A pedal's label, like a section's, prefixes its controls' addresses: `/fuzz/drive`. Pedals given to a page share one pedalboard.
- **Tabs.** A page given `tab`s shows one tab at a time, chosen from a strip along its top: `(page (tab "Bass" bass-panel bass-chain) (tab "Master" master))`. A tab is a view; controls and their addresses do not change with it. A page with tabs holds only tabs, and every group is in one. Each tab has a label, unlike every other tab's. Each tab shows the tabs its audio comes from and goes to: the compiler records the pipes each drawn instrument plays into and each drawn effect reads and writes, and a tab that reads a pipe another writes comes from it; a tab that writes a pipe an `audio-out` reads goes to `output`.
- **Look.** `page` takes `:theme`, `:carbon` (dark, the default) or `:paper` (light), and `:accent`, a color written `"#4fd1c5"` or as a CSS color's name such as `"teal"`: `(page synth :theme :paper :accent "teal")`.
- **The page.** A piece calls `page` once. Without it, a piece that makes controls has a page that holds them, and the groups they are in, in the order they were made. Controls the page places nowhere join its last panel as a last section, and groups it places nowhere, such as a `chain`'s pedalboard, join it at the end; so do controls when its last group is a pedalboard.
- **Order.** `pedal`, `synth-panel`, and `section` given an instance draw its controls before its nodes are built: before `play` or `process` builds it, before `chain` processes it, and before a modulator's signal is first used. Drawing an instance after, or twice, is an error, as its nodes would not read the controls. A slot given a beat value keeps it and has no control, as a control's range in seconds cannot follow the tempo.
- **Sizes.** The compiler lays the page out on its grid: each control in a section takes a place, `:at`, and a size, `:span`, in units, packed in the order written as Page layout describes; each section and pedal takes the size its contents need. `:at`, `:span`, and `:size` are each two whole numbers from 1 to 256. `:size [w h]` on a `section`, `pedal`, `panel`, `synth-panel`, or `pedalboard` sets its size by hand; a size smaller than its contents need is an error that names the size they need. A pedal places its controls itself, so they take no `:at`: it holds them in centred rows a unit apart, two side by side (three when it holds more than four), a control of another height starting a new row, at the width that gives it the shape nearest a pedal's enclosure, then its name, LED, and footswitch; its connections stand at one height, so pedals of every size on a board connect level.
- **Reserved.** `/espresso/reload` and `/espresso/clips` are the server's: on the first it tells open pages to load the page again, and on the second that a page saved a clip to the bank it names. No derived address takes either, and an `osc-in` or `osc-out` on the page port at either is an error.

A drawn page is restyled and extended by the project's own files, in order of how far they go:

1. `:theme` and `:accent` on `page`.
2. `ui/style.css`, which the drawn page loads after its own style: the `--es-*` properties and `::part()` rules of Page controls restyle every control on it.
3. `ui/script.js`, which it loads after its own script, for what the language does not draw. Each group and control on a drawn page carries its `label`, so a script finds its place by it: `document.querySelector('es-section[label="Sweep"]').prepend(screen)` sets a screen of its own, with a `span` in units, above the section's controls, and setting a control's `unit` attribute to a unit the script adds to `espressoUI.units` shows the control's value in it.
4. `espresso-client --page FILE > ui/index.html`, or Play → Save Page as ui/index.html in the GUI editor, writes the drawn page into the project, without the drawn page's reload. `ui/index.html` is then the project's own page, served in place of the drawn one and edited by hand; the piece's layout no longer draws it. A compile that fails writes nothing, and the empty file a shell's redirect then leaves counts as no page.

## Page controls

`/espresso-ui.js` is a library of page controls: custom elements bound to the piece's OSC addresses. A page loads it after the helper and places controls as HTML elements:

```html
<script src="/espresso.js"></script>
<script src="/espresso-ui.js"></script>
<es-knob address="/cutoff" label="Cutoff" min="20" max="20000" scale="exp" value="800" unit="hz"></es-knob>
<es-meter address="/level" label="Level" max="0.25"></es-meter>
<es-keys low="48" high="71"></es-keys>
```

A control sends to its `address`, which shows as its tooltip unless it has a `title`. Value controls (`es-knob`, `es-slider`, `es-toggle`, `es-select`, `es-xy`) send `[address, value]` when the user changes them, show the numbers the piece sends to the same address without sending them back, and send their value again each time the socket opens, so the piece starts where the page is. `es-button` and `es-keys` send only while they are played. Displays (`es-meter`, `es-lamp`, `es-value`) show what the piece sends and send nothing.

Every control fires an `input` event as its value changes, with `event.detail.value` and `event.detail.fromPage`, which is true for a change the user made; `es-keys` gives `event.detail.note` and `event.detail.velocity` instead of a value. A control's `value` property reads its value, `[x, y]` for `es-xy`; on value controls and displays, setting it shows a value without sending it. `reset()` returns a value control to its `value` attribute and sends that, as a double click does.

| Element | Sends | Played by |
|---|---|---|
| `es-knob` | Its value | A vertical drag, 150 pixels for the whole travel, 1500 with Shift; the arrow keys (1% a press, or a step), Page Up and Page Down (10%), Home and End; a double click to its `value` |
| `es-slider` | Its value | A press or a drag, which puts it under the pointer; keys and a double click as `es-knob`. `orientation="vertical"` stands it up |
| `es-toggle` | 1 when on, 0 when off | A click, Space, or Enter. `value="1"` starts it on; `orientation="vertical"` stands it up, on at the top |
| `es-select` | The index of the chosen `<option>` | A list built from its `<option>` children; `value`, or an option's `selected` attribute, chooses the first |
| `es-button` | 1 when pressed, 0 when let go | A pointer, Space, or Enter held down. Its content, or its `label`, is its text |
| `es-keys` | `[address, note, velocity]` on a press and `[address, note, 0]` on the release | Each pointer holds one key and slides from key to key; a note held by two pointers sounds once, from the first press to the last release. While it has focus, the computer keyboard plays it by key position, A to ; from the lowest C, and Z and X move that an octave down and up |
| `es-clips` | Its slot, from 0, or −1 to stop | A row of its `bank`'s `slots`: its `label`, a cell a slot, and a stop button. A press on a cell that holds a clip starts it on the piece's next grid line, a press on the playing cell or the stop button stops it there, and a press on a cell waiting to start takes it back. A press on an empty cell, a cell's Load button, or L on a cell opens a MIDI file, which the page saves as the slot's (`POST /clips/<bank>/<slot>`). It shows the slot the piece reports at `<address>/playing`, which pulses, and the slot at `<address>/queued`, which flashes until the grid line and reads Next; a playing cell whose stop is queued flashes and reads Stops. A press made while the page is not connected is not sent and says so. A cell's tooltip is its clip's title; a cell loading a file dims until the server answers. The arrow keys move between cells, Up and Down between the rows of a section |
| `es-xy` | `[address, x, y]` | A press or a drag on a square, which puts the point under the pointer, y rising upward; the arrow keys; a double click to `value="x,y"` |
| `es-meter` | Nothing | Shows the value it receives as a bar, with a line at the highest value of the last `hold` milliseconds (1000; 0 follows the value). `orientation="vertical"` stands it up |
| `es-lamp` | Nothing | A light, off at the bottom of its range and full at the top |
| `es-value` | Nothing | The value it receives as text, in its `unit` or as one of its `options`; empty until a value arrives |
| `es-footswitch` | 1 while it engages its effect, 0 while it bypasses it | A pedal's stomp switch: a click, Space, or Enter. It starts at 1 unless `value="0"`. On a pedal whose LED is a lamp, a ring of light (`--es-footswitch-on`) shows it engaged |
| `es-drawbar` | Its value, 0 to 8 by default in steps of 1 | An organ's drawbar, pulled down to raise it: a press or a drag puts it under the pointer; keys and a double click as `es-knob` |

`es-keys` sends to `/note` by default, the address `osc-notes` reads, with a `velocity` of 100 over the keys `low` to `high` (48 to 71, C3 to B4). It has no `label` text of its own.

A value maps to a control's travel through these attributes:

| Attribute | Meaning |
|---|---|
| `min`, `max` | The ends of the range; 0 and 1 by default |
| `scale` | `linear` (the default); `exp`, equal ratios over the travel, for frequencies and times, with `min` and `max` above 0; `db`, equal decibels over the travel, for amplitudes, from -60 dB when `min` is 0; or a scale the page adds |
| `step` | Rounds the value to a multiple of `step` from `min`; the arrow keys move a linear stepped range one step |
| `steps` | A selector of that many positions, 0 to `steps` − 1 |
| `value` | The starting value, and the value a reset returns to |
| `unit` | How the value reads: `hz` (Hz, kHz), `s` (ms below 1 s), `db` (an amplitude in decibels), `%` (0 to 1 as a percent), or a unit the page adds; without one, a number of 3 significant digits |
| `options` | Names for the positions of a selector or a stepped range, separated by commas, the first for its lowest value |
| `label` | The text shown with the control, and its name to assistive technology; the address when it is absent |

`es-knob` also takes `sweep`, its travel either side of the top in degrees (135), and `angles`, a list of degrees that makes a selector stopping at each angle and sending the stop's index. `mod="low,high"` draws an arc around the track over the span a modulation reaches, from `low` to `high` in its travel from the knob's position, which the layout writes for a knob that `modulate` or `modulated` moves (`docs/library.md`, std/modulators); `--es-mod` colors it, the text color by default. Every control the user plays is reachable from the keyboard, and every control has the role and value its HTML counterpart has.

A page adds scales and units by name. A scale maps a position from 0 to 1 to a value and back; a unit writes a value as text:

```js
espressoUI.scales.square = {
  value: (p, min, max) => min + p * p * (max - min),
  position: (v, min, max) => Math.sqrt((v - min) / (max - min)),
};
espressoUI.units.cents = (v) => Math.round(v * 100) + " cents";
```

Controls take their first value once the document is parsed, so a page's script adds scales and units before or after the controls that use them.

The look comes from CSS custom properties, which a page sets on any element to restyle the controls within it:

| Property | Default | Meaning |
|---|---|---|
| `--es-accent` | `#e8a33d` | A knob's arc, a slider's and a meter's fill, a toggle that is on, a pressed button or key, a pad's point, a lamp's light |
| `--es-surface` | `#262626` | A knob's body, a lamp that is off, the faces of buttons, lists, and pads |
| `--es-line` | `#4a4a4a` | Tracks and outlines |
| `--es-text` | `#ece9e2` | Text, pointers, thumbs, and a meter's peak line |
| `--es-muted` | `#8f8b83` | Labels, and a toggle's thumb while off |
| `--es-focus` | `#6fa8dc` | The focus ring |
| `--es-font` | `12px/1.2 system-ui, sans-serif` | The type |
| `--es-knob-size` | `56px` | A knob's width and height |
| `--es-slider-length`, `--es-slider-thickness` | `160px`, `8px` | A slider's track |
| `--es-meter-length`, `--es-meter-thickness` | `160px`, `8px` | A meter's bar |
| `--es-toggle-length`, `--es-toggle-thickness`, `--es-toggle-inset` | `36px`, `18px`, `2px` | A toggle's track, and the gap around its thumb |
| `--es-lamp-size` | `14px` | A lamp's diameter |
| `--es-keys-height` | `120px` | A keyboard's height |
| `--es-key-white`, `--es-key-black` | `#ece9e2`, `#161616` | A keyboard's keys |
| `--es-xy-size` | `160px` | A pad's width and height |

A page styles the parts of a control with `::part()`, as `es-knob::part(pointer) { stroke-width: 6px; }`:

| Element | Parts |
|---|---|
| `es-knob` | `label`, `knob`, `focus`, `track`, `fill`, `mod`, `body`, `pointer`, `value` |
| `es-slider` | `label`, `track`, `fill`, `thumb`, `value` |
| `es-toggle` | `label`, `track`, `thumb`; both also `on` while it is on |
| `es-select` | `label`, `select` |
| `es-button` | `button`; also `down` while pressed |
| `es-keys` | `keys`, and `key` with `white` or `black`; also `down` while held |
| `es-clips` | `label`, `cells`, `cell`, `status` |
| `es-xy` | `label`, `pad`, `line`, `point`, `value` |
| `es-meter` | `label`, `track`, `fill`, `peak` |
| `es-lamp` | `label`, `light` |
| `es-value` | `label`, `value` |

Three kits draw the panels of instruments and effects around controls and sections:

| Element | Draws |
|---|---|
| `es-synth` | A synthesizer's panel between wooden cheeks: its `label` as the model's name and its `maker` under a rule, its sections side by side. A section that holds an `es-keys` is its keybed, under the other sections and as wide as the panel |
| `es-pedal` | A pedal of its `size` in units: its controls in centred rows by their `at` rows, each of its `span`, then its `label` as its name, an LED, and its `es-footswitch`; its jacks two units from the top. The LED is its `es-lamp` when it holds one, and otherwise lit while the footswitch engages it. Without a `size`, it is 10 × 18 units, its knobs two to a row, and 14 wide for more than four |
| `es-pedalboard` | Pedals in a row, in the order of their chain, each in a paint of its own |

`es-status` shows the connection's state as text: Connecting…, then Connected, and Disconnected · reconnecting… while the socket is closed after being open.

| Property | Default | Meaning |
|---|---|---|
| `--es-cheek`, `--es-face` | Walnut, dark grey | A synth panel's cheeks and face |
| `--es-paint`, `--es-ink` | Set by the pedalboard; `#3f8fb8` and `#0d2a38` alone | A pedal's enclosure and its text |
| `--es-footswitch-size` | `64px` | A footswitch's diameter |
| `--es-drawbar-length` | `160px`; 88 px in a section | A drawbar's track |
| `--es-drawbar-tip`, `--es-drawbar-rod` | `#f2efe6`, `#8a8780` | A drawbar's tip, and the part of its rod pulled out |

`espressoUI.valueAt(p, range)`, `espressoUI.positionOf(v, range)`, `espressoUI.fit(v, range)`, and `espressoUI.format(v, unit)` give a page the controls' own mapping, for a range `{ min, max, scale, step }`.

### Page layout

A page lays out its controls on a grid of one unit, `--es-u`, 24 pixels by default. `es-panel` holds sections side by side, and `es-section` is a grid of units in which each control fills its footprint:

```html
<es-panel>
  <header>WAVETABLE</header>
  <es-section label="Oscillators">
    <es-knob size="big" address="/pitch" label="Pitch" min="55" max="440" scale="exp" value="110" unit="hz"></es-knob>
  </es-section>
  <es-section label="Sweep">
    <es-knob address="/sweep/rate" label="Rate"></es-knob>
    <es-knob address="/sweep/depth" label="Depth"></es-knob>
  </es-section>
</es-panel>
```

| Footprint | Units (w × h) |
|---|---|
| `es-knob` | 3 × 4, a 56 px dial |
| `es-knob size="big"` | 4 × 5, an 80 px dial |
| `es-toggle`, `es-button` | 3 × 3 |
| `es-lamp`, `es-value` | 3 × 2 |
| `es-select` | 4 × 2 |
| `es-slider`, `es-meter` | 6 × 2; 2 × 6 with `orientation="vertical"` |
| `es-xy` | 8 × 8 |
| `es-keys` | One unit per white key × 5 |
| `es-footswitch` | 3 × 3 |
| `es-drawbar` | 2 × 6 |

A control sits centred in its footprint; any other child fills one unit unless its `span` says otherwise.

| Attribute | On | Meaning |
|---|---|---|
| `label` | `es-section`, `es-panel` | A section's title, one unit high above its grid, the section at least as wide as it in whole units; the group's name to assistive technology |
| `columns` | `es-section` | Rows of that many units, filled in turn. Without it, children flow in one row, and a child that fits under the one before, within the section's height, stacks under it |
| `span` | A child of `es-section` | Its width and height in units, as `span="9 3"` |
| `at` | A child of `es-section` | Its column and row, from 1, as `at="4 1"` |
| `flow` | `es-panel` | `row` (the default), sections side by side; `column`, sections stacked |
| `size` | `es-section`, `es-pedal`, `es-panel`, `es-synth`, `es-pedalboard` | Its size in units, as `size="9 4"`: a section's columns, a pedal's width and height, the least width and height of the others |

An `es-panel` is padded one unit and keeps its sections one unit apart; a child other than a section, such as a header, spans the sections' width. Neither element has a shadow root, so a page styles both, and their children, with its own rules. A section's title takes `--es-heading` for its color and `--es-heading-font` for its type.

A drawn page stands in an `es-canvas`, which fills the window and pans and zooms the page it holds, so a page larger than the window is reached by moving it: the wheel or a trackpad pans; Ctrl or ⌘ and the wheel, or a pinch, zoom about the pointer, from 25% to 400%; a drag on the background (not on a control), Space and a drag, or a drag with the middle button pans, even from a control, which then takes no press; a press on the background takes the focus from the control that had it. While no control has focus, the arrow keys pan (further with Shift), − and = zoom, Shift 1 fits the page to the window, and Shift 0 returns to 100%. A bar in the window's corner holds Zoom out, the zoom (a double click returns to 100%), Zoom in, and Fit. The browser keeps each page's view across reloads, by the page's title and the canvas's `name`, so pages of other projects served on the same port keep views of their own; a page opens at 100%, centred where it fits the window and from its top left where it does not. A page of its own uses the canvas by wrapping its content in `<es-canvas>`.

A page with tabs is an `es-tabs` of `es-tab`s, each with its `label` and its own `es-canvas`. `es-tabs` shows one tab at a time and draws a strip of tabs along the window's top: each tab's button names it and, after an arrow, the `to` it goes to; for the open tab, the strip's end reads From and its `from`, and To and its `to`. A click on a button, or the arrow keys, Home, and End on the strip, open a tab; the browser keeps each page's open tab across reloads, by its label, for that set of tab labels. A tab's canvas keeps a view of its own.

The examples `mini-clone` and `rack` have pages of their own, `ui/index.html`, built from these controls: each draws its panel in SVG and places the controls over the drawing. The other examples' pages are drawn from their layouts.

## Mappers

A mapper transforms a pipe with an expression:

```lisp
(mapper knob (fn (x) (scale-exp x 0 1 300hz 8khz)))
```

The result is used wherever a pipe is read. The compiler turns each operation into a generator node; the function may use the operations `+ - * / neg abs exp log sin cos tanh floor round pow min max quantize clamp scale scale-exp`. The output is an audio pipe when an input is audio, otherwise a control pipe. Outside a mapper, these operations take numbers only.

Constants follow the dimension rules of each operation, inside and outside a mapper. `min`, `max`, `clamp`, `quantize`, `scale`, and `scale-exp` take arguments of one dimension; `+` and `-` do not take levels; `neg` and `abs` do not take levels; `exp`, `log`, `sin`, `cos`, `tanh`, and the exponent of `pow` take plain numbers. Beat values are not accepted. Division by a constant zero is an error.

`scale` onto a range of levels interpolates in decibels and converts the result with a `db-to-amp` node: `(scale x 0 1 -40db 0db)` gives -20 dB for an `x` of 0.5. A level of 0 has no decibel value, so such a range needs a floor, such as `-60db`. `scale-exp` onto levels interpolates the amplitudes geometrically, which is the same curve.

## Classes and generic functions

```lisp
(defclass voice ()
  ((freq  :initarg :freq  :initform 220hz :accessor freq)
   (level :initarg :level :initform -6db)))

(defclass bass (voice)
  ((freq :initform 55hz)))

(defgeneric build (voice out))

(defmethod build ((v voice) out)
  (sine-osc :freq (freq v) :out out))

(defmethod build :before ((v bass) out)
  (set! (freq v) (* (freq v) 2)))
```

| Form | Meaning |
|---|---|
| `(defclass name (superclasses...) ((slot options...)...))` | Defines a class; a slot is a name or a name with options |
| `(make class :initarg value...)` | A new instance |
| `(slot-value object 'slot)` | Reads a slot |
| `(defgeneric name (params...))` | Declares a generic function dispatching on its required parameters |
| `(defmethod name [qualifier] (params...) body...)` | Adds a method; a parameter is `name` or `(name class)` |
| `(call-next-method [args...])` | In a method, calls the next method, with this method's arguments when none are given |
| `(next-method-p)` | In a method, true when there is a next method |

Slot options are `:initarg :keyword`, `:initform expression`, and `:accessor name`. An accessor is a generic function of one argument that reads the slot. `make` sets each slot from its initarg, or else by evaluating its initform, or else to `nil`; initforms are evaluated at each `make`. A class inherits the slots of its superclasses: the initform of the most specific class that gives one applies, and every class's initargs set the slot. `:control call` names the page control a pedal or a synth panel draws the slot as (Pages): a call of `knob`, `slider`, `toggle`, or another control, such as `(knob :min 500hz :max 8khz)`, evaluated where its `defclass` is written when the pedal is drawn. It takes the slot's name as its label unless it gives one and the slot's value as its `:initial` unless it gives one, and the slot then holds the control's pipe; a slot that holds a pipe is not drawn. The most specific class that gives a `:control` decides it, as for initforms.

Classes and generic functions are global. A class must be defined before its subclasses and before the methods that name it. Redefining a class affects the instances made afterwards.

The class precedence list orders a class before its superclasses by C3 linearization. A hierarchy with no consistent order is an error.

A method applies when each of its specialized arguments is an instance of its class or of a subclass. An unspecialized parameter, or one specialized on `t`, accepts any value. Applicable methods are ordered by their first argument's class precedence, then the second's, and so on. A call runs the `:around` methods, most specific first, each through `call-next-method`; then the `:before` methods, most specific first; the primary methods, most specific first, each through `call-next-method`; and the `:after` methods, least specific first. The call returns the value of the outermost `:around` method, or else of the most specific primary method. A call with no applicable primary method is an error. `defmethod` without a prior `defgeneric` creates the generic function. A method with the same qualifier and specializers as an existing one replaces it.

A call to a generic function is one step of the instance path, named for the generic function, whichever methods run.

## Node IDs

Each node's ID is its file, the entry piece, and its instance path: the named function calls from the piece to the primitive call, each written `callee@line:column#n`. `n` counts calls from the same place under the same parent, so the eight voices built by one `map` are numbered 0 to 7. Calls to anonymous functions add no step.

The reserved argument `:key` replaces the count: `(voice f out :key 'bass)`. Keys keep each node's ID, and so its state across swaps, when the items of a `map` are reordered. A keyed call to an anonymous function adds a step named `fn`. Two calls to the same function under the same parent with the same key are an error. A key is a symbol, string, keyword, or integer. `:key` on a builtin other than a primitive, or on a call made outside a piece build, is an error.

A keyword argument given twice in one call, including `:key` and the options of `pipe`, is an error at its second occurrence.

## Errors

Errors stop the compilation and leave the running piece unchanged. Each names the file, line, and column of the value or innermost form at fault, and the calls that led to it. Parameter values are checked at the call against their dimension, range (in the unit written), and the pipe types they accept; ports are checked for their pipe types and required bindings.

```text
main.lisp:2:19: parameter :freq expects a frequency, got a time
```

Evaluation nests at most 2000 levels and runs at most 5,000,000 steps. A program that nests deeper or runs longer fails with an error.

## Warnings

A piece that compiles may still carry warnings. They are reported with their location, in the editor console and on standard error, and do not stop the piece:

| Warning | When |
|---|---|
| A loop grows without bound | The gains along a loop through one or more audio feedback pipes multiply to 1 or more; parallel paths add. A loop may pass through inputs or through parameters bound to audio pipes. `gain` amounts, `adsr` gains, and `mix` levels (per input) count; adders, delays, pans, and the `b` operand of `subtract`, `min`, and `max` count as 1. With a node of unknown gain on the loop, such as a filter or a gain bound to a pipe, the warning appears only above 1, and says "may" |
| A gain is very large | A constant `gain`, `adsr` gain, or `mix` level on an audio path exceeds +24 dB |
| A frequency aliases | A constant frequency parameter exceeds half the sample rate |
| A buffer has two writers | Two nodes write one buffer, each recording over the other |

## Not supported

Beat values in mappers, `play` in source files, text, and mouse and keyboard input to sketches.
