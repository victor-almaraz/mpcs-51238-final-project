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

A pipe declaration names a pipe and its shape:

| Field | Type | Default | Meaning |
|---|---|---|---|
| `pipe` | Pipe reference | | The pipe |
| `channels` | 1 to 8 | 1 | Channel count. Event pipes have one channel |
| `feedback` | Boolean | `false` | Delays the pipe's data by one block. Audio and control pipes only |

Control pipe 0 is the tempo pipe, in beats per minute. It exists without a declaration, and any node may read or write it.

Index ranges per type are set by the server's pipe table: 256 audio pipes, 1024 control pipes, and 128 event pipes.

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
| Pipe reference | Parameters bound to a pipe of a type the parameter accepts |

## Execution order

Nodes run in array order once per block. Every node that reads a pipe, through an input or a bound parameter, follows every node that writes that pipe. A node may read a pipe it writes. A cycle is valid only through a feedback pipe.

## Plan swaps

A node whose ID, type, parameter bindings, and port shapes match a node of the running plan takes over that node's state and does not fade. Other nodes of the new plan fade in, and unmatched nodes of the running plan fade out, over the fade time given with `/play`.

## Validation

The server rejects IR that does not match the schema or the primitive tables. Each error names the failing node and, when present, its `src` location:

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

The parameters, defaults, ranges, and ports of each type are listed in `src/metadata/primitiveSpec.cpp`.
