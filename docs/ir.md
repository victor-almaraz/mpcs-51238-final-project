# IR format

The intermediate representation (IR) describes a graph of primitives: the pipes it declares and its nodes in execution order. The interpreter emits IR; the server validates it and builds a plan from it. `schema/ir.schema.json` defines the structure. The primitive tables in `src/metadata/primitiveSpec.cpp` define the types, parameters, and ports. The tables carry the metadata version, 0.9.0 (`include/espresso/metadata/version.h`), which the server prints at startup and with `--version`.

All values are canonical units: seconds, hertz, linear amplitude, semitones, and beats.

## Document

| Field | Type | Required | Meaning |
|---|---|---|---|
| `format` | `"espresso-ir"` | Yes | Format name |
| `version` | `1` | Yes | Format version |
| `seed` | Integer ≥ 0 | No (0) | Project seed for random generators |
| `tempo` | Number > 0 | No | Tempo set when the plan becomes active, in beats per minute |
| `meter` | Integer ≥ 1 | No (4) | Beats per bar, for bar quantization |
| `page` | `{"port": P, "layout": L}`, P from 1024 to 65535 | No | The project's page port: while the plan is active, the server serves the project's pages on TCP port P ([Pages](language.md#pages)). `layout`, optional, is the page's layout (Page layout) |
| `pipes` | Array of pipe declarations | Yes | Pipes the graph uses |
| `buffers` | Array of buffer declarations | No | Buffers the graph uses |
| `nodes` | Array of nodes | Yes | Nodes in execution order |

## Pipes

A pipe reference is an object with one key, the pipe type, whose value is the pipe index:

```json
{"audio": 3}
```

| Type | Data | Multiple writers |
|---|---|---|
| `audio` | Blocks of samples per channel | Sum |
| `control` | One value per channel per block | The last writer in execution order |
| `event` | Timestamped events: MIDI messages, triggers, values, OSC messages | Merge, sorted by sample offset |

A pipe declaration names a pipe and its shape:

| Field | Type | Default | Meaning |
|---|---|---|---|
| `pipe` | Pipe reference | | The pipe |
| `channels` | 1 to 8 | 1 | Channel count. Event pipes have one channel |
| `feedback` | Boolean | `false` | Delays the pipe's data by one block. Audio and control pipes only |

Control pipe 0 is the tempo pipe, in beats per minute. It exists without a declaration, and any node may read or write it. A declaration of it has one channel and is not a feedback pipe.

Index ranges per type are set by the server's pipe table: 1024 audio pipes, 4096 control pipes, and 128 event pipes.

## Buffers

A buffer declaration names a buffer: audio memory of a fixed number of channels and frames, which nodes read and write through their `buffer` parameters.

```json
{"buffer": 0, "id": "main.lisp/main/buffer@4:15#0", "channels": 2, "seconds": 20, "file": "samples/rain.wav"}
```

| Field | Type | Default | Meaning |
|---|---|---|---|
| `buffer` | Integer ≥ 0 | | Index, unique in the graph |
| `id` | String | | Buffer ID, unique in the graph and stable across compilations |
| `channels` | 1 to 8 | 1 | Channel count |
| `seconds` | Number > 0 | | Length. Without it, the buffer is as long as its file |
| `file` | String | | Audio file loaded into the buffer, relative to the server's project root |
| `resample` | Boolean | `true` | Whether the file is resampled to the server's sample rate. Only with `file`; the writer omits it when true |
| `src` | Object | | Definition site, as for nodes |

A declaration has `seconds`, `file`, or both. With both, a longer file is cut and a shorter one leaves the rest silent. The server decodes WAV, AIFF, FLAC, MP3, and Ogg Vorbis files and resamples them to its sample rate, unless `resample` is false: then the buffer holds the file's samples as they are, at the file's own rate, and its `seconds` count at that rate. Readers then play it, and writers record into it, at the file's rate, and positions in it count in the file's seconds. A file that does not load refuses the plan with an error naming the buffer: `buffer <id>: cannot load <file>: <reason>`.

A buffer whose ID, channel count, `seconds`, `file`, and `resample` match a buffer of a plan the server holds is the same buffer: the new plan shares it, with its contents. The memory of every buffer the server holds is bounded by `--buffer-memory`; a plan that would exceed it is refused.

## Spectral frames

The spectral types' `in` and `out` ports, other than `fft`'s input and `ifft`'s output, are frame ports. An event pipe that a frame port connects carries spectral frames: only frame ports may connect to it, it has exactly one writer, and every frame input reads a pipe that a frame output writes. `fft`'s `size` is a power of two from 256 to 16384 and its `overlap` 2 or 4.

## Nodes

| Field | Type | Required | Meaning |
|---|---|---|---|
| `id` | String | Yes | Node ID, unique in the graph and stable across compilations |
| `type` | String | Yes | Primitive type, such as `sineOsc` |
| `params` | Object | No | Parameter values by name; omitted parameters take their defaults |
| `inputs` | Object | No | Input ports by name: a pipe reference or an array of them |
| `outputs` | Object | No | Output ports by name: a pipe reference or an array of them |
| `src` | Object | No | Definition site: `file`, `line`, `column`, and optionally `offset` and `length` |

A parameter value is one of:

| Value | Use |
|---|---|
| Number | Numeric parameters, in the parameter's canonical unit |
| String | Choice parameters (one of the listed names) and text parameters |
| Array of numbers | List parameters, such as a sequencer's `values` |
| Array of strings | Text list parameters, such as a `webIn`'s `fields`; `[]` is an empty list |
| Pipe reference | Parameters bound to a pipe of a type the parameter accepts |
| `{"buffer": index}` | Buffer parameters, which every node that has one must give |

## Execution order

Nodes run in array order once per block. Every node that reads a pipe, through an input or a bound parameter, follows every node that writes that pipe. A cycle, including a node that reads a pipe it writes, is valid only through a feedback pipe. The compiler also places a buffer's writers before its readers, unless that would close a cycle; the server does not require it.

The server may run the nodes of a block on several threads. Two nodes run in array order when they use one pipe, buffer, I/O port, or the device's outputs and at least one writes it, or when both report signal faults; a read of an audio or control feedback pipe uses the previous block's data and orders nothing. Other nodes may run at the same time, in either order. Writers of one audio pipe add into it in array order. A plan renders the same bits at every thread count: every thread that runs nodes rounds to nearest and flushes denormal values to zero, and the build neither contracts multiplies and adds into fused operations nor accepts `-ffast-math`.

## Plan swaps

A node whose ID, type, parameter bindings, port shapes, identity parameters (a `clipPlayer`'s `bank`), and the IDs of the nodes feeding its audio inputs match a node of the running plan takes over that node's state and does not fade. Other nodes of the new plan fade in, and unmatched nodes of the running plan fade out, over the fade time given with `/play`. The fade applies once along each signal path, where it leaves the fading nodes. Note-offs pass through fading nodes, so notes held across a swap end. A swap requested during a fade waits for it to finish; a quantized swap then waits for the next beat or bar.

## Output safety

`audioOut` is the last stage before the device. It limits its output to its `ceiling` parameter (a level, at most 1; default 1, full scale) with a peak limiter: instant attack, 50 ms release. Signals below the ceiling pass unchanged. After every sink has run, each device channel is clipped to full scale, since sinks sharing a channel add. A non-finite sample (infinity or NaN) mutes the node until its plan is replaced, and the server reports the node to clients as an unsolicited `/fail` with code 10. Control values set with `/c.set` or received by `oscIn` that are not finite are dropped.

## I/O errors

Serial and web nodes keep counts of what they receive, send, and lose. The server reports a node's failed requests, streams, sockets, and devices to clients as an unsolicited `/io.error` message: the node ID, the latest message, and how many errors occurred since the last report, at most once a second for each node. The `/counters` message carries the total number of I/O errors as its ninth argument. An offline render opens no device, makes no request, and opens no page listener.

## Page layout

`page.layout` is a tree of elements whose root has the kind `page`. Each element is an object:

| Member | Value | Required | Meaning |
|---|---|---|---|
| `kind` | A string | Yes | The element's kind |
| `label` | A string | No | A group's title, a control's label |
| `address` | A string starting with `/` | For a control or display, unless its kind has a default | The OSC address on the page port it sends to or shows |
| `params` | An object of parameter values | No | Its kind's parameters, in canonical units; the rest take their defaults |
| `children` | An array of elements | No | A group's elements, in order |
| `src` | A source span | No | Where the element is written |

| Kind | Holds or does | Parameters |
|---|---|---|
| `page` | `panel`, `synth`, `pedalboard`, or only `tab` | `theme` (`carbon`, `paper`), `accent` (a color) |
| `tab` | `panel`, `synth`, `pedalboard` | `from`, `to` (names of other tabs; `to` also `output`) |
| `panel` | `section` | `flow` (`row`, `column`), `size` |
| `synth` | `section` | `maker`, `size` |
| `pedalboard` | `pedal` | `size` |
| `section` | Controls, displays, and screens | `columns`, `size` |
| `pedal` | Controls, displays, and screens | `size` |
| `knob`, `slider` | Sends a value | `min`, `max`, `scale`, `step`, `initial`, `unit`, `options`; a knob also `size`, `sweep`, `angles`, `steps`, `mod` (the span its modulations reach, two numbers in its travel); a slider `orientation` |
| `drawbar` | Sends a value | `min`, `max`, `step`, `initial`, `unit`, `options` |
| `toggle`, `footswitch`, `button` | Sends 1 and 0 | `initial` (a toggle 0, a footswitch 1); a toggle `orientation` |
| `select` | Sends an option's index | `options` (required), `initial` |
| `xy` | Sends x and y | `min`, `max`, `scale`, `step`, `initial` (two numbers), `unit` |
| `keys` | Sends notes; `/note` by default | `low`, `high`, `velocity` |
| `clips` | Sends a slot of a clip bank; an `oscOut` sends its `clipPlayer`'s `playing` and `queued` at the address's `/playing` and `/queued` | `bank`, `slots`, `grid` (beats), `initial` |
| `meter`, `lamp` | Shows a value | `min`, `max`; a meter `scale`, `hold` (seconds), `orientation` |
| `value` | Shows a value | `unit`, `options` |
| `screen` | An area for a page's own script; no address | |

Every element but the root also takes `span` and `at`, each two whole numbers from 1 to 4096; `size`, on the groups that take it, is two whole numbers from 1 to 4096, its width and height in units. The compiler writes every section child's `at` and `span`, each pedal child's (its `at` row is its row's top in the pedal), and each section's and pedal's `size`; the validator lays a layout out again and refuses one whose `size` is smaller than its contents need or whose children overlap. A control's address is one an `oscIn` on the page port reads, or an `oscIn` there without an address reads every one; a display's is one an `oscOut` there sends; `/espresso/reload` and `/espresso/clips` are reserved for the server on the page port. A control's `initial` lies within `min` to `max`, a knob's of `steps` or `angles` within its positions. `options` on a control other than a `select` are names with no comma, and `accent` is `#` with 3, 4, 6, or 8 hexadecimal digits or a CSS color's name.

## Validation

The server rejects IR nested more than 64 levels deep, and IR that does not match the schema or the primitive tables. Every required parameter is given, and a required text parameter, such as a web node's `url` or a `bufferSave`'s `file`, is not empty. Each primitive's constraints also apply:

- A `mix` whose `levels` has entries has one per pipe of its `in` input.
- A `sequencer` has values.
- An `svf` has an output. An `oscIn`, a `serialIn`, a `webIn`, a `webStream`, and a `webSocket` have an `out` or a `value` output.
- An `oscOut` has an `in` or a `value` input; with a `value` input, its `address` is not empty.
- A `pan`'s output pipe has at least two channels.
- An `adsr` with an `in` input writes an audio pipe.
- A `wavetableOsc`'s `table-size`, and an `fft`'s `size` and `overlap`, are powers of two.
- A serial node's `baud` is a standard rate: 9600, 19200, 38400, 57600, 115200, 230400, 460800, or 921600.
- A web node's `url` has its type's scheme: `ws://` or `wss://` for `webSocket`, `http://` or `https://` for the others. It has at most 8 `fields`, and, with a `value` pipe, one per channel of that pipe. Each of its `headers` is `Name: value`. A `webOut` with a `value` input has `fields`.
- A `clipPlayer`'s `bank` is lowercase letters, digits, and hyphens, and no other `clipPlayer` plays it.

A layout's elements are checked against their kinds (Page layout); an error names the element's path, such as `layout: page > panel "Synth" > section "Filter" > knob "Q"`. Each error names the failing node and, when present, its `src` location:

```text
main.lisp:3:5: node main.lisp/piece/sine#0: parameter freq is -5; expected 0 to 100000
```

## Example

A sawtooth through a filter swept by an LFO, enveloped by a half-beat metronome, and panned to stereo:

```json
{
  "format": "espresso-ir", "version": 1, "seed": 3, "tempo": 110,
  "pipes": [
    {"pipe": {"event": 0}},
    {"pipe": {"audio": 0}},
    {"pipe": {"control": 1}},
    {"pipe": {"audio": 1}},
    {"pipe": {"audio": 2}},
    {"pipe": {"audio": 3}, "channels": 2}
  ],
  "nodes": [
    {"id": "clock", "type": "metro", "params": {"every": 0.5}, "outputs": {"out": {"event": 0}}},
    {"id": "osc", "type": "sawOsc", "params": {"freq": 110}, "outputs": {"out": {"audio": 0}}},
    {"id": "sweep", "type": "lfo", "params": {"rate": 0.25, "depth": 1500, "offset": 2000},
     "outputs": {"out": {"control": 1}}},
    {"id": "filter", "type": "svf", "params": {"cutoff": {"control": 1}, "q": 4},
     "inputs": {"in": {"audio": 0}}, "outputs": {"out": {"audio": 1}}},
    {"id": "env", "type": "adsr",
     "params": {"attack": 0.005, "decay": 0.12, "sustain": 0, "release": 0.05, "gain": 0.5},
     "inputs": {"in": {"audio": 1}, "gate": {"event": 0}}, "outputs": {"out": {"audio": 2}}},
    {"id": "pan", "type": "pan", "params": {"position": -0.3},
     "inputs": {"in": {"audio": 2}}, "outputs": {"out": {"audio": 3}}},
    {"id": "out", "type": "audioOut", "inputs": {"in": {"audio": 3}}}
  ]
}
```

## Primitive types

| Group | Types |
|---|---|
| Sources | `audioIn`, `midiIn`, `oscIn`, `serialIn`, `webIn`, `webStream`, `webSocket` |
| Sinks | `audioOut`, `midiOut`, `oscOut`, `bufferSave`, `serialOut`, `webOut` |
| Oscillators | `sineOsc`, `sawOsc`, `pulseOsc`, `triangleOsc`, `noise` |
| Filters | `biquad`, `svf`, `ladder` |
| Envelopes and modulation | `adsr`, `lfo`, `smoother` |
| Delays | `delayLine`, `comb`, `allpass` |
| Buffers | `bufferWrite`, `bufferRead`, `samplePlayer`, `grains`, `wavetableOsc` |
| Spectral | `fft`, `ifft`, `pvDelay`, `pvFreeze`, `pvBlur` |
| Math and routing | `add`, `multiply`, `mix`, `gain`, `clip`, `pan`, `crossfade` |
| Conversion | `dbToAmp`, `beatsToSeconds` |
| Mapper operations | `neg`, `abs`, `exp`, `log`, `sin`, `cos`, `tanh`, `floor`, `round`, `subtract`, `divide`, `pow`, `min`, `max`, `quantize`, `clamp`, `scale`, `scaleExp` |
| Events | `noteToFreq`, `gate`, `velocity`, `controlChange`, `pitchBend`, `eventFilter`, `oscNotes`, `voiceSplit`, `triggerNotes`, `metro`, `sequencer`, `chance`, `threshold`, `sampleAndHold`, `clipPlayer` |

The parameters, defaults, ranges, and ports of each type are listed in `src/metadata/primitiveSpec.cpp`.
