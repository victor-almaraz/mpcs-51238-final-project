# IR format

The intermediate representation (IR) describes a graph of primitives: the pipes it declares and its nodes in execution order. The interpreter emits IR; the server validates it and builds a plan from it. `schema/ir.schema.json` defines the structure. The primitive tables in `include/proj/metadata/primitiveSpec.h` define the types, parameters, and ports.

All values are canonical units: seconds, hertz, linear amplitude, semitones, and beats.

## Document

| Field | Type | Required | Meaning |
|---|---|---|---|
| `format` | `"proj-ir"` | Yes | Format name |
| `version` | `1` | Yes | Format version |
| `seed` | Integer ≥ 0 | No (0) | Project seed for random generators |
| `tempo` | Number > 0 | No | Tempo set when the plan becomes active, in beats per minute |
| `meter` | Integer ≥ 1 | No (4) | Beats per bar, for bar quantization |
| `pipes` | Array of pipe declarations | Yes | Pipes the graph uses |
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
| `video` | One frame: premultiplied RGBA pixels at the server's frame size, with the transform drawing applies | The last writer in execution order |

A pipe declaration names a pipe and its shape:

| Field | Type | Default | Meaning |
|---|---|---|---|
| `pipe` | Pipe reference | | The pipe |
| `channels` | 1 to 8 | 1 | Channel count. Event and video pipes have one channel |
| `feedback` | Boolean | `false` | Delays the pipe's data by one block. Audio and control pipes only |

Control pipe 0 is the tempo pipe, in beats per minute. It exists without a declaration, and any node may read or write it.

Index ranges per type are set by the server's pipe table: 256 audio pipes, 1024 control pipes, 128 event pipes, and 16 video pipes.

## Clock domains

A node that reads or writes a video pipe belongs to the video domain and runs once per frame on the render thread; every other node runs once per block on the audio thread. A video node's parameter bound to a control pipe reads its current value; bound to an audio pipe, it reads the pipe's last sample of the latest block. The visual layer shows the input of the first `videoOut` node. A video pipe that one drawing node writes and one drawing node reads as its input, and that no other node reads, links a chain: the chain draws in place into the frame of the pipe it ends in, and its linking pipes are not written. A chain whose last pipe has another writer copies its frames from node to node.

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
| Array of four numbers, or `"none"` | Color parameters: red, green, blue, alpha, each 0 to 1 (straight alpha); `"none"` for a fill or stroke that is not drawn |
| Pipe reference | Parameters bound to a pipe of a type the parameter accepts |

## Execution order

Nodes run in array order once per block. Every node that reads a pipe, through an input or a bound parameter, follows every node that writes that pipe. A cycle, including a node that reads a pipe it writes, is valid only through a feedback pipe.

## Plan swaps

A node whose ID, type, parameter bindings, port shapes, and the IDs of the nodes feeding its audio inputs match a node of the running plan takes over that node's state and does not fade. Other nodes of the new plan fade in, and unmatched nodes of the running plan fade out, over the fade time given with `/play`. The fade applies once along each signal path, where it leaves the fading nodes. Note-offs pass through fading nodes, so notes held across a swap end. A swap requested during a fade waits for it to finish; a quantized swap then waits for the next beat or bar.

## Output safety

`audioOut` is the last stage before the device. It limits its output to its `ceiling` parameter (a level, at most 1; default 1, full scale) with a peak limiter: instant attack, 50 ms release. Signals below the ceiling pass unchanged. After every sink has run, each device channel is clipped to full scale, since sinks sharing a channel add. A non-finite sample (infinity or NaN) mutes the node until its plan is replaced, and the server reports the node to clients as an unsolicited `/fail` with code 10. Control values set with `/c.set` or received by `oscIn` that are not finite are dropped.

## Validation

The server rejects IR that does not match the schema or the primitive tables, including each primitive's constraints: a `mix` has one level per input, a `sequencer` has values, an `svf` or `oscIn` has an output, an `oscOut` has an input and an address, a `pan` writes a two-channel pipe, and an `adsr` with an input writes an audio pipe. Each error names the failing node and, when present, its `src` location:

```text
main.lisp:3:5: node main.lisp/piece/sine#0: parameter freq is -5; expected 0 to 100000
```

## Example

A sawtooth through a filter swept by an LFO, enveloped by a half-beat metronome, and panned to stereo:

```json
{
  "format": "proj-ir", "version": 1, "seed": 3, "tempo": 110,
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
| Sources | `audioIn`, `midiIn`, `oscIn` |
| Sinks | `audioOut`, `midiOut`, `oscOut` |
| Oscillators | `sineOsc`, `sawOsc`, `pulseOsc`, `triangleOsc`, `noise` |
| Filters | `biquad`, `svf`, `ladder` |
| Envelopes and modulation | `adsr`, `lfo`, `smoother` |
| Delays | `delayLine`, `comb`, `allpass` |
| Math and routing | `add`, `multiply`, `mix`, `gain`, `clip`, `pan`, `crossfade` |
| Conversion | `dbToAmp`, `beatsToSeconds` |
| Mapper operations | `neg`, `abs`, `exp`, `log`, `sin`, `cos`, `tanh`, `floor`, `round`, `subtract`, `divide`, `pow`, `min`, `max`, `quantize`, `clamp`, `scale`, `scaleExp` |
| Events | `noteToFreq`, `gate`, `velocity`, `controlChange`, `pitchBend`, `eventFilter`, `voiceSplit`, `metro`, `sequencer`, `chance`, `threshold`, `sampleAndHold` |
| Images | `background`, `point`, `line`, `rect`, `ellipse`, `circle`, `triangle`, `quad`, `arc`, `bezier`, `transform`, `lerpColor`, `imagePlayer` |
| Visual layer | `videoOut` |

The parameters, defaults, ranges, and ports of each type are listed in `src/metadata/primitiveSpec.cpp`.
