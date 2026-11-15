// Parameter and port tables (primitives.md §7.6, §7.8, §7.9; sources and
// sinks §7.2, §7.3). Every value is canonical: seconds, hertz, linear
// amplitude, semitones, beats.

#include <algorithm>

#include <proj/metadata/primitiveSpec.h>
#include <proj/metadata/text.h>

namespace metadata {

std::string_view toString(dimension d) noexcept {
    switch (d) {
    case dimension::number: return "number";
    case dimension::time: return "time";
    case dimension::frequency: return "frequency";
    case dimension::level: return "level";
    case dimension::interval: return "interval";
    case dimension::beats: return "beats";
    case dimension::choice: return "choice";
    case dimension::text: return "text";
    case dimension::list: return "list";
    }
    return "number";
}

namespace {

constexpr double maxFreq = 1e5;
constexpr double maxTime = 3600.0;

// --- Parameter builders -------------------------------------------------

constexpr paramSpec num(std::string_view name, double def, double lo, double hi,
                        kindSet bind = signalPipes) {
    return {.name = name, .dim = dimension::number, .defaultValue = def, .min = lo, .max = hi,
            .bindable = bind};
}
constexpr paramSpec qty(std::string_view name, dimension dim, double def, double lo, double hi,
                        kindSet bind = signalPipes) {
    return {.name = name, .dim = dim, .defaultValue = def, .min = lo, .max = hi, .bindable = bind};
}
constexpr paramSpec freq(std::string_view name = "freq", double def = 440.0) {
    return qty(name, dimension::frequency, def, 0.0, maxFreq);
}
constexpr paramSpec seconds(std::string_view name, double def, kindSet bind = signalPipes) {
    return qty(name, dimension::time, def, 0.0, maxTime, bind);
}
constexpr paramSpec choice(std::string_view name, std::span<const std::string_view> names) {
    return {.name = name, .dim = dimension::choice, .choices = names, .defaultText = names.front()};
}
constexpr paramSpec text(std::string_view name, std::string_view def) {
    return {.name = name, .dim = dimension::text, .defaultText = def};
}
constexpr paramSpec list(std::string_view name) { return {.name = name, .dim = dimension::list}; }

constexpr paramSpec phase = num("phase", 0.0, 0.0, 1.0);
constexpr paramSpec midiChannel = num("channel", 0.0, 0.0, 16.0, staticOnly);

// --- Choices ------------------------------------------------------------

constexpr std::string_view noiseColors[] = {"white", "pink", "brown"};
constexpr std::string_view biquadModes[] = {"low-pass", "high-pass", "band-pass", "notch",
                                            "all-pass", "peaking",   "low-shelf", "high-shelf"};
constexpr std::string_view svfModes[] = {"low", "band", "high", "notch"};
constexpr std::string_view lfoShapes[] = {"sine", "triangle", "saw", "square", "sample-and-hold"};
constexpr std::string_view curves[] = {"exponential", "linear"};
constexpr std::string_view interpolations[] = {"linear", "none", "cubic"};
constexpr std::string_view laws[] = {"equal-power", "linear"};
constexpr std::string_view eventTypes[] = {"any",        "note",     "control-change", "pitch-bend",
                                           "program-change", "pressure", "trigger",  "value", "osc"};
constexpr std::string_view orders[] = {"forward", "backward", "ping-pong", "random"};
constexpr std::string_view directions[] = {"rising", "falling", "both"};

// --- Ports --------------------------------------------------------------

constexpr portSpec port(std::string_view name, kindSet kinds, bool required = true,
                        bool multiple = false) {
    return {.name = name, .kinds = kinds, .required = required, .multiple = multiple};
}

constexpr portSpec audioIn[] = {port("in", audioPipes)};
constexpr portSpec audioOut[] = {port("out", audioPipes)};
constexpr portSpec signalIn[] = {port("in", signalPipes)};
constexpr portSpec signalOut[] = {port("out", signalPipes)};
constexpr portSpec signalIns[] = {port("in", signalPipes, true, true)};
constexpr portSpec controlOut[] = {port("out", controlPipes)};
constexpr portSpec eventIn[] = {port("in", eventPipes)};
constexpr portSpec eventOut[] = {port("out", eventPipes)};
constexpr portSpec svfOuts[] = {port("out", audioPipes, false), port("low", audioPipes, false),
                                port("band", audioPipes, false), port("high", audioPipes, false),
                                port("notch", audioPipes, false)};
constexpr portSpec adsrIns[] = {port("in", audioPipes, false), port("gate", eventPipes, false)};
constexpr portSpec crossfadeIns[] = {port("a", signalPipes), port("b", signalPipes)};
constexpr portSpec voiceOuts[] = {port("out", eventPipes, true, true)};
constexpr portSpec triggerIn[] = {port("in", eventPipes)};
constexpr portSpec sequencerOuts[] = {port("out", controlPipes, false), port("events", eventPipes, false)};
constexpr portSpec holdIns[] = {port("trigger", eventPipes), port("in", signalPipes)};
constexpr portSpec oscIns[] = {port("in", eventPipes, false), port("value", controlPipes, false)};
constexpr portSpec oscOuts[] = {port("out", eventPipes, false), port("value", controlPipes, false)};

constexpr std::span<const portSpec> noPorts = {};
constexpr std::span<const paramSpec> noParams = {};

// --- Constraints the builder relies on ----------------------------------

constexpr std::string_view inPort[] = {"in"};
constexpr std::string_view outPort[] = {"out"};
constexpr std::string_view valuePort[] = {"value"};
constexpr std::string_view inOut[] = {"in", "out"};
constexpr std::string_view svfPorts[] = {"out", "low", "band", "high", "notch"};
constexpr std::string_view oscInPorts[] = {"out", "value"};
constexpr std::string_view oscOutPorts[] = {"in", "value"};

constexpr constraint mixRules[] = {{.kind = rule::entryPerPipe, .param = "levels", .ports = inPort}};
constexpr constraint sequencerRules[] = {{.kind = rule::notEmpty, .param = "values"}};
constexpr constraint svfRules[] = {{.kind = rule::anyOutput, .ports = svfPorts}};
constexpr constraint oscInRules[] = {{.kind = rule::anyOutput, .ports = oscInPorts}};
constexpr constraint oscOutRules[] = {{.kind = rule::anyInput, .ports = oscOutPorts},
                                      {.kind = rule::textWith, .param = "address", .ports = valuePort}};
constexpr constraint panRules[] = {{.kind = rule::channels, .ports = outPort, .count = 2}};
constexpr constraint adsrRules[] = {{.kind = rule::audioWith, .ports = inOut}};

// --- Parameters per type ------------------------------------------------

constexpr paramSpec oscParams[] = {freq(), phase};
constexpr paramSpec pulseParams[] = {freq(), phase, num("width", 0.5, 0.0, 1.0)};
constexpr paramSpec noiseParams[] = {choice("color", noiseColors)};
constexpr paramSpec biquadParams[] = {choice("mode", biquadModes), freq("freq", 1000.0),
                                      num("q", 0.7071, 0.01, 100.0, controlPipes),
                                      qty("gain", dimension::level, 1.0, 0.0, 1e3, controlPipes)};
constexpr paramSpec svfParams[] = {choice("mode", svfModes), freq("cutoff", 1000.0),
                                   num("q", 0.7071, 0.01, 100.0)};
constexpr paramSpec ladderParams[] = {freq("cutoff", 1000.0), num("resonance", 0.0, 0.0, 1.0),
                                      qty("drive", dimension::level, 1.0, 0.0, 100.0)};
constexpr paramSpec adsrParams[] = {seconds("attack", 0.01, controlPipes), seconds("decay", 0.1, controlPipes),
                                    qty("sustain", dimension::level, 0.7, 0.0, 1.0, controlPipes),
                                    seconds("release", 0.2, controlPipes),
                                    qty("gain", dimension::level, 1.0, 0.0, 1e3, controlPipes)};
constexpr paramSpec lfoParams[] = {choice("shape", lfoShapes), freq("rate", 1.0), phase,
                                   num("depth", 1.0, -1e9, 1e9), num("offset", 0.0, -1e9, 1e9)};
constexpr paramSpec smootherParams[] = {seconds("time", 0.05, staticOnly), choice("curve", curves)};
constexpr paramSpec delayParams[] = {seconds("time", 0.25), seconds("max-time", 1.0, staticOnly),
                                     choice("interpolation", interpolations)};
constexpr paramSpec combParams[] = {seconds("time", 0.05), seconds("max-time", 1.0, staticOnly),
                                    num("feedback", 0.5, -1.0, 1.0), num("damping", 0.2, 0.0, 1.0)};
constexpr paramSpec allpassParams[] = {seconds("time", 0.005), seconds("max-time", 0.1, staticOnly),
                                       num("feedback", 0.5, -1.0, 1.0)};
constexpr paramSpec mixParams[] = {list("levels")};
constexpr paramSpec gainParams[] = {qty("amount", dimension::level, 1.0, -1e9, 1e9)};
constexpr paramSpec clipParams[] = {num("low", -1.0, -1e9, 1e9), num("high", 1.0, -1e9, 1e9)};
constexpr paramSpec panParams[] = {num("position", 0.0, -1.0, 1.0)};
constexpr paramSpec crossfadeParams[] = {num("position", 0.5, 0.0, 1.0), choice("law", laws)};
constexpr paramSpec operandParams[] = {num("b", 0.0, -1e9, 1e9)};
constexpr paramSpec divisorParams[] = {num("b", 1.0, -1e9, 1e9)};
constexpr paramSpec stepParams[] = {num("step", 1.0, 0.0, 1e9)};
constexpr paramSpec scaleParams[] = {num("in-low", 0.0, -1e9, 1e9), num("in-high", 1.0, -1e9, 1e9),
                                     num("out-low", 0.0, -1e9, 1e9), num("out-high", 1.0, -1e9, 1e9)};
constexpr paramSpec noteToFreqParams[] = {freq("reference", 440.0)};
constexpr paramSpec ccParams[] = {num("cc", 1.0, 0.0, 127.0, staticOnly), midiChannel};
constexpr paramSpec bendParams[] = {qty("range", dimension::interval, 2.0, 0.0, 96.0, staticOnly),
                                    midiChannel};
constexpr paramSpec filterParams[] = {choice("type", eventTypes), midiChannel,
                                      num("low", 0.0, 0.0, 127.0, staticOnly),
                                      num("high", 127.0, 0.0, 127.0, staticOnly), text("address", "")};
constexpr paramSpec metroParams[] = {qty("every", dimension::beats, 1.0, 1.0 / 64.0, 1e3, staticOnly)};
constexpr paramSpec sequencerParams[] = {list("values"), choice("order", orders)};
constexpr paramSpec chanceParams[] = {num("probability", 0.5, 0.0, 1.0, controlPipes)};
constexpr paramSpec thresholdParams[] = {num("level", 0.0, -1e9, 1e9, controlPipes),
                                         choice("direction", directions)};
constexpr paramSpec deviceChannelParams[] = {num("channel", 0.0, 0.0, 255.0, staticOnly)};
constexpr paramSpec audioOutParams[] = {num("channel", 0.0, 0.0, 255.0, staticOnly),
                                        qty("ceiling", dimension::level, 1.0, 0.0, 1.0, staticOnly)};
constexpr paramSpec midiParams[] = {text("device", ""), midiChannel};
constexpr paramSpec oscInParams[] = {num("port", 57120.0, 1.0, 65535.0, staticOnly), text("address", "")};
constexpr paramSpec oscOutParams[] = {num("port", 57120.0, 1.0, 65535.0, staticOnly),
                                      text("address", "/value")};

// --- The table ----------------------------------------------------------

using enum role;

// A mapper operation: reads in, takes its operands as parameters.
constexpr primitiveSpec mapperOp(std::string_view type, operands dims,
                                 std::span<const paramSpec> params = noParams) {
    return {type, generator, params, signalIn, signalOut, {}, dims};
}

constexpr primitiveSpec table[] = {
    // Sources and sinks (§7.2, §7.3)
    {"audioIn", source, deviceChannelParams, noPorts, audioOut},  // Device input channels to an audio pipe
    {"audioOut", sink, audioOutParams, audioIn,
     noPorts},  // An audio pipe to device output channels, limited to a ceiling
    {"midiIn", source, midiParams, noPorts, eventOut},             // MIDI messages from a device
    {"midiOut", sink, midiParams, eventIn, noPorts},               // MIDI events to a device
    {"oscIn", source, oscInParams, noPorts, oscOuts, oscInRules},  // OSC messages from a UDP port
    {"oscOut", sink, oscOutParams, oscIns, noPorts, oscOutRules},  // OSC messages to a UDP port
    // Oscillators (§7.6)
    {"sineOsc", generator, oscParams, noPorts, audioOut},      // Sine oscillator
    {"sawOsc", generator, oscParams, noPorts, audioOut},       // Band-limited sawtooth oscillator
    {"pulseOsc", generator, pulseParams, noPorts, audioOut},   // Band-limited pulse oscillator
    {"triangleOsc", generator, oscParams, noPorts, audioOut},  // Band-limited triangle oscillator
    {"noise", generator, noiseParams, noPorts, audioOut},      // White, pink, or brown noise
    // Filters
    {"biquad", generator, biquadParams, audioIn, audioOut},     // RBJ cookbook biquad filter
    {"svf", generator, svfParams, audioIn, svfOuts, svfRules},  // Trapezoidal state-variable filter
    {"ladder", generator, ladderParams, audioIn, audioOut},     // Resonant 4-pole low-pass ladder filter
    // Envelopes and modulation
    {"adsr", generator, adsrParams, adsrIns, signalOut, adsrRules},  // Attack-decay-sustain-release envelope
    {"lfo", generator, lfoParams, noPorts, signalOut},               // Low-frequency oscillator
    {"smoother", generator, smootherParams, signalIn, signalOut},    // Smooths a signal over a time constant
    // Delays
    {"delayLine", generator, delayParams, audioIn, audioOut},  // Interpolated delay line
    {"comb", generator, combParams, audioIn, audioOut},        // Feedback comb filter with damping
    {"allpass", generator, allpassParams, audioIn, audioOut},  // Schroeder allpass filter
    // Math and routing
    {"add", generator, noParams, signalIns, signalOut},             // Sum of the inputs
    {"multiply", generator, noParams, signalIns, signalOut},        // Product of the inputs
    {"mix", generator, mixParams, signalIns, signalOut, mixRules},  // Sum of the inputs, each with a level
    {"gain", generator, gainParams, signalIn, signalOut},           // Input times an amount
    {"clip", generator, clipParams, signalIn, signalOut},           // Input limited to a range
    {"pan", generator, panParams, audioIn, audioOut, panRules},     // Equal-power mono to stereo panner
    {"crossfade", generator, crossfadeParams, crossfadeIns, signalOut},  // Crossfade between two inputs
    // Conversion (§7.4.5, §7.4.6)
    {"dbToAmp", generator, noParams, signalIn, signalOut},         // Decibels to linear amplitude
    {"beatsToSeconds", generator, noParams, signalIn, signalOut},  // Beats to seconds at the current tempo
    // Mapper operations (§7.9)
    mapperOp("neg", operands::sign),                              // Negation
    mapperOp("abs", operands::sign),                              // Absolute value
    mapperOp("exp", operands::plain),                             // Natural exponential
    mapperOp("log", operands::plain),                             // Natural logarithm
    mapperOp("sin", operands::plain),                             // Sine
    mapperOp("cos", operands::plain),                             // Cosine
    mapperOp("tanh", operands::plain),                            // Hyperbolic tangent
    mapperOp("floor", operands::keep),                            // Largest integer not above the input
    mapperOp("round", operands::keep),                            // Nearest integer
    {"subtract", generator, operandParams, signalIn, signalOut},  // Input minus b; lowers + and -
    {"divide", generator, divisorParams, signalIn, signalOut},    // Input divided by b; lowers /
    mapperOp("pow", operands::plain, divisorParams),              // Input to the power b
    mapperOp("min", operands::same, operandParams),               // Smaller of the input and b
    mapperOp("max", operands::same, operandParams),               // Larger of the input and b
    mapperOp("quantize", operands::same, stepParams),             // Input rounded to a multiple of step
    mapperOp("clamp", operands::same, clipParams),                // Input limited to low and high
    mapperOp("scale", operands::ranges, scaleParams),             // Linear map from one range to another
    mapperOp("scaleExp", operands::ranges, scaleParams),          // Exponential map from one range to another
    // Event generators (§7.8)
    {"noteToFreq", generator, noteToFreqParams, eventIn, controlOut},  // Latest note to frequency
    {"gate", generator, noParams, eventIn, controlOut},                // Note on and off to 1 and 0
    {"velocity", generator, noParams, eventIn, controlOut},       // Velocity of the latest note-on, 0 to 1
    {"controlChange", generator, ccParams, eventIn, controlOut},  // One controller number to a value, 0 to 1
    {"pitchBend", generator, bendParams, eventIn, controlOut},    // Pitch bend in semitones
    {"eventFilter", generator, filterParams, eventIn, eventOut},  // Keeps matching events
    {"voiceSplit", generator, noParams, eventIn, voiceOuts},  // Assigns notes to voices, stealing the oldest
    {"metro", generator, metroParams, noPorts, eventOut},     // A trigger every N beats
    {"sequencer", generator, sequencerParams, triggerIn, sequencerOuts,
     sequencerRules},                                        // Steps through values on each trigger
    {"chance", generator, chanceParams, eventIn, eventOut},  // Passes each event with a probability
    {"threshold", generator, thresholdParams, signalIn,
     eventOut},                                                   // A trigger when the input crosses a level
    {"sampleAndHold", generator, noParams, holdIns, controlOut},  // Holds the input at each trigger
};

// The first item of @p items whose key, by @p key, is @p name.
template <typename T, typename Key>
optionalRef<T> byKey(std::span<const T> items, std::string_view name, Key key) noexcept {
    const auto it = std::find_if(items.begin(), items.end(), [&](const T& x) { return key(x) == name; });
    if (it == items.end()) return std::nullopt;
    return std::cref(*it);
}

template <typename T>
optionalRef<T> byName(std::span<const T> items, std::string_view name) noexcept {
    return byKey(items, name, [](const T& x) { return x.name; });
}

bool bound(const std::map<std::string, std::vector<pipeRef>, std::less<>>& ports, std::string_view name) {
    const auto it = ports.find(name);
    return it != ports.end() && !it->second.empty();
}

// How constraint messages name parameters and ports.
struct namer {
    naming style;

    std::string param(std::string_view name) const {
        return style == naming::language ? ":" + std::string(name) : "parameter " + std::string(name);
    }
    std::string port(std::string_view name, bool input) const {
        if (style == naming::language) return ":" + std::string(name);
        return (input ? "input " : "output ") + std::string(name);
    }
    std::string ports(std::span<const std::string_view> names, bool input) const {
        if (style == naming::language) return joinNames(names, ":");
        return (input ? "input " : "output ") + joinNames(names);
    }
};

std::string count(std::size_t n, std::string_view one, std::string_view many) {
    return std::to_string(n) + ' ' + std::string(n == 1 ? one : many);
}

} // namespace

optionalRef<paramSpec> primitiveSpec::param(std::string_view name) const noexcept { return byName(params, name); }
optionalRef<portSpec> primitiveSpec::input(std::string_view name) const noexcept { return byName(inputs, name); }
optionalRef<portSpec> primitiveSpec::output(std::string_view name) const noexcept { return byName(outputs, name); }

std::span<const primitiveSpec> primitiveSpecs() noexcept { return table; }

optionalRef<primitiveSpec> findPrimitive(std::string_view type) noexcept {
    return byKey(std::span<const primitiveSpec>(table), type, [](const primitiveSpec& s) { return s.type; });
}

std::vector<std::string> checkConstraints(const primitiveSpec& spec, const nodeSpec& node,
                                          const graphSpec& graph, naming names) {
    const namer say{names};
    auto list = [&](std::string_view name) -> std::span<const double> {
        const auto it = node.params.find(name);
        if (it == node.params.end()) return {};
        const auto l = as<std::vector<double>>(it->second);
        return l ? std::span<const double>(l->get()) : std::span<const double>();
    };
    auto text = [&](std::string_view name) -> std::string_view {
        if (const auto it = node.params.find(name); it != node.params.end()) {
            if (const auto t = as<std::string>(it->second)) return t->get();
        }
        const auto p = spec.param(name);
        return p ? p->get().defaultText : std::string_view();
    };
    std::vector<std::string> out;
    for (const constraint& c : spec.constraints) {
        switch (c.kind) {
        case rule::anyInput:
        case rule::anyOutput: {
            const bool input = c.kind == rule::anyInput;
            const auto& ports = input ? node.inputs : node.outputs;
            if (std::none_of(c.ports.begin(), c.ports.end(),
                             [&](std::string_view p) { return bound(ports, p); })) {
                out.push_back("needs at least one of " + say.ports(c.ports, input));
            }
            break;
        }
        case rule::entryPerPipe: {
            const auto entries = list(c.param);
            const auto it = node.inputs.find(c.ports.front());
            const std::size_t pipes = it == node.inputs.end() ? 0 : it->second.size();
            if (!entries.empty() && entries.size() != pipes) {
                out.push_back(say.param(c.param) + " has " + count(entries.size(), "entry", "entries") +
                              " for " + count(pipes, "pipe", "pipes") + " on " +
                              say.port(c.ports.front(), true));
            }
            break;
        }
        case rule::notEmpty:
            if (list(c.param).empty()) out.push_back(say.param(c.param) + " must not be empty");
            break;
        case rule::channels: {
            const auto it = node.outputs.find(c.ports.front());
            if (it == node.outputs.end() || it->second.empty()) break;
            const auto d = findPipe(graph, it->second.front());
            if (d && d->get().channels < c.count) {
                out.push_back(say.port(c.ports.front(), false) + " needs a pipe with at least " +
                              std::to_string(c.count) + " channels");
            }
            break;
        }
        case rule::audioWith: {
            const auto it = node.outputs.find(c.ports[1]);
            if (!bound(node.inputs, c.ports[0]) || it == node.outputs.end()) break;
            if (std::any_of(it->second.begin(), it->second.end(),
                            [](pipeRef r) { return r.kind != pipeKind::audio; })) {
                out.push_back("with " + say.port(c.ports[0], true) + ", " + say.port(c.ports[1], false) +
                              " must be an audio pipe");
            }
            break;
        }
        case rule::textWith:
            if (bound(node.inputs, c.ports.front()) && text(c.param).empty()) {
                out.push_back("with " + say.port(c.ports.front(), true) + ", " + say.param(c.param) +
                              " must not be empty");
            }
            break;
        }
    }
    return out;
}

} // namespace metadata
