<!-- Written from src/metadata/primitiveSpec.cpp by `cmake --build build --target primitive-reference`; tests/interpreter/primitiveReference_test.cpp fails when it differs. -->

# Primitives

Every primitive, by the name the language calls it by and its IR type. A parameter takes a constant, written in any unit of its dimension, or, where the table names pipe kinds, a pipe of one of those kinds. Defaults and ranges are in the units shown; levels are shown in decibels. Ports connect pipes: inputs read them and outputs write them. The [language reference](../language.md) describes what the primitives do, and the [IR format](../ir.md) how they are written in the IR.

## Sources

### audio-in

IR type `audioIn`, a source. Inputs: none. Outputs: `:out` (audio, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:channel` | number | 0 | 0 to 255 |  |

### midi-in

IR type `midiIn`, a source. Inputs: none. Outputs: `:out` (event, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:device` | text |  |  |  |
| `:channel` | number | 0 | 0 to 16 |  |

### osc-in

IR type `oscIn`, a source. Inputs: none. Outputs: `:out` (event), `:value` (control).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:port` | number | 57120 | 1 to 65535 |  |
| `:address` | text |  |  |  |
| `:initial` | number | 0 |  |  |

### serial-in

IR type `serialIn`, a source. Inputs: none. Outputs: `:out` (event), `:value` (control).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:device` | text |  |  |  |
| `:baud` | number | 115200 | 9600 to 921600 |  |

### web-in

IR type `webIn`, a source. Inputs: none. Outputs: `:out` (event), `:value` (control).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:url` | text | required |  |  |
| `:every` | time | 10 s | 100 ms to 3600 s |  |
| `:fields` | text list |  |  |  |
| `:headers` | text list |  |  |  |
| `:timeout` | time | 5 s | 100 ms to 3600 s |  |

### web-stream

IR type `webStream`, a source. Inputs: none. Outputs: `:out` (event), `:value` (control).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:url` | text | required |  |  |
| `:headers` | text list |  |  |  |
| `:fields` | text list |  |  |  |
| `:event` | text |  |  |  |

### web-socket

IR type `webSocket`, a source. Inputs: none. Outputs: `:out` (event), `:value` (control).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:url` | text | required |  |  |
| `:headers` | text list |  |  |  |
| `:fields` | text list |  |  |  |
| `:hello` | text |  |  |  |

## Sinks

### audio-out

IR type `audioOut`, a sink. Inputs: `:in` (audio, required). Outputs: none.

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:channel` | number | 0 | 0 to 255 |  |
| `:ceiling` | level | 0 dB | 0 to 0 dB |  |

### midi-out

IR type `midiOut`, a sink. Inputs: `:in` (event, required). Outputs: none.

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:device` | text |  |  |  |
| `:channel` | number | 0 | 0 to 16 |  |

### osc-out

IR type `oscOut`, a sink. Inputs: `:in` (event), `:value` (control). Outputs: none.

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:port` | number | 57120 | 1 to 65535 |  |
| `:address` | text | `/value` |  |  |

### buffer-save

IR type `bufferSave`, a sink. Inputs: `:trigger` (event, required). Outputs: none.

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:buffer` | buffer | required |  |  |
| `:file` | text | required |  |  |

### serial-out

IR type `serialOut`, a sink. Inputs: `:in` (event, required), `:value` (control). Outputs: none.

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:device` | text |  |  |  |
| `:baud` | number | 115200 | 9600 to 921600 |  |

### web-out

IR type `webOut`, a sink. Inputs: `:in` (event, required), `:value` (control). Outputs: `:dropped` (control).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:url` | text | required |  |  |
| `:headers` | text list |  |  |  |
| `:timeout` | time | 5 s | 100 ms to 3600 s |  |
| `:method` | choice | `post` | `:post`, `:put`, `:get` |  |
| `:fields` | text list |  |  |  |

## Oscillators

### sine-osc

IR type `sineOsc`, a generator. Inputs: none. Outputs: `:out` (audio, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:freq` | frequency | 440 Hz | 0 Hz to 100000 Hz | audio, control |
| `:phase` | number | 0 | 0 to 1 | audio, control |

### saw-osc

IR type `sawOsc`, a generator. Inputs: none. Outputs: `:out` (audio, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:freq` | frequency | 440 Hz | 0 Hz to 100000 Hz | audio, control |
| `:phase` | number | 0 | 0 to 1 | audio, control |

### pulse-osc

IR type `pulseOsc`, a generator. Inputs: none. Outputs: `:out` (audio, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:freq` | frequency | 440 Hz | 0 Hz to 100000 Hz | audio, control |
| `:phase` | number | 0 | 0 to 1 | audio, control |
| `:width` | number | 0.5 | 0 to 1 | audio, control |

### triangle-osc

IR type `triangleOsc`, a generator. Inputs: none. Outputs: `:out` (audio, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:freq` | frequency | 440 Hz | 0 Hz to 100000 Hz | audio, control |
| `:phase` | number | 0 | 0 to 1 | audio, control |

### noise

IR type `noise`, a generator. Inputs: none. Outputs: `:out` (audio, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:color` | choice | `white` | `:white`, `:pink`, `:brown` |  |

## Filters

### biquad

IR type `biquad`, a generator. Inputs: `:in` (audio, required). Outputs: `:out` (audio, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:mode` | choice | `low-pass` | `:low-pass`, `:high-pass`, `:band-pass`, `:notch`, `:all-pass`, `:peaking`, `:low-shelf`, `:high-shelf` |  |
| `:freq` | frequency | 1000 Hz | 0 Hz to 100000 Hz | audio, control |
| `:q` | number | 0.7071 | 0.01 to 100 | control |
| `:gain` | level | 0 dB | 0 to 60 dB | control |

### svf

IR type `svf`, a generator. Inputs: `:in` (audio, required). Outputs: `:out` (audio), `:low` (audio), `:band` (audio), `:high` (audio), `:notch` (audio).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:mode` | choice | `low` | `:low`, `:band`, `:high`, `:notch` |  |
| `:cutoff` | frequency | 1000 Hz | 0 Hz to 100000 Hz | audio, control |
| `:q` | number | 0.7071 | 0.01 to 100 | audio, control |

### ladder

IR type `ladder`, a generator. Inputs: `:in` (audio, required). Outputs: `:out` (audio, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:cutoff` | frequency | 1000 Hz | 0 Hz to 100000 Hz | audio, control |
| `:resonance` | number | 0 | 0 to 1 | audio, control |
| `:drive` | level | 0 dB | 0 to 40 dB | audio, control |

## Envelopes and modulation

### adsr

IR type `adsr`, a generator. Inputs: `:in` (audio), `:gate` (event). Outputs: `:out` (audio, control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:attack` | time | 10 ms | 0 s to 3600 s | control |
| `:decay` | time | 100 ms | 0 s to 3600 s | control |
| `:sustain` | level | -3.1 dB | 0 to 0 dB | control |
| `:release` | time | 200 ms | 0 s to 3600 s | control |
| `:gain` | level | 0 dB | 0 to 60 dB | control |

### lfo

IR type `lfo`, a generator. Inputs: none. Outputs: `:out` (audio, control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:shape` | choice | `sine` | `:sine`, `:triangle`, `:saw`, `:square`, `:sample-and-hold` |  |
| `:rate` | frequency | 1 Hz | 0 Hz to 100000 Hz | audio, control |
| `:phase` | number | 0 | 0 to 1 | audio, control |
| `:depth` | number | 1 |  | audio, control |
| `:offset` | number | 0 |  | audio, control |

### smoother

IR type `smoother`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:time` | time | 50 ms | 0 s to 3600 s |  |
| `:curve` | choice | `exponential` | `:exponential`, `:linear` |  |

## Delays

### delay-line

IR type `delayLine`, a generator. Inputs: `:in` (audio, required). Outputs: `:out` (audio, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:time` | time | 250 ms | 0 s to 3600 s | audio, control |
| `:max-time` | time | 1 s | 0 s to 3600 s |  |
| `:interpolation` | choice | `linear` | `:linear`, `:none`, `:cubic` |  |

### comb

IR type `comb`, a generator. Inputs: `:in` (audio, required). Outputs: `:out` (audio, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:time` | time | 50 ms | 0 s to 3600 s | audio, control |
| `:max-time` | time | 1 s | 0 s to 3600 s |  |
| `:feedback` | number | 0.5 | -1 to 1 | audio, control |
| `:damping` | number | 0.2 | 0 to 1 | audio, control |

### allpass

IR type `allpass`, a generator. Inputs: `:in` (audio, required). Outputs: `:out` (audio, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:time` | time | 5 ms | 0 s to 3600 s | audio, control |
| `:max-time` | time | 100 ms | 0 s to 3600 s |  |
| `:feedback` | number | 0.5 | -1 to 1 | audio, control |

## Buffers

### buffer-write

IR type `bufferWrite`, a generator. Inputs: `:in` (audio, required), `:reset` (event). Outputs: `:position` (control).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:buffer` | buffer | required |  |  |
| `:record` | number | 1 | 0 to 1 | audio, control |
| `:keep` | number | 0 | 0 to 1 | audio, control |
| `:mode` | choice | `loop` | `:loop`, `:once` |  |

### buffer-read

IR type `bufferRead`, a generator. Inputs: none. Outputs: `:out` (audio, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:buffer` | buffer | required |  |  |
| `:position` | time | 0 s | -3600 s to 3600 s | audio, control |
| `:interpolation` | choice | `cubic` | `:cubic`, `:linear`, `:none` |  |

### sample-player

IR type `samplePlayer`, a generator. Inputs: `:trigger` (event, required). Outputs: `:out` (audio, required), `:done` (event).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:buffer` | buffer | required |  |  |
| `:rate` | number | 1 | -1000 to 1000 | audio, control |
| `:root` | number | 60 | 0 to 127 |  |
| `:start` | time | 0 s | 0 s to 3600 s | audio, control |
| `:loop` | choice | `off` | `:off`, `:forward`, `:ping-pong` |  |
| `:loop-start` | time | 0 s | 0 s to 3600 s | audio, control |
| `:loop-end` | time | 0 s | 0 s to 3600 s | audio, control |
| `:interpolation` | choice | `cubic` | `:cubic`, `:linear`, `:none` |  |

### grains

IR type `grains`, a generator. Inputs: `:trigger` (event). Outputs: `:out` (audio, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:buffer` | buffer | required |  |  |
| `:voices` | number | 32 | 1 to 128 |  |
| `:density` | frequency | 10 Hz | 0 Hz to 1000 Hz | audio, control |
| `:duration` | time | 100 ms | 1 ms to 10 s | audio, control |
| `:position` | time | 0 s | -3600 s to 3600 s | audio, control |
| `:jitter` | time | 0 s | 0 s to 3600 s | audio, control |
| `:pitch` | interval | 0 st | -48 st to 48 st | audio, control |
| `:pitch-jitter` | interval | 0 st | 0 st to 48 st | audio, control |
| `:reverse` | number | 0 | 0 to 1 | audio, control |
| `:spread` | number | 0 | 0 to 1 | audio, control |
| `:window` | choice | `hann` | `:hann`, `:tukey`, `:triangle` |  |

### wavetable-osc

IR type `wavetableOsc`, a generator. Inputs: none. Outputs: `:out` (audio, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:buffer` | buffer | required |  |  |
| `:freq` | frequency | 440 Hz | 0 Hz to 100000 Hz | audio, control |
| `:phase` | number | 0 | 0 to 1 | audio, control |
| `:position` | number | 0 | 0 to 1 | audio, control |
| `:table-size` | number | 2048 | 256 to 4096 |  |

## Spectral

### fft

IR type `fft`, a generator. Inputs: `:in` (audio, required). Outputs: `:out` (frames, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:size` | number | 2048 | 256 to 16384 |  |
| `:overlap` | number | 4 | 2 to 4 |  |

### ifft

IR type `ifft`, a generator. Inputs: `:in` (frames, required). Outputs: `:out` (audio, required).

### pv-delay

IR type `pvDelay`, a generator. Inputs: `:in` (frames, required), `:redraw` (event). Outputs: `:out` (frames, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:max-time` | time | 4 s | 1 ms to 60 s |  |
| `:time` | time | 500 ms | 0 s to 3600 s | audio, control |
| `:shape` | choice | `flat` | `:flat`, `:rising`, `:falling`, `:random` |  |
| `:spread` | number | 1 | 0 to 1 | audio, control |
| `:feedback` | number | 0 | -0.99 to 0.99 | audio, control |
| `:mix` | number | 1 | 0 to 1 | audio, control |

### pv-freeze

IR type `pvFreeze`, a generator. Inputs: `:in` (frames, required). Outputs: `:out` (frames, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:hold` | number | 0 | 0 to 1 | audio, control |
| `:fade` | time | 50 ms | 0 s to 10 s |  |

### pv-blur

IR type `pvBlur`, a generator. Inputs: `:in` (frames, required). Outputs: `:out` (frames, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:amount` | number | 0.5 | 0 to 1 | audio, control |

## Math and routing

### add

IR type `add`, a generator. Inputs: `:in` (audio, control, required, several pipes). Outputs: `:out` (audio, control, required).

### multiply

IR type `multiply`, a generator. Inputs: `:in` (audio, control, required, several pipes). Outputs: `:out` (audio, control, required).

### mix

IR type `mix`, a generator. Inputs: `:in` (audio, control, required, several pipes). Outputs: `:out` (audio, control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:levels` | list |  |  |  |

### gain

IR type `gain`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:amount` | level | 0 dB |  | audio, control |

### clip

IR type `clip`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:low` | number | -1 |  | audio, control |
| `:high` | number | 1 |  | audio, control |

### pan

IR type `pan`, a generator. Inputs: `:in` (audio, required). Outputs: `:out` (audio, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:position` | number | 0 | -1 to 1 | audio, control |

### crossfade

IR type `crossfade`, a generator. Inputs: `:a` (audio, control, required), `:b` (audio, control, required). Outputs: `:out` (audio, control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:position` | number | 0.5 | 0 to 1 | audio, control |
| `:law` | choice | `equal-power` | `:equal-power`, `:linear` |  |

## Conversion

### db-to-amp

IR type `dbToAmp`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

### beats-to-seconds

IR type `beatsToSeconds`, a generator. Inputs: `:in` (audio, control). Outputs: `:out` (audio, control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:beats` | number | 0 |  |  |

## Mapper operations

### neg

IR type `neg`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

### abs

IR type `abs`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

### exp

IR type `exp`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

### log

IR type `log`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

### sin

IR type `sin`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

### cos

IR type `cos`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

### tanh

IR type `tanh`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

### floor

IR type `floor`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

### round

IR type `round`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

### subtract

IR type `subtract`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:b` | number | 0 |  | audio, control |

### divide

IR type `divide`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:b` | number | 1 |  | audio, control |

### pow

IR type `pow`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:b` | number | 1 |  | audio, control |

### min

IR type `min`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:b` | number | 0 |  | audio, control |

### max

IR type `max`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:b` | number | 0 |  | audio, control |

### quantize

IR type `quantize`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:step` | number | 1 | from 0 | audio, control |

### clamp

IR type `clamp`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:low` | number | -1 |  | audio, control |
| `:high` | number | 1 |  | audio, control |

### scale

IR type `scale`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:in-low` | number | 0 |  | audio, control |
| `:in-high` | number | 1 |  | audio, control |
| `:out-low` | number | 0 |  | audio, control |
| `:out-high` | number | 1 |  | audio, control |

### scale-exp

IR type `scaleExp`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (audio, control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:in-low` | number | 0 |  | audio, control |
| `:in-high` | number | 1 |  | audio, control |
| `:out-low` | number | 0 |  | audio, control |
| `:out-high` | number | 1 |  | audio, control |

## Events

### note-to-freq

IR type `noteToFreq`, a generator. Inputs: `:in` (event, required). Outputs: `:out` (control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:reference` | frequency | 440 Hz | 0 Hz to 100000 Hz | audio, control |

### gate

IR type `gate`, a generator. Inputs: `:in` (event, required). Outputs: `:out` (control, required).

### velocity

IR type `velocity`, a generator. Inputs: `:in` (event, required). Outputs: `:out` (control, required).

### control-change

IR type `controlChange`, a generator. Inputs: `:in` (event, required). Outputs: `:out` (control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:cc` | number | 1 | 0 to 127 |  |
| `:channel` | number | 0 | 0 to 16 |  |

### pitch-bend

IR type `pitchBend`, a generator. Inputs: `:in` (event, required). Outputs: `:out` (control, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:range` | interval | 2 st | 0 st to 96 st |  |
| `:channel` | number | 0 | 0 to 16 |  |

### event-filter

IR type `eventFilter`, a generator. Inputs: `:in` (event, required). Outputs: `:out` (event, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:type` | choice | `any` | `:any`, `:note`, `:control-change`, `:pitch-bend`, `:program-change`, `:pressure`, `:trigger`, `:value`, `:osc` |  |
| `:channel` | number | 0 | 0 to 16 |  |
| `:low` | number | 0 | 0 to 127 |  |
| `:high` | number | 127 | 0 to 127 |  |
| `:address` | text |  |  |  |

### osc-notes

IR type `oscNotes`, a generator. Inputs: `:in` (event, required). Outputs: `:out` (event, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:address` | text | `/note` |  |  |

### voice-split

IR type `voiceSplit`, a generator. Inputs: `:in` (event, required). Outputs: `:out` (event, required, several pipes).

### trigger-notes

IR type `triggerNotes`, a generator. Inputs: `:in` (event, required). Outputs: `:out` (event, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:pitch` | number | 60 | 0 to 127 | control |
| `:velocity` | number | 0.8 | 0 to 1 | control |
| `:length` | time | 250 ms | 0 s to 3600 s | control |

### metro

IR type `metro`, a generator. Inputs: none. Outputs: `:out` (event, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:every` | beats | 1 beat | 0.015625 beats to 1000 beats |  |

### sequencer

IR type `sequencer`, a generator. Inputs: `:in` (event, required). Outputs: `:out` (control), `:events` (event).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:values` | list |  |  |  |
| `:order` | choice | `forward` | `:forward`, `:backward`, `:ping-pong`, `:random`, `:once` |  |

### chance

IR type `chance`, a generator. Inputs: `:in` (event, required). Outputs: `:out` (event, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:probability` | number | 0.5 | 0 to 1 | control |

### threshold

IR type `threshold`, a generator. Inputs: `:in` (audio, control, required). Outputs: `:out` (event, required).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:level` | number | 0 |  | control |
| `:direction` | choice | `rising` | `:rising`, `:falling`, `:both` |  |

### sample-and-hold

IR type `sampleAndHold`, a generator. Inputs: `:trigger` (event, required), `:in` (audio, control, required). Outputs: `:out` (control, required).

### clip-player

IR type `clipPlayer`, a generator. Inputs: none. Outputs: `:out` (event, required), `:playing` (control), `:queued` (control).

| Parameter | Dimension | Default | Values | Pipes |
|---|---|---|---|---|
| `:bank` | text | required |  |  |
| `:slots` | number | 8 | 1 to 64 |  |
| `:grid` | beats | 16 beats | 0.015625 beats to 1000 beats |  |
| `:select` | number | -1 | -1 to 63 | control |
