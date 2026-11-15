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
| `(module name)` | Names the file's module. Has no other effect |
| `(defpiece name (params...) body...)` | Defines a piece: a function that builds the graph |

The editor plays `main.lisp`; without one, it plays the file being edited. The piece named `main` is built unless another entry is given (`proj-client --compile FILE --entry NAME`). A project is one file; `import` is not supported.

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

A parameter accepts a plain number in its canonical unit or a quantity of its dimension. A quantity of another dimension is an error. Beat values for time parameters are not supported.

## Builtins

| Builtin | Meaning |
|---|---|
| `+ - * /` | Arithmetic with dimensions; on pipes inside a mapper, generators |
| `= < > <= >=` | Comparisons of numbers of the same dimension; `=` also compares symbols, keywords, and strings |
| `not` | Logical negation |
| `list` | A vector of its arguments |
| `(map f vector)` | `f` applied to each element, in order; a vector of the results |
| `(range n)` | `[0 1 ... n-1]` |
| `(tempo 96bpm)` | Sets the tempo when the piece starts |
| `neg abs exp log sin cos tanh floor round` | One argument |
| `pow min max quantize` | Two arguments: `(quantize x step)` |
| `clamp` | `(clamp x low high)` |
| `scale`, `scale-exp` | `(scale x in-low in-high out-low out-high)`: a linear or exponential map |
| `(rgb r g b)`, `(rgb r g b a)` | A color: red, green, blue, and alpha (default 1), each a plain number from 0 to 1 |
| `(hsb h s b)`, `(hsb h s b a)` | A color from hue, saturation, and brightness, each 0 to 1, converted to RGB |

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

## Mappers

A mapper transforms a pipe with an expression:

```lisp
(mapper knob (fn (x) (scale-exp x 0 1 300hz 8khz)))
```

The result is used wherever a pipe is read. The compiler turns each operation into a generator node; the function may use the operations `+ - * / neg abs exp log sin cos tanh floor round pow min max quantize clamp scale scale-exp`. The output is an audio pipe when an input is audio, otherwise a control pipe. Outside a mapper, these operations take numbers only.

Constants follow the dimension rules of each operation, inside and outside a mapper. `min`, `max`, `clamp`, `quantize`, `scale`, and `scale-exp` take arguments of one dimension; `+` and `-` do not take levels; `neg` and `abs` do not take levels; `exp`, `log`, `sin`, `cos`, `tanh`, and the exponent of `pow` take plain numbers. Beat values are not accepted. Division by a constant zero is an error.

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

## Not supported

`import` and multi-file projects, beat values for time parameters, `play` in source files, text, and mouse and keyboard input to sketches.
