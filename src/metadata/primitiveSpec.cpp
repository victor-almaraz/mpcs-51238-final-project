// Parameter and port tables (primitives.md §7.6, §7.8, §7.9; sources and
// sinks §7.2, §7.3). Every value is canonical: seconds, hertz, linear
// amplitude, semitones, beats.

#include <proj/metadata/primitiveSpec.h>

#include <algorithm>

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

constexpr primitiveSpec unary(std::string_view type, std::string_view summary) {
    return {type, generator, summary, noParams, signalIn, signalOut};
}
constexpr primitiveSpec binary(std::string_view type, std::string_view summary,
                               std::span<const paramSpec> params = operandParams) {
    return {type, generator, summary, params, signalIn, signalOut};
}

constexpr primitiveSpec table[] = {
    // Sources and sinks (§7.2, §7.3)
    {"audioIn", source, "Device input channels to an audio pipe", deviceChannelParams, noPorts, audioOut},
    {"audioOut", sink, "An audio pipe to device output channels, limited to a ceiling", audioOutParams, audioIn, noPorts},
    {"midiIn", source, "MIDI messages from a device", midiParams, noPorts, eventOut},
    {"midiOut", sink, "MIDI events to a device", midiParams, eventIn, noPorts},
    {"oscIn", source, "OSC messages from a UDP port", oscInParams, noPorts, oscOuts},
    {"oscOut", sink, "OSC messages to a UDP port", oscOutParams, oscIns, noPorts},
    // Oscillators (§7.6)
    {"sineOsc", generator, "Sine oscillator", oscParams, noPorts, audioOut},
    {"sawOsc", generator, "Band-limited sawtooth oscillator", oscParams, noPorts, audioOut},
    {"pulseOsc", generator, "Band-limited pulse oscillator", pulseParams, noPorts, audioOut},
    {"triangleOsc", generator, "Band-limited triangle oscillator", oscParams, noPorts, audioOut},
    {"noise", generator, "White, pink, or brown noise", noiseParams, noPorts, audioOut},
    // Filters
    {"biquad", generator, "RBJ cookbook biquad filter", biquadParams, audioIn, audioOut},
    {"svf", generator, "Trapezoidal state-variable filter", svfParams, audioIn, svfOuts},
    {"ladder", generator, "Resonant 4-pole low-pass ladder filter", ladderParams, audioIn, audioOut},
    // Envelopes and modulation
    {"adsr", generator, "Attack-decay-sustain-release envelope", adsrParams, adsrIns, signalOut},
    {"lfo", generator, "Low-frequency oscillator", lfoParams, noPorts, signalOut},
    {"smoother", generator, "Smooths a signal over a time constant", smootherParams, signalIn, signalOut},
    // Delays
    {"delayLine", generator, "Interpolated delay line", delayParams, audioIn, audioOut},
    {"comb", generator, "Feedback comb filter with damping", combParams, audioIn, audioOut},
    {"allpass", generator, "Schroeder allpass filter", allpassParams, audioIn, audioOut},
    // Math and routing
    {"add", generator, "Sum of the inputs", noParams, signalIns, signalOut},
    {"multiply", generator, "Product of the inputs", noParams, signalIns, signalOut},
    {"mix", generator, "Sum of the inputs, each with a level", mixParams, signalIns, signalOut},
    {"gain", generator, "Input times an amount", gainParams, signalIn, signalOut},
    {"clip", generator, "Input limited to a range", clipParams, signalIn, signalOut},
    {"pan", generator, "Equal-power mono to stereo panner", panParams, audioIn, audioOut},
    {"crossfade", generator, "Crossfade between two inputs", crossfadeParams, crossfadeIns, signalOut},
    // Conversion (§7.4.5, §7.4.6)
    {"dbToAmp", generator, "Decibels to linear amplitude", noParams, signalIn, signalOut},
    {"beatsToSeconds", generator, "Beats to seconds at the current tempo", noParams, signalIn, signalOut},
    // Mapper operations (§7.9)
    unary("neg", "Negation"),
    unary("abs", "Absolute value"),
    unary("exp", "Natural exponential"),
    unary("log", "Natural logarithm"),
    unary("sin", "Sine"),
    unary("cos", "Cosine"),
    unary("tanh", "Hyperbolic tangent"),
    unary("floor", "Largest integer not above the input"),
    unary("round", "Nearest integer"),
    binary("subtract", "Input minus b"),
    binary("divide", "Input divided by b", divisorParams),
    binary("pow", "Input to the power b", divisorParams),
    binary("min", "Smaller of the input and b"),
    binary("max", "Larger of the input and b"),
    binary("quantize", "Input rounded to a multiple of step", stepParams),
    binary("clamp", "Input limited to low and high", clipParams),
    binary("scale", "Linear map from one range to another", scaleParams),
    binary("scaleExp", "Exponential map from one range to another", scaleParams),
    // Event generators (§7.8)
    {"noteToFreq", generator, "Latest note to frequency", noteToFreqParams, eventIn, controlOut},
    {"gate", generator, "Note on and off to 1 and 0", noParams, eventIn, controlOut},
    {"velocity", generator, "Velocity of the latest note-on, 0 to 1", noParams, eventIn, controlOut},
    {"controlChange", generator, "One controller number to a value, 0 to 1", ccParams, eventIn, controlOut},
    {"pitchBend", generator, "Pitch bend in semitones", bendParams, eventIn, controlOut},
    {"eventFilter", generator, "Keeps matching events", filterParams, eventIn, eventOut},
    {"voiceSplit", generator, "Assigns notes to voices, stealing the oldest", noParams, eventIn, voiceOuts},
    {"metro", generator, "A trigger every N beats", metroParams, noPorts, eventOut},
    {"sequencer", generator, "Steps through values on each trigger", sequencerParams, triggerIn, sequencerOuts},
    {"chance", generator, "Passes each event with a probability", chanceParams, eventIn, eventOut},
    {"threshold", generator, "A trigger when the input crosses a level", thresholdParams, signalIn, eventOut},
    {"sampleAndHold", generator, "Holds the input at each trigger", noParams, holdIns, controlOut},
};

template <typename T>
optionalRef<T> byName(std::span<const T> items, std::string_view name) noexcept {
    const auto it = std::find_if(items.begin(), items.end(), [&](const T& x) { return x.name == name; });
    if (it == items.end()) return std::nullopt;
    return std::cref(*it);
}

} // namespace

optionalRef<paramSpec> primitiveSpec::param(std::string_view name) const noexcept { return byName(params, name); }
optionalRef<portSpec> primitiveSpec::input(std::string_view name) const noexcept { return byName(inputs, name); }
optionalRef<portSpec> primitiveSpec::output(std::string_view name) const noexcept { return byName(outputs, name); }

std::span<const primitiveSpec> primitiveSpecs() noexcept { return table; }

optionalRef<primitiveSpec> findPrimitive(std::string_view type) noexcept {
    const auto it = std::find_if(std::begin(table), std::end(table),
                                 [&](const primitiveSpec& s) { return s.type == type; });
    if (it == std::end(table)) return std::nullopt;
    return std::cref(*it);
}

} // namespace metadata
