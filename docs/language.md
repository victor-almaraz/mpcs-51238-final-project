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
| `(unit-value q 'unit)` | Quantity `q` as a plain number in `unit`: `(unit-value 250ms 's)` is 0.25 |
| `(tempo 96bpm)` | Sets the tempo when the piece starts |
| `neg abs exp log sin cos tanh floor round` | One argument |
| `pow min max quantize` | Two arguments: `(quantize x step)` |
| `clamp` | `(clamp x low high)` |
| `scale`, `scale-exp` | `(scale x in-low in-high out-low out-high)`: a linear or exponential map |
| `(rgb r g b)`, `(rgb r g b a)` | A color: red, green, blue, and alpha (default 1), each a plain number from 0 to 1 |
| `(hsb h s b)`, `(hsb h s b a)` | A color from hue, saturation, and brightness, each 0 to 1, converted to RGB |

An index or a count, such as the `i` of `nth`, the `n` of `range`, and the `:channels` of `pipe`, is an integer or a real with an integer value. `/`, `floor`, and `round` return reals, so `(nth items (floor (/ k 2)))` is element `k / 2` rounded down. A real with a fractional part, such as `1.5`, is an error.

## Pipes and primitives

```lisp
(pipe :audio)                ; one channel
(pipe :audio :channels 2)
(pipe :control)
(pipe :event)
(pipe :audio :feedback #t)   ; delays its data by one block
(pipe :video)                ; one frame
```

Every primitive is called by its name in kebab-case (`sine-osc`, `delay-line`, `scale-exp`) with keyword arguments: its parameters and its ports. A parameter takes a constant or a pipe; a port takes a pipe or a vector of pipes.

```lisp
(svf :in osc :cutoff cutoff-pipe :q 0.7 :mode :low :out filtered)
(add :in [a b] :out sum)
```

Choice parameters take keywords (`:mode :low`); text parameters take strings (`:device "keyboard"`); list parameters take vectors (`:levels [1 0.5]`). `docs/ir.md` lists the primitive types; the tables in `src/metadata/primitiveSpec.cpp` list their parameters, ranges, defaults, and ports.

Color parameters take a color (`:fill (rgb 1 0.5 0)`), `:none` where nothing should be drawn (`:stroke :none`), or a control pipe of four channels, such as one written by `lerp-color`.

A `sequencer` moves to its next value at each event on `:in` other than a note-off, writes the current value to the control pipe on `:out`, and emits it as a value event on `:events`. `:order` is `:forward` (the default), `:backward`, `:ping-pong` (to the last value and back, without repeating either end), `:random` (drawn from a generator seeded from the project seed and the node ID), or `:once`. `:forward` and `:backward` wrap at the ends. `:once` stops on the last value: later events leave `:out` at the last value and emit nothing on `:events`, so the sequence plays one time. A node that adopts a sequencer's state across a plan swap keeps its position, including the end of a `:once` sequence.

Pipes and primitives can be created only while a piece is built. The compiler orders the nodes by data flow, so calls may appear in any order. A loop, including a node that reads a pipe it writes, needs a feedback pipe.

## Images

Image generators draw frames for the visual layer, the server's window. Each reads a video pipe on `:in` (a transparent frame when none is given), draws over it, and writes the result to the video pipe on `:out`, so a chain of calls builds one picture; `video-out` shows its input.

```lisp
(let ((sky (pipe :video)) (drawn (pipe :video)))
  (background :color (rgb 0.05 0.05 0.1) :out sky)
  (circle :in sky :x 640 :y 360 :diameter 80 :fill (rgb 1 0.5 0) :stroke :none :out drawn)
  (video-out :in drawn))
```

| Generator | Parameters |
|---|---|
| `background` | `:color` |
| `point` | `:x :y :stroke :stroke-weight :stroke-cap` |
| `line` | `:x1 :y1 :x2 :y2 :stroke :stroke-weight :stroke-cap` |
| `rect` | `:x :y :width :height :mode :fill :stroke :stroke-weight`; `:mode` is `:corner`, `:corners`, `:center`, or `:radius` |
| `ellipse` | As `rect`; `:mode` is `:center` (default), `:radius`, `:corner`, or `:corners` |
| `circle` | `:x :y :diameter :fill :stroke :stroke-weight` |
| `triangle`, `quad` | `:x1 :y1` to `:x3 :y3` or `:x4 :y4`, `:fill :stroke :stroke-weight` |
| `arc` | `:x :y :width :height :start :stop :mode :fill :stroke :stroke-weight`; angles in radians, clockwise from 3 o'clock; `:mode` is `:open`, `:chord`, or `:pie` |
| `bezier` | `:x1 :y1 :cx1 :cy1 :cx2 :cy2 :x2 :y2 :stroke :stroke-weight` |
| `transform` | `:translate-x :translate-y :rotate :scale-x :scale-y`: applies to the drawing after it, until the next `transform` |
| `lerp-color` | `:color-a :color-b :amount`: writes the color between them to a 4-channel control pipe on `:out` |
| `image-player` | `:image :x :y :width :height`: an image file, relative to the project root; `:width` and `:height` 0 keep its size |

Coordinates are pixels of the frame, with the origin at the top left. The defaults follow Processing: a light gray background, a white fill, and a black stroke one pixel wide with round caps (`:stroke-cap` is `:round`, `:square`, or `:project`). Any coordinate, size, angle, or weight can follow a control pipe or an audio pipe, and a color can follow a 4-channel control pipe, so the picture moves with the sound.

## Buffers

A buffer is audio memory that a piece declares and its primitives share. `buffer` makes one, while a piece is built, with a channel count from 1 to 8 (default 1) and a length, a file, or both:

```lisp
(buffer :channels 2 :seconds 20s)                        ; 20 s of silence
(buffer :channels 2 :file "samples/rain.wav")            ; the file's length
(buffer :channels 2 :seconds 20s :file "samples/rain.wav")  ; the file, cut or padded to 20 s
(buffer :file "samples/tables.wav" :resample #f)         ; the file's samples, not resampled
```

`:seconds` takes a time or a plain number of seconds; a beat value is an error, as a buffer's length is fixed when it is made. A file path is relative to the project root. WAV, AIFF, FLAC, MP3, and Ogg Vorbis files load, resampled to the server's sample rate. `:resample #f` keeps a file's samples as they are, at its own rate, as a wavetable file needs so that its tables keep their length; its `:seconds` count at that rate. A file's channels fill the buffer's in order; a mono file fills every channel, and a mono buffer takes the average of a file's channels. A file that is missing or cannot be decoded refuses the plan, and the console points at the `buffer` form.

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

Slot options are `:initarg :keyword`, `:initform expression`, and `:accessor name`. An accessor is a generic function of one argument that reads the slot. `make` sets each slot from its initarg, or else by evaluating its initform, or else to `nil`; initforms are evaluated at each `make`. A class inherits the slots of its superclasses: the initform of the most specific class that gives one applies, and every class's initargs set the slot.

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
