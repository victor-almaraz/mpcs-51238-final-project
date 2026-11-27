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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
    case dimension::buffer: return "buffer";
    case dimension::textList: return "text list";
    }
    return "number";
}

bool isNumeric(dimension d) noexcept {
    switch (d) {
    case dimension::choice:
    case dimension::text:
    case dimension::list:
    case dimension::buffer:
    case dimension::textList: return false;
    case dimension::number:
    case dimension::time:
    case dimension::frequency:
    case dimension::level:
    case dimension::interval:
    case dimension::beats: return true;
    }
    return false;
}

namespace {

constexpr double maxFreq = 1e5;
constexpr double maxTime = 3600.0;

// --- Parameter builders -------------------------------------------------

// The builders here and below are consteval: they run only in the compiler,
// and every table they build is a constant of the program.

consteval paramSpec num(std::string_view name, double def, double lo, double hi, kindSet bind = signalPipes) {
    return {.name = name, .dim = dimension::number, .defaultValue = def, .min = lo, .max = hi,
            .bindable = bind};
}
consteval paramSpec qty(std::string_view name, dimension dim, double def, double lo, double hi,
                        kindSet bind = signalPipes) {
    return {.name = name, .dim = dim, .defaultValue = def, .min = lo, .max = hi, .bindable = bind};
}
consteval paramSpec freq(std::string_view name = "freq", double def = 440.0) {
    return qty(name, dimension::frequency, def, 0.0, maxFreq);
}
consteval paramSpec seconds(std::string_view name, double def, kindSet bind = signalPipes) {
    return qty(name, dimension::time, def, 0.0, maxTime, bind);
}
consteval paramSpec choice(std::string_view name, std::span<const std::string_view> names) {
    return {.name = name, .dim = dimension::choice, .choices = names, .defaultText = names.front()};
}
consteval paramSpec text(std::string_view name, std::string_view def) {
    return {.name = name, .dim = dimension::text, .defaultText = def};
}
// A text parameter every node must give, not empty.
consteval paramSpec requiredText(std::string_view name) {
    return {.name = name, .dim = dimension::text, .required = true};
}
consteval paramSpec list(std::string_view name) { return {.name = name, .dim = dimension::list}; }
consteval paramSpec textList(std::string_view name) { return {.name = name, .dim = dimension::textList}; }
// A buffer the node reads, or writes when @p writes (samples.md §12.1).
consteval paramSpec buffer(bool writes) {
    return {.name = "buffer", .dim = dimension::buffer, .writes = writes, .required = true};
}
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

// --- Ports --------------------------------------------------------------

consteval portSpec port(std::string_view name, kindSet kinds, bool required = true, bool multiple = false) {
    return {.name = name, .kinds = kinds, .required = required, .multiple = multiple};
}

// A port of spectral frames (samples.md §10.4).
consteval portSpec framePort(std::string_view name) {
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
constexpr std::array clipPlayerOuts = {port("out", eventPipes), port("playing", controlPipes, false),
                                       port("queued", controlPipes, false)};
constexpr std::array oscIns = {port("in", eventPipes, false), port("value", controlPipes, false)};
constexpr std::array oscOuts = {port("out", eventPipes, false), port("value", controlPipes, false)};
constexpr std::array serialOutIns = {port("in", eventPipes), port("value", controlPipes, false)};
constexpr std::array webOutOuts = {port("dropped", controlPipes, false)};

constexpr std::span<const portSpec> noPorts = {};
constexpr std::span<const paramSpec> noParams = {};

// --- Constraints the builder relies on ----------------------------------

constexpr auto inPort = std::to_array<std::string_view>({"in"});
constexpr auto outPort = std::to_array<std::string_view>({"out"});
constexpr auto valuePort = std::to_array<std::string_view>({"value"});
constexpr auto inOut = std::to_array<std::string_view>({"in", "out"});
constexpr auto svfPorts = std::to_array<std::string_view>({"out", "low", "band", "high", "notch"});
constexpr auto outOrValue = std::to_array<std::string_view>({"out", "value"});
constexpr auto inOrValue = std::to_array<std::string_view>({"in", "value"});
constexpr auto httpSchemes = std::to_array<std::string_view>({"http://", "https://"});
constexpr auto wsSchemes = std::to_array<std::string_view>({"ws://", "wss://"});
constexpr auto httpMethods = std::to_array<std::string_view>({"post", "put", "get"});
// Serial speeds (io.md §4.2).
constexpr auto bauds = std::to_array<double>({9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600});

constexpr auto mixRules =
    std::to_array<constraint>({{.kind = rule::entryPerPipe, .param = "levels", .ports = inPort}});
constexpr auto sequencerRules = std::to_array<constraint>({{.kind = rule::notEmpty, .param = "values"}});
constexpr auto svfRules = std::to_array<constraint>({{.kind = rule::anyOutput, .ports = svfPorts}});
constexpr auto oscInRules = std::to_array<constraint>({{.kind = rule::anyOutput, .ports = outOrValue}});
constexpr auto oscOutRules =
    std::to_array<constraint>({{.kind = rule::anyInput, .ports = inOrValue},
                               {.kind = rule::textWith, .param = "address", .ports = valuePort}});
constexpr constraint baudRule = {.kind = rule::oneOf, .param = "baud", .values = bauds};
constexpr auto serialInRules =
    std::to_array<constraint>({{.kind = rule::anyOutput, .ports = outOrValue}, baudRule});
constexpr auto serialOutRules = std::to_array<constraint>({baudRule});
// A field path per :value channel, at most one per channel a pipe has
// (io.md §5.1, §9.3).
constexpr constraint fieldCount = {.kind = rule::atMost, .param = "fields", .count = maxChannels};
constexpr constraint fieldPerChannel = {.kind = rule::entryPerChannel, .param = "fields", .ports = valuePort};
constexpr constraint headerRule = {.kind = rule::headers, .param = "headers"};
constexpr auto httpSourceRules =
    std::to_array<constraint>({{.kind = rule::anyOutput, .ports = outOrValue},
                               {.kind = rule::scheme, .param = "url", .names = httpSchemes},
                               fieldCount,
                               fieldPerChannel,
                               headerRule});
constexpr auto socketSourceRules =
    std::to_array<constraint>({{.kind = rule::anyOutput, .ports = outOrValue},
                               {.kind = rule::scheme, .param = "url", .names = wsSchemes},
                               fieldCount,
                               fieldPerChannel,
                               headerRule});
// A key per :value channel names each value in the body (io.md §7.2).
constexpr auto webOutRules =
    std::to_array<constraint>({{.kind = rule::scheme, .param = "url", .names = httpSchemes},
                               fieldCount,
                               fieldPerChannel,
                               {.kind = rule::textWith, .param = "fields", .ports = valuePort},
                               headerRule});
constexpr auto panRules = std::to_array<constraint>({{.kind = rule::channels, .ports = outPort, .count = 2}});
constexpr auto wavetableRules =
    std::to_array<constraint>({{.kind = rule::powerOfTwo, .param = "table-size"}});
constexpr auto fftRules = std::to_array<constraint>(
    {{.kind = rule::powerOfTwo, .param = "size"}, {.kind = rule::powerOfTwo, .param = "overlap"}});
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
constexpr std::array oscNotesParams = {text("address", "/note")};
constexpr std::array filterParams = {choice("type", eventTypes), midiChannel,
                                     num("low", 0.0, 0.0, 127.0, staticOnly),
                                     num("high", 127.0, 0.0, 127.0, staticOnly), text("address", "")};
constexpr std::array notesParams = {num("pitch", 60.0, 0.0, 127.0, controlPipes),
                                    num("velocity", 0.8, 0.0, 1.0, controlPipes),
                                    seconds("length", 0.25, controlPipes)};
constexpr std::array metroParams = {qty("every", dimension::beats, 1.0, 1.0 / 64.0, 1e3, staticOnly)};
constexpr std::array sequencerParams = {list("values"), choice("order", orders)};
constexpr std::array clipPlayerParams = {requiredText("bank"), num("slots", 8.0, 1.0, 64.0, staticOnly),
                                         qty("grid", dimension::beats, 16.0, 1.0 / 64.0, 1e3, staticOnly),
                                         num("select", -1.0, -1.0, 63.0, controlPipes)};
constexpr std::array chanceParams = {num("probability", 0.5, 0.0, 1.0, controlPipes)};
constexpr std::array thresholdParams = {num("level", 0.0, -1e9, 1e9, controlPipes),
                                        choice("direction", directions)};
constexpr std::array deviceChannelParams = {num("channel", 0.0, 0.0, 255.0, staticOnly)};
constexpr std::array audioOutParams = {num("channel", 0.0, 0.0, 255.0, staticOnly),
                                       qty("ceiling", dimension::level, 1.0, 0.0, 1.0, staticOnly)};
constexpr std::array midiParams = {text("device", ""), midiChannel};
// A UDP port, or :page for the project's page port (io.md §12.3).
constexpr paramSpec oscPort = {.name = "port",
                               .dim = dimension::number,
                               .defaultValue = 57120.0,
                               .min = 1.0,
                               .max = 65535.0,
                               .bindable = staticOnly,
                               .pagePort = true};
// The value an osc-in writes before its first message (a page's control at its default), in any unit.
constexpr paramSpec oscInitial = {.name = "initial", .dim = dimension::number, .anyUnit = true};
constexpr std::array oscInParams = {oscPort, text("address", ""), oscInitial};
constexpr std::array oscOutParams = {oscPort, text("address", "/value")};
// Polls a URL (io.md §5.1); intervals are wall-clock seconds, so static.
constexpr std::array webInParams = {
    requiredText("url"), qty("every", dimension::time, 10.0, 0.1, maxTime, staticOnly), textList("fields"),
    textList("headers"), qty("timeout", dimension::time, 5.0, 0.1, maxTime, staticOnly)};
// A stream held open (io.md §6.1, §6.4).
constexpr std::array webStreamParams = {requiredText("url"), textList("headers"), textList("fields"),
                                        text("event", "")};
constexpr std::array webSocketParams = {requiredText("url"), textList("headers"), textList("fields"),
                                        text("hello", "")};
// A request per event (io.md §7.1).
constexpr std::array webOutParams = {requiredText("url"), textList("headers"),
                                     qty("timeout", dimension::time, 5.0, 0.1, maxTime, staticOnly),
                                     choice("method", httpMethods), textList("fields")};
// A device path or pattern, and its speed (io.md §4.1, §4.2).
constexpr std::array serialParams = {text("device", ""), num("baud", 115200.0, 9600.0, 921600.0, staticOnly)};

// --- The table ----------------------------------------------------------

using enum role;

// A mapper operation: reads in, takes its operands as parameters.
consteval primitiveSpec mapperOp(std::string_view type, operands dims,
                                 std::span<const paramSpec> params = noParams) {
    return {.type = type,
            .kind = generator,
            .params = params,
            .inputs = signalIn,
            .outputs = signalOut,
            .mapper = dims};
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
    // Lines of numbers from a serial device (io.md §4.7)
    {.type = "serialIn",
     .kind = source,
     .params = serialParams,
     .inputs = noPorts,
     .outputs = oscOuts,
     .constraints = serialInRules},
    // Lines of numbers to a serial device, one per event (io.md §4.8)
    {.type = "serialOut",
     .kind = sink,
     .params = serialParams,
     .inputs = serialOutIns,
     .outputs = noPorts,
     .constraints = serialOutRules},
    // Numbers from a JSON document fetched every interval (io.md §5)
    {.type = "webIn",
     .kind = source,
     .params = webInParams,
     .inputs = noPorts,
     .outputs = oscOuts,
     .constraints = httpSourceRules},
    // Numbers from the messages of a server-sent event stream (io.md §6.1)
    {.type = "webStream",
     .kind = source,
     .params = webStreamParams,
     .inputs = noPorts,
     .outputs = oscOuts,
     .constraints = httpSourceRules},
    // Numbers from the messages of a WebSocket (io.md §6.4)
    {.type = "webSocket",
     .kind = source,
     .params = webSocketParams,
     .inputs = noPorts,
     .outputs = oscOuts,
     .constraints = socketSourceRules},
    // An HTTP request per event, carrying values as JSON (io.md §7)
    {.type = "webOut",
     .kind = sink,
     .params = webOutParams,
     .inputs = serialOutIns,
     .outputs = webOutOuts,
     .constraints = webOutRules},

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
    // Note-ons and note-offs from OSC messages [address, note, velocity,
    // channel] (io.md §12.10)
    {.type = "oscNotes", .kind = generator, .params = oscNotesParams, .inputs = eventIn, .outputs = eventOut},
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
    // Plays a bank's clips, changing clip on a grid line (canvas.md §5.2)
    {.type = "clipPlayer",
     .kind = generator,
     .params = clipPlayerParams,
     .inputs = noPorts,
     .outputs = clipPlayerOuts},
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
    // A text list parameter; empty when absent or not a text list.
    auto texts = [&](std::string_view name) -> std::span<const std::string> {
        const auto it = node.params.find(name);
        if (it == node.params.end()) return {};
        const auto l = as<std::vector<std::string>>(it->second);
        return l ? std::span<const std::string>(l->get()) : std::span<const std::string>();
    };
    // A numeric parameter, given or by default; nothing when it is given
    // but not a number.
    auto number = [&](std::string_view name) -> std::optional<double> {
        if (const auto it = node.params.find(name); it != node.params.end()) {
            const auto d = as<double>(it->second);
            return d ? std::optional(d->get()) : std::nullopt;
        }
        const auto p = spec.param(name);
        return p ? p->get().defaultValue : 0.0;
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
            if (bound(node.inputs, c.ports.front()) && text(c.param).empty() && texts(c.param).empty()) {
                out.push_back("with " + say.port(c.ports.front(), true) + ", " + say.param(c.param) +
                              " must not be empty");
            }
            break;
        case rule::powerOfTwo: {
            const auto v = number(c.param);
            if (!v) break;  // the parameter's own check reports it
            // NaN fails the first comparison; 2^62 bounds the integer cast.
            const bool integral = *v >= 1.0 && *v <= 0x1p62 && std::trunc(*v) == *v;
            const auto n = integral ? static_cast<std::uint64_t>(*v) : 0;
            if (!integral || (n & (n - 1)) != 0)
                out.push_back(say.param(c.param) + " must be a power of two");
            break;
        }
        case rule::scheme: {
            const auto t = text(c.param);
            if (t.empty() ||
                std::ranges::any_of(c.names, [&](std::string_view s) { return t.starts_with(s); }))
                break;
            out.push_back(say.param(c.param) + " must start with " + joinNames(c.names));
            break;
        }
        case rule::atMost:
            if (const auto n = texts(c.param).size(); std::cmp_greater(n, c.count)) {
                out.push_back(say.param(c.param) + " has " + count(n, "entry", "entries") + "; at most " +
                              std::to_string(c.count) + " are allowed");
            }
            break;
        case rule::headers:
            for (const auto& h : texts(c.param)) {
                const auto colon = h.find(": ");
                if (colon == 0 || colon == std::string::npos) {
                    out.push_back(say.param(c.param) + " entry \"" + h + "\" must be a header, Name: value");
                }
            }
            break;
        case rule::entryPerChannel: {
            const auto entries = texts(c.param).size();
            const bool input = bound(node.inputs, c.ports.front());
            const auto& ports = input ? node.inputs : node.outputs;
            const auto it = ports.find(c.ports.front());
            if (entries == 0 || it == ports.end() || it->second.empty()) break;
            const auto d = findPipe(graph, it->second.front());
            if (d && std::cmp_not_equal(d->get().channels, entries)) {
                out.push_back(say.param(c.param) + " has " + count(entries, "entry", "entries") + " for " +
                              count(static_cast<std::size_t>(d->get().channels), "channel", "channels") +
                              " on " + say.port(c.ports.front(), input));
            }
            break;
        }
        case rule::oneOf: {
            const auto v = number(c.param);
            if (!v || std::ranges::find(c.values, *v) != c.values.end()) break;
            std::string accepted;
            for (const double x : c.values) accepted += (accepted.empty() ? "" : ", ") + formatNumber(x);
            out.push_back(say.param(c.param) + " must be one of " + accepted);
            break;
        }
        }
    }
    return out;
}

} // namespace metadata
