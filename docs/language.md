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

Choice parameters take keywords (`:mode :low`); text parameters take strings (`:device "keyboard"`); list parameters take vectors (`:levels [1 0.5]`). `docs/ir.md` lists the primitive types; the tables in `src/metadata/primitiveSpec.cpp` list their parameters, ranges, defaults, and ports.

Pipes and primitives can be created only while a piece is built. The compiler orders the nodes by data flow, so calls may appear in any order. A loop, including a node that reads a pipe it writes, needs a feedback pipe.

## Mappers

A mapper transforms a pipe with an expression:

```lisp
(mapper knob (fn (x) (scale-exp x 0 1 300hz 8khz)))
```

The result is used wherever a pipe is read. The compiler turns each operation into a generator node; the function may use the operations `+ - * / neg abs exp log sin cos tanh floor round pow min max quantize clamp scale scale-exp`. The output is an audio pipe when an input is audio, otherwise a control pipe. Outside a mapper, these operations take numbers only.

Constants follow the dimension rules of each operation, inside and outside a mapper. `min`, `max`, `clamp`, `quantize`, `scale`, and `scale-exp` take arguments of one dimension; `+` and `-` do not take levels; `neg` and `abs` do not take levels; `exp`, `log`, `sin`, `cos`, `tanh`, and the exponent of `pow` take plain numbers. Beat values are not accepted. Division by a constant zero is an error.

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

`import` and multi-file projects, `defclass`, `defgeneric`, `defmethod`, and `make`, beat values for time parameters, `play` in source files, and video pipes.
