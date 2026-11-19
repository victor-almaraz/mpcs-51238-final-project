// Parameter and port tables (primitives.md §7.6, §7.8, §7.9; sources and
// sinks §7.2, §7.3). Every value is canonical: seconds, hertz, linear
// amplitude, semitones, beats.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/metadata/primitiveSpec.h>
#include <espresso/metadata/text.h>

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
    case dimension::color: return "color";
    case dimension::buffer: return "buffer";
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
// A text parameter every node must give, not empty.
constexpr paramSpec requiredText(std::string_view name) {
    return {.name = name, .dim = dimension::text, .required = true};
}
constexpr paramSpec list(std::string_view name) { return {.name = name, .dim = dimension::list}; }
// A buffer the node reads, or writes when @p writes (samples.md §12.1).
constexpr paramSpec buffer(bool writes) {
    return {.name = "buffer", .dim = dimension::buffer, .writes = writes, .required = true};
}
// A color, static (RGBA) or following a 4-channel control pipe (§7.7.4).
constexpr paramSpec color(std::string_view name, rgba def, bool noneAllowed = false) {
    return {.name = name,
            .dim = dimension::color,
            .bindable = controlPipes,
            .defaultColor = def,
            .noneAllowed = noneAllowed};
}

// Image geometry is in pixels of the output frame (§7.7.5).
constexpr double maxPixels = 1e6;
constexpr paramSpec pixels(std::string_view name) { return num(name, 0.0, -maxPixels, maxPixels); }
constexpr paramSpec extent(std::string_view name, double def) { return num(name, def, 0.0, maxPixels); }
// Angles are radians, clockwise from the positive x axis (y points down).
constexpr paramSpec radians(std::string_view name, double def) { return num(name, def, -1e6, 1e6); }

// Processing's defaults: white fill, black stroke one pixel wide.
constexpr paramSpec fill = color("fill", {1.0, 1.0, 1.0, 1.0}, true);
constexpr paramSpec stroke = color("stroke", {0.0, 0.0, 0.0, 1.0}, true);
constexpr paramSpec strokeWeight = extent("stroke-weight", 1.0);

constexpr paramSpec phase = num("phase", 0.0, 0.0, 1.0);
constexpr paramSpec midiChannel = num("channel", 0.0, 0.0, 16.0, staticOnly);

// --- Choices ------------------------------------------------------------

constexpr auto noiseColors = std::to_array<std::string_view>({"white", "pink", "brown"});
constexpr auto biquadModes = std::to_array<std::string_view>(
    {"low-pass", "high-pass", "band-pass", "notch", "all-pass", "peaking", "low-shelf", "high-shelf"});
constexpr auto svfModes = std::to_array<std::string_view>({"low", "band", "high", "notch"});
constexpr auto lfoShapes =
    std::to_array<std::string_view>({"sine", "triangle", "saw", "square", "sample-and-hold"});
constexpr auto curves = std::to_array<std::string_view>({"exponential", "linear"});
constexpr auto interpolations = std::to_array<std::string_view>({"linear", "none", "cubic"});
constexpr auto readInterpolations = std::to_array<std::string_view>({"cubic", "linear", "none"});
constexpr auto writeModes = std::to_array<std::string_view>({"loop", "once"});
constexpr auto loopModes = std::to_array<std::string_view>({"off", "forward", "ping-pong"});
constexpr auto grainWindows = std::to_array<std::string_view>({"hann", "tukey", "triangle"});
constexpr auto delayShapes = std::to_array<std::string_view>({"flat", "rising", "falling", "random"});
constexpr auto laws = std::to_array<std::string_view>({"equal-power", "linear"});
constexpr auto eventTypes = std::to_array<std::string_view>(
    {"any", "note", "control-change", "pitch-bend", "program-change", "pressure", "trigger", "value", "osc"});
constexpr auto orders =
    std::to_array<std::string_view>({"forward", "backward", "ping-pong", "random", "once"});
constexpr auto directions = std::to_array<std::string_view>({"rising", "falling", "both"});

constexpr auto rectModes = std::to_array<std::string_view>({"corner", "corners", "center", "radius"});
constexpr auto ellipseModes = std::to_array<std::string_view>({"center", "radius", "corner", "corners"});
constexpr auto strokeCaps = std::to_array<std::string_view>({"round", "square", "project"});
constexpr paramSpec strokeCap = choice("stroke-cap", strokeCaps);
constexpr auto arcModes = std::to_array<std::string_view>({"open", "chord", "pie"});

// --- Ports --------------------------------------------------------------

constexpr portSpec port(std::string_view name, kindSet kinds, bool required = true,
                        bool multiple = false) {
    return {.name = name, .kinds = kinds, .required = required, .multiple = multiple};
}

// A port of spectral frames (samples.md §10.4).
constexpr portSpec framePort(std::string_view name) {
    return {.name = name, .kinds = eventPipes, .required = true, .multiple = false, .frames = true};
}

constexpr std::array audioIn = {port("in", audioPipes)};
constexpr std::array audioOut = {port("out", audioPipes)};
constexpr std::array frameIn = {framePort("in")};
constexpr std::array pvDelayIns = {framePort("in"), port("redraw", eventPipes, false)};
constexpr std::array frameOut = {framePort("out")};
constexpr std::array signalIn = {port("in", signalPipes)};
constexpr std::array bufferWriteIns = {port("in", audioPipes), port("reset", eventPipes, false)};
constexpr std::array bufferWriteOuts = {port("position", controlPipes, false)};
constexpr std::array samplePlayerIns = {port("trigger", eventPipes)};
constexpr std::array grainsIns = {port("trigger", eventPipes, false)};
constexpr std::array samplePlayerOuts = {port("out", audioPipes), port("done", eventPipes, false)};
constexpr std::array optionalSignalIn = {port("in", signalPipes, false)};
constexpr std::array signalOut = {port("out", signalPipes)};
constexpr std::array signalIns = {port("in", signalPipes, true, true)};
constexpr std::array controlOut = {port("out", controlPipes)};
constexpr std::array eventIn = {port("in", eventPipes)};
constexpr std::array eventOut = {port("out", eventPipes)};
constexpr std::array svfOuts = {port("out", audioPipes, false), port("low", audioPipes, false),
                                port("band", audioPipes, false), port("high", audioPipes, false),
                                port("notch", audioPipes, false)};
constexpr std::array adsrIns = {port("in", audioPipes, false), port("gate", eventPipes, false)};
constexpr std::array crossfadeIns = {port("a", signalPipes), port("b", signalPipes)};
constexpr std::array voiceOuts = {port("out", eventPipes, true, true)};
constexpr std::array triggerIn = {port("in", eventPipes)};
constexpr std::array sequencerOuts = {port("out", controlPipes, false), port("events", eventPipes, false)};
constexpr std::array holdIns = {port("trigger", eventPipes), port("in", signalPipes)};
constexpr std::array oscIns = {port("in", eventPipes, false), port("value", controlPipes, false)};
// Image generators read an input frame and draw over it (§7.7.1); without
// an input they draw over a transparent frame.
constexpr std::array videoIn = {port("in", videoPipes, false)};
constexpr std::array videoInRequired = {port("in", videoPipes)};
constexpr std::array videoOut = {port("out", videoPipes)};
constexpr std::array oscOuts = {port("out", eventPipes, false), port("value", controlPipes, false)};

constexpr std::span<const portSpec> noPorts = {};
constexpr std::span<const paramSpec> noParams = {};

// --- Constraints the builder relies on ----------------------------------

constexpr auto inPort = std::to_array<std::string_view>({"in"});
constexpr auto outPort = std::to_array<std::string_view>({"out"});
constexpr auto valuePort = std::to_array<std::string_view>({"value"});
constexpr auto inOut = std::to_array<std::string_view>({"in", "out"});
constexpr auto svfPorts = std::to_array<std::string_view>({"out", "low", "band", "high", "notch"});
constexpr auto oscInPorts = std::to_array<std::string_view>({"out", "value"});
constexpr auto oscOutPorts = std::to_array<std::string_view>({"in", "value"});

constexpr auto mixRules =
    std::to_array<constraint>({{.kind = rule::entryPerPipe, .param = "levels", .ports = inPort}});
constexpr auto sequencerRules = std::to_array<constraint>({{.kind = rule::notEmpty, .param = "values"}});
constexpr auto svfRules = std::to_array<constraint>({{.kind = rule::anyOutput, .ports = svfPorts}});
constexpr auto oscInRules = std::to_array<constraint>({{.kind = rule::anyOutput, .ports = oscInPorts}});
constexpr auto oscOutRules =
    std::to_array<constraint>({{.kind = rule::anyInput, .ports = oscOutPorts},
                               {.kind = rule::textWith, .param = "address", .ports = valuePort}});
constexpr auto panRules = std::to_array<constraint>({{.kind = rule::channels, .ports = outPort, .count = 2}});
constexpr auto wavetableRules =
    std::to_array<constraint>({{.kind = rule::powerOfTwo, .param = "table-size"}});
constexpr auto fftRules = std::to_array<constraint>(
    {{.kind = rule::powerOfTwo, .param = "size"}, {.kind = rule::powerOfTwo, .param = "overlap"}});
constexpr auto colorOutRules =
    std::to_array<constraint>({{.kind = rule::channels, .ports = outPort, .count = colorChannels}});
constexpr auto adsrRules = std::to_array<constraint>({{.kind = rule::audioWith, .ports = inOut}});

// --- Parameters per type ------------------------------------------------

constexpr std::array oscParams = {freq(), phase};
constexpr std::array pulseParams = {freq(), phase, num("width", 0.5, 0.0, 1.0)};
constexpr std::array noiseParams = {choice("color", noiseColors)};
constexpr std::array biquadParams = {choice("mode", biquadModes), freq("freq", 1000.0),
                                     num("q", 0.7071, 0.01, 100.0, controlPipes),
                                     qty("gain", dimension::level, 1.0, 0.0, 1e3, controlPipes)};
constexpr std::array svfParams = {choice("mode", svfModes), freq("cutoff", 1000.0),
                                  num("q", 0.7071, 0.01, 100.0)};
constexpr std::array ladderParams = {freq("cutoff", 1000.0), num("resonance", 0.0, 0.0, 1.0),
                                     qty("drive", dimension::level, 1.0, 0.0, 100.0)};
constexpr std::array adsrParams = {seconds("attack", 0.01, controlPipes), seconds("decay", 0.1, controlPipes),
                                   qty("sustain", dimension::level, 0.7, 0.0, 1.0, controlPipes),
                                   seconds("release", 0.2, controlPipes),
                                   qty("gain", dimension::level, 1.0, 0.0, 1e3, controlPipes)};
constexpr std::array lfoParams = {choice("shape", lfoShapes), freq("rate", 1.0), phase,
                                  num("depth", 1.0, -1e9, 1e9), num("offset", 0.0, -1e9, 1e9)};
constexpr std::array smootherParams = {seconds("time", 0.05, staticOnly), choice("curve", curves)};
constexpr std::array delayParams = {seconds("time", 0.25), seconds("max-time", 1.0, staticOnly),
                                    choice("interpolation", interpolations)};
constexpr std::array combParams = {seconds("time", 0.05), seconds("max-time", 1.0, staticOnly),
                                   num("feedback", 0.5, -1.0, 1.0), num("damping", 0.2, 0.0, 1.0)};
constexpr std::array allpassParams = {seconds("time", 0.005), seconds("max-time", 0.1, staticOnly),
                                      num("feedback", 0.5, -1.0, 1.0)};
constexpr std::array bufferWriteParams = {buffer(true), num("record", 1.0, 0.0, 1.0),
                                          num("keep", 0.0, 0.0, 1.0), choice("mode", writeModes)};
constexpr std::array bufferReadParams = {buffer(false),
                                         qty("position", dimension::time, 0.0, -maxTime, maxTime),
                                         choice("interpolation", readInterpolations)};
constexpr std::array samplePlayerParams = {buffer(false),
                                           num("rate", 1.0, -1e3, 1e3),
                                           num("root", 60.0, 0.0, 127.0, staticOnly),
                                           seconds("start", 0.0),
                                           choice("loop", loopModes),
                                           seconds("loop-start", 0.0),
                                           seconds("loop-end", 0.0),
                                           choice("interpolation", readInterpolations)};
// The file is relative to the project root (samples.md §18.1).
constexpr std::array bufferSaveParams = {buffer(false), requiredText("file")};
constexpr std::array grainsParams = {buffer(false),
                                     num("voices", 32.0, 1.0, 128.0, staticOnly),
                                     qty("density", dimension::frequency, 10.0, 0.0, 1000.0),
                                     qty("duration", dimension::time, 0.1, 0.001, 10.0),
                                     qty("position", dimension::time, 0.0, -maxTime, maxTime),
                                     seconds("jitter", 0.0),
                                     qty("pitch", dimension::interval, 0.0, -48.0, 48.0),
                                     qty("pitch-jitter", dimension::interval, 0.0, 0.0, 48.0),
                                     num("reverse", 0.0, 0.0, 1.0),
                                     num("spread", 0.0, 0.0, 1.0),
                                     choice("window", grainWindows)};
constexpr std::array fftParams = {num("size", 2048.0, 256.0, 16384.0, staticOnly),
                                  num("overlap", 4.0, 2.0, 4.0, staticOnly)};
constexpr std::array pvDelayParams = {qty("max-time", dimension::time, 4.0, 0.001, 60.0, staticOnly),
                                      seconds("time", 0.5),
                                      choice("shape", delayShapes),
                                      num("spread", 1.0, 0.0, 1.0),
                                      num("feedback", 0.0, -0.99, 0.99),
                                      num("mix", 1.0, 0.0, 1.0)};
constexpr std::array pvFreezeParams = {num("hold", 0.0, 0.0, 1.0),
                                       qty("fade", dimension::time, 0.05, 0.0, 10.0, staticOnly)};
constexpr std::array pvBlurParams = {num("amount", 0.5, 0.0, 1.0)};
constexpr std::array wavetableParams = {buffer(false), freq(), phase, num("position", 0.0, 0.0, 1.0),
                                        num("table-size", 2048.0, 256.0, 4096.0, staticOnly)};
constexpr std::array mixParams = {list("levels")};
constexpr std::array gainParams = {qty("amount", dimension::level, 1.0, -1e9, 1e9)};
constexpr std::array clipParams = {num("low", -1.0, -1e9, 1e9), num("high", 1.0, -1e9, 1e9)};
constexpr std::array panParams = {num("position", 0.0, -1.0, 1.0)};
constexpr std::array crossfadeParams = {num("position", 0.5, 0.0, 1.0), choice("law", laws)};
constexpr std::array operandParams = {num("b", 0.0, -1e9, 1e9)};
constexpr std::array beatsParams = {num("beats", 0.0, -1e9, 1e9, staticOnly)};
constexpr std::array divisorParams = {num("b", 1.0, -1e9, 1e9)};
constexpr std::array stepParams = {num("step", 1.0, 0.0, 1e9)};
constexpr std::array scaleParams = {num("in-low", 0.0, -1e9, 1e9), num("in-high", 1.0, -1e9, 1e9),
                                    num("out-low", 0.0, -1e9, 1e9), num("out-high", 1.0, -1e9, 1e9)};
constexpr std::array noteToFreqParams = {freq("reference", 440.0)};
constexpr std::array ccParams = {num("cc", 1.0, 0.0, 127.0, staticOnly), midiChannel};
constexpr std::array bendParams = {qty("range", dimension::interval, 2.0, 0.0, 96.0, staticOnly),
                                   midiChannel};
constexpr std::array filterParams = {choice("type", eventTypes), midiChannel,
                                     num("low", 0.0, 0.0, 127.0, staticOnly),
                                     num("high", 127.0, 0.0, 127.0, staticOnly), text("address", "")};
constexpr std::array notesParams = {num("pitch", 60.0, 0.0, 127.0, controlPipes),
                                    num("velocity", 0.8, 0.0, 1.0, controlPipes),
                                    seconds("length", 0.25, controlPipes)};
constexpr std::array metroParams = {qty("every", dimension::beats, 1.0, 1.0 / 64.0, 1e3, staticOnly)};
constexpr std::array sequencerParams = {list("values"), choice("order", orders)};
constexpr std::array chanceParams = {num("probability", 0.5, 0.0, 1.0, controlPipes)};
constexpr std::array thresholdParams = {num("level", 0.0, -1e9, 1e9, controlPipes),
                                        choice("direction", directions)};
constexpr std::array deviceChannelParams = {num("channel", 0.0, 0.0, 255.0, staticOnly)};
constexpr std::array audioOutParams = {num("channel", 0.0, 0.0, 255.0, staticOnly),
                                       qty("ceiling", dimension::level, 1.0, 0.0, 1.0, staticOnly)};
constexpr std::array midiParams = {text("device", ""), midiChannel};
constexpr std::array oscInParams = {num("port", 57120.0, 1.0, 65535.0, staticOnly), text("address", "")};
constexpr std::array oscOutParams = {num("port", 57120.0, 1.0, 65535.0, staticOnly),
                                     text("address", "/value")};

// Processing's default background: light gray.
constexpr std::array backgroundParams = {color("color", {0.8, 0.8, 0.8, 1.0})};
constexpr std::array pointParams = {pixels("x"), pixels("y"), stroke, strokeWeight, strokeCap};
constexpr std::array lineParams = {pixels("x1"), pixels("y1"), pixels("x2"), pixels("y2"),
                                   stroke,       strokeWeight, strokeCap};
constexpr std::array triangleParams = {pixels("x1"), pixels("y1"), pixels("x2"), pixels("y2"), pixels("x3"),
                                       pixels("y3"), fill,         stroke,       strokeWeight};
constexpr std::array quadParams = {pixels("x1"), pixels("y1"), pixels("x2"), pixels("y2"),
                                   pixels("x3"), pixels("y3"), pixels("x4"), pixels("y4"),
                                   fill,         stroke,       strokeWeight};
constexpr std::array arcParams = {pixels("x"),
                                  pixels("y"),
                                  extent("width", 100.0),
                                  extent("height", 100.0),
                                  radians("start", 0.0),
                                  radians("stop", std::numbers::pi),
                                  choice("mode", arcModes),
                                  fill,
                                  stroke,
                                  strokeWeight};
constexpr std::array bezierParams = {pixels("x1"),  pixels("y1"), pixels("cx1"), pixels("cy1"), pixels("cx2"),
                                     pixels("cy2"), pixels("x2"), pixels("y2"),  stroke,        strokeWeight};
constexpr std::array transformParams = {pixels("translate-x"), pixels("translate-y"), radians("rotate", 0.0),
                                        num("scale-x", 1.0, -1e6, 1e6), num("scale-y", 1.0, -1e6, 1e6)};
constexpr std::array lerpColorParams = {color("color-a", {0.0, 0.0, 0.0, 1.0}),
                                        color("color-b", {1.0, 1.0, 1.0, 1.0}), num("amount", 0.0, 0.0, 1.0)};
// width and height 0: the image's own size.
constexpr std::array imagePlayerParams = {text("image", ""), pixels("x"), pixels("y"), extent("width", 0.0),
                                          extent("height", 0.0)};
// A rect's or an ellipse's: a box placed by one of @p modes, then paint.
constexpr std::array<paramSpec, 8> boxParams(std::span<const std::string_view> modes) {
    return {pixels("x"),
            pixels("y"),
            extent("width", 100.0),
            extent("height", 100.0),
            choice("mode", modes),
            fill,
            stroke,
            strokeWeight};
}
constexpr auto rectParams = boxParams(rectModes);
constexpr auto ellipseParams = boxParams(ellipseModes);
constexpr std::array circleParams = {pixels("x"), pixels("y"), extent("diameter", 100.0),
                                     fill,        stroke,      strokeWeight};

// --- The table ----------------------------------------------------------

using enum role;

// A mapper operation: reads in, takes its operands as parameters.
constexpr primitiveSpec mapperOp(std::string_view type, operands dims,
                                 std::span<const paramSpec> params = noParams) {
    return {.type = type,
            .kind = generator,
            .params = params,
            .inputs = signalIn,
            .outputs = signalOut,
            .mapper = dims};
}

// An image generator: draws over its input frame into its output (§7.7.1).
constexpr primitiveSpec drawing(std::string_view type, std::span<const paramSpec> params) {
    return {.type = type, .kind = generator, .params = params, .inputs = videoIn, .outputs = videoOut};
}

constexpr auto table = std::to_array<primitiveSpec>({
    // Sources and sinks (§7.2, §7.3)
    // Device input channels to an audio pipe
    {.type = "audioIn",
     .kind = source,
     .params = deviceChannelParams,
     .inputs = noPorts,
     .outputs = audioOut},
    // An audio pipe to device output channels, limited to a ceiling
    {.type = "audioOut", .kind = sink, .params = audioOutParams, .inputs = audioIn, .outputs = noPorts},
    // MIDI messages from a device
    {.type = "midiIn", .kind = source, .params = midiParams, .inputs = noPorts, .outputs = eventOut},
    // MIDI events to a device
    {.type = "midiOut", .kind = sink, .params = midiParams, .inputs = eventIn, .outputs = noPorts},
    // OSC messages from a UDP port
    {.type = "oscIn",
     .kind = source,
     .params = oscInParams,
     .inputs = noPorts,
     .outputs = oscOuts,
     .constraints = oscInRules},
    // OSC messages to a UDP port
    {.type = "oscOut",
     .kind = sink,
     .params = oscOutParams,
     .inputs = oscIns,
     .outputs = noPorts,
     .constraints = oscOutRules},

    // Oscillators (§7.6)
    // Sine oscillator
    {.type = "sineOsc", .kind = generator, .params = oscParams, .inputs = noPorts, .outputs = audioOut},
    // Band-limited sawtooth oscillator
    {.type = "sawOsc", .kind = generator, .params = oscParams, .inputs = noPorts, .outputs = audioOut},
    // Band-limited pulse oscillator
    {.type = "pulseOsc", .kind = generator, .params = pulseParams, .inputs = noPorts, .outputs = audioOut},
    // Band-limited triangle oscillator
    {.type = "triangleOsc", .kind = generator, .params = oscParams, .inputs = noPorts, .outputs = audioOut},
    // White, pink, or brown noise
    {.type = "noise", .kind = generator, .params = noiseParams, .inputs = noPorts, .outputs = audioOut},

    // Filters
    // RBJ cookbook biquad filter
    {.type = "biquad", .kind = generator, .params = biquadParams, .inputs = audioIn, .outputs = audioOut},
    // Trapezoidal state-variable filter
    {.type = "svf",
     .kind = generator,
     .params = svfParams,
     .inputs = audioIn,
     .outputs = svfOuts,
     .constraints = svfRules},
    // Resonant 4-pole low-pass ladder filter
    {.type = "ladder", .kind = generator, .params = ladderParams, .inputs = audioIn, .outputs = audioOut},

    // Envelopes and modulation
    // Attack-decay-sustain-release envelope
    {.type = "adsr",
     .kind = generator,
     .params = adsrParams,
     .inputs = adsrIns,
     .outputs = signalOut,
     .constraints = adsrRules},
    // Low-frequency oscillator
    {.type = "lfo", .kind = generator, .params = lfoParams, .inputs = noPorts, .outputs = signalOut},
    // Smooths a signal over a time constant
    {.type = "smoother",
     .kind = generator,
     .params = smootherParams,
     .inputs = signalIn,
     .outputs = signalOut},

    // Delays
    // Interpolated delay line
    {.type = "delayLine", .kind = generator, .params = delayParams, .inputs = audioIn, .outputs = audioOut},
    // Feedback comb filter with damping
    {.type = "comb", .kind = generator, .params = combParams, .inputs = audioIn, .outputs = audioOut},
    // Schroeder allpass filter
    {.type = "allpass", .kind = generator, .params = allpassParams, .inputs = audioIn, .outputs = audioOut},

    // Buffers (samples.md §6)
    // Records into a buffer: replace, overdub, or hold (freeze)
    {.type = "bufferWrite",
     .kind = generator,
     .params = bufferWriteParams,
     .inputs = bufferWriteIns,
     .outputs = bufferWriteOuts},
    // Reads a buffer at a position
    {.type = "bufferRead",
     .kind = generator,
     .params = bufferReadParams,
     .inputs = noPorts,
     .outputs = audioOut},
    // Short windowed grains from a buffer, from a pool of voices (samples.md §8)
    {.type = "grains", .kind = generator, .params = grainsParams, .inputs = grainsIns, .outputs = audioOut},
    // Plays a buffer from a start position at each trigger (samples.md §7)
    {.type = "samplePlayer",
     .kind = generator,
     .params = samplePlayerParams,
     .inputs = samplePlayerIns,
     .outputs = samplePlayerOuts},

    // Writes a buffer to a WAV file at each trigger (samples.md §18)
    {.type = "bufferSave",
     .kind = sink,
     .params = bufferSaveParams,
     .inputs = samplePlayerIns,
     .outputs = noPorts},

    // A band-limited oscillator over tables in a buffer (samples.md §9)
    {.type = "wavetableOsc",
     .kind = generator,
     .params = wavetableParams,
     .inputs = noPorts,
     .outputs = audioOut,
     .constraints = wavetableRules},

    // Spectral (samples.md §10, §11)
    // Spectral frames of a signal, a frame every hop
    {.type = "fft",
     .kind = generator,
     .params = fftParams,
     .inputs = audioIn,
     .outputs = frameOut,
     .constraints = fftRules},
    // A signal resynthesized from spectral frames
    {.type = "ifft", .kind = generator, .params = noParams, .inputs = frameIn, .outputs = audioOut},
    // Each bin delayed by its own number of frames
    {.type = "pvDelay",
     .kind = generator,
     .params = pvDelayParams,
     .inputs = pvDelayIns,
     .outputs = frameOut},
    // A spectrum held, its phases advancing at each bin's frequency
    {.type = "pvFreeze", .kind = generator, .params = pvFreezeParams, .inputs = frameIn, .outputs = frameOut},
    // Each bin's magnitude smoothed over time
    {.type = "pvBlur", .kind = generator, .params = pvBlurParams, .inputs = frameIn, .outputs = frameOut},

    // Math and routing
    // Sum of the inputs
    {.type = "add", .kind = generator, .params = noParams, .inputs = signalIns, .outputs = signalOut},
    // Product of the inputs
    {.type = "multiply", .kind = generator, .params = noParams, .inputs = signalIns, .outputs = signalOut},
    // Sum of the inputs, each with a level
    {.type = "mix",
     .kind = generator,
     .params = mixParams,
     .inputs = signalIns,
     .outputs = signalOut,
     .constraints = mixRules},
    // Input times an amount
    {.type = "gain", .kind = generator, .params = gainParams, .inputs = signalIn, .outputs = signalOut},
    // Input limited to a range
    {.type = "clip", .kind = generator, .params = clipParams, .inputs = signalIn, .outputs = signalOut},
    // Equal-power mono to stereo panner
    {.type = "pan",
     .kind = generator,
     .params = panParams,
     .inputs = audioIn,
     .outputs = audioOut,
     .constraints = panRules},
    // Crossfade between two inputs
    {.type = "crossfade",
     .kind = generator,
     .params = crossfadeParams,
     .inputs = crossfadeIns,
     .outputs = signalOut},

    // Conversion (§7.4.5, §7.4.6)
    // Decibels to linear amplitude
    {.type = "dbToAmp", .kind = generator, .params = noParams, .inputs = signalIn, .outputs = signalOut},
    // Beats to seconds at the current tempo: the input, 0 when unbound,
    // plus a constant number of beats
    {.type = "beatsToSeconds",
     .kind = generator,
     .params = beatsParams,
     .inputs = optionalSignalIn,
     .outputs = signalOut},

    // Mapper operations (§7.9)
    // Negation
    mapperOp("neg", operands::sign),
    // Absolute value
    mapperOp("abs", operands::sign),
    // Natural exponential
    mapperOp("exp", operands::plain),
    // Natural logarithm
    mapperOp("log", operands::plain),
    // Sine
    mapperOp("sin", operands::plain),
    // Cosine
    mapperOp("cos", operands::plain),
    // Hyperbolic tangent
    mapperOp("tanh", operands::plain),
    // Largest integer not above the input
    mapperOp("floor", operands::keep),
    // Nearest integer
    mapperOp("round", operands::keep),
    // Input minus b; lowers + and -
    {.type = "subtract",
     .kind = generator,
     .params = operandParams,
     .inputs = signalIn,
     .outputs = signalOut},
    // Input divided by b; lowers /
    {.type = "divide", .kind = generator, .params = divisorParams, .inputs = signalIn, .outputs = signalOut},
    // Input to the power b
    mapperOp("pow", operands::plain, divisorParams),
    // Smaller of the input and b
    mapperOp("min", operands::same, operandParams),
    // Larger of the input and b
    mapperOp("max", operands::same, operandParams),
    // Input rounded to a multiple of step
    mapperOp("quantize", operands::same, stepParams),
    // Input limited to low and high
    mapperOp("clamp", operands::same, clipParams),
    // Linear map from one range to another
    mapperOp("scale", operands::ranges, scaleParams),
    // Exponential map from one range to another
    mapperOp("scaleExp", operands::ranges, scaleParams),

    // Event generators (§7.8)
    // Latest note to frequency
    {.type = "noteToFreq",
     .kind = generator,
     .params = noteToFreqParams,
     .inputs = eventIn,
     .outputs = controlOut},
    // Note on and off to 1 and 0
    {.type = "gate", .kind = generator, .params = noParams, .inputs = eventIn, .outputs = controlOut},
    // Velocity of the latest note-on, 0 to 1
    {.type = "velocity", .kind = generator, .params = noParams, .inputs = eventIn, .outputs = controlOut},
    // One controller number to a value, 0 to 1
    {.type = "controlChange",
     .kind = generator,
     .params = ccParams,
     .inputs = eventIn,
     .outputs = controlOut},
    // Pitch bend in semitones
    {.type = "pitchBend", .kind = generator, .params = bendParams, .inputs = eventIn, .outputs = controlOut},
    // Keeps matching events
    {.type = "eventFilter",
     .kind = generator,
     .params = filterParams,
     .inputs = eventIn,
     .outputs = eventOut},
    // Assigns notes to voices, stealing the oldest
    {.type = "voiceSplit", .kind = generator, .params = noParams, .inputs = eventIn, .outputs = voiceOuts},
    // A note for each trigger or value event: a note-on, then a note-off
    // after its length; a value event plays its number unless pitch is given
    {.type = "triggerNotes",
     .kind = generator,
     .params = notesParams,
     .inputs = triggerIn,
     .outputs = eventOut},
    // A trigger every N beats
    {.type = "metro", .kind = generator, .params = metroParams, .inputs = noPorts, .outputs = eventOut},
    // Steps through values on each trigger
    {.type = "sequencer",
     .kind = generator,
     .params = sequencerParams,
     .inputs = triggerIn,
     .outputs = sequencerOuts,
     .constraints = sequencerRules},
    // Passes each event with a probability
    {.type = "chance", .kind = generator, .params = chanceParams, .inputs = eventIn, .outputs = eventOut},
    // A trigger when the input crosses a level
    {.type = "threshold",
     .kind = generator,
     .params = thresholdParams,
     .inputs = signalIn,
     .outputs = eventOut},
    // Holds the input at each trigger
    {.type = "sampleAndHold",
     .kind = generator,
     .params = noParams,
     .inputs = holdIns,
     .outputs = controlOut},

    // Images (§7.7)
    // The input frame with a color composited over it
    drawing("background", backgroundParams),
    // A rectangle placed by its mode: corner (x, y, width, height), corners
    // (x, y to width, height), center, or radius (half sizes)
    drawing("rect", rectParams),
    // A dot of the stroke color and weight: round, or square for the other caps
    drawing("point", pointParams),
    // A line segment
    drawing("line", lineParams),
    // An ellipse placed by its mode: center (x, y, width, height), radius
    // (half sizes), corner, or corners
    drawing("ellipse", ellipseParams),
    // A circle of a diameter about its center
    drawing("circle", circleParams),
    // A triangle through three points
    drawing("triangle", triangleParams),
    // A quadrilateral through four points, in order
    drawing("quad", quadParams),
    // An arc of the ellipse about (x, y), from start to stop radians,
    // clockwise; open fills the chord region and strokes the curve only
    drawing("arc", arcParams),
    // A cubic Bezier curve from (x1, y1) through two controls to (x2, y2)
    drawing("bezier", bezierParams),
    // Translation, rotation, and scale for the drawing downstream, until the
    // next transform (§7.7.2)
    drawing("transform", transformParams),
    // A color between color-a and color-b, at amount (0 to 1), to a
    // 4-channel control pipe that other image parameters follow
    {.type = "lerpColor",
     .kind = generator,
     .params = lerpColorParams,
     .inputs = noPorts,
     .outputs = controlOut,
     .constraints = colorOutRules},
    // An image file drawn at (x, y), scaled to width × height (0: its own
    // size); the file loads when the plan is built (§7.7.6)
    drawing("imagePlayer", imagePlayerParams),
    // A frame to the server window: the visual layer
    {.type = "videoOut", .kind = sink, .params = noParams, .inputs = videoInRequired, .outputs = noPorts},
});

// The first item of @p items whose key, by @p key, is @p name.
template <typename T, typename Key>
optionalRef<T> byKey(std::span<const T> items, std::string_view name, Key key) noexcept {
    const auto it = std::ranges::find_if(items, [&](const T& x) { return key(x) == name; });
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

bufferAccess buffersOf(const nodeSpec& node) {
    bufferAccess out;
    const auto spec = findPrimitive(node.type);
    if (!spec) return out;
    for (const auto& p : spec->get().params) {
        if (p.dim != dimension::buffer) continue;
        const auto it = node.params.find(p.name);
        if (it == node.params.end()) continue;
        if (const auto b = as<bufferRef>(it->second)) (p.writes ? out.writes : out.reads).push_back(*b);
    }
    return out;
}

std::map<bufferRef, std::vector<std::size_t>> writersByBuffer(const graphSpec& graph) {
    std::map<bufferRef, std::vector<std::size_t>> writers;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        for (const auto b : buffersOf(graph.nodes[i]).writes) writers[b].push_back(i);
    }
    return writers;
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
    for (const paramSpec& p : spec.params) {
        if (!p.required) continue;
        if (!node.params.contains(p.name)) {
            out.push_back(say.param(p.name) + " is required");
        } else if (p.dim == dimension::text && text(p.name).empty()) {
            out.push_back(say.param(p.name) + " must not be empty");
        }
    }
    for (const constraint& c : spec.constraints) {
        switch (c.kind) {
        case rule::anyInput:
        case rule::anyOutput: {
            const bool input = c.kind == rule::anyInput;
            const auto& ports = input ? node.inputs : node.outputs;
            if (std::ranges::none_of(c.ports, [&](std::string_view p) { return bound(ports, p); })) {
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
            if (std::ranges::any_of(it->second, [](pipeRef r) { return r.kind != pipeKind::audio; })) {
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
        case rule::powerOfTwo: {
            const auto p = spec.param(c.param);
            double v = p ? p->get().defaultValue : 0.0;
            if (const auto it = node.params.find(c.param); it != node.params.end()) {
                const auto d = as<double>(it->second);
                if (!d) break;  // the parameter's own check reports it
                v = *d;
            }
            // NaN fails the first comparison; 2^62 bounds the integer cast.
            const bool integral = v >= 1.0 && v <= 0x1p62 && std::trunc(v) == v;
            const auto n = integral ? static_cast<std::uint64_t>(v) : 0;
            if (!integral || (n & (n - 1)) != 0)
                out.push_back(say.param(c.param) + " must be a power of two");
            break;
        }
        }
    }
    return out;
}

} // namespace metadata
