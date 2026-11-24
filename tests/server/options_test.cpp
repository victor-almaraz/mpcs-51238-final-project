// The server's command-line options: defaults, every option, every
// refusal and its message, --help and --version, and the engine options
// they give.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <espresso/primitives/engine.h>
#include <espresso/protocol/messages.h>

#include "cores.h"
#include "options.h"

using namespace std::string_view_literals;

namespace {

constexpr std::string_view version = "espresso-server 1.2.3 (metadata 4, JUCE v5.6.7)";

// What parse() did with one command line.
struct parsed {
    std::optional<int> code;
    server::options o;
    std::string out;
    std::string errors;
};

// Parses the program's name followed by @p rest.
parsed parseArgs(std::initializer_list<std::string_view> rest) {
    std::vector<std::string_view> args{"espresso-server"};
    args.insert(args.end(), rest);
    parsed p;
    std::ostringstream out;
    std::ostringstream errors;
    p.code = server::parse(args, p.o, version, out, errors);
    p.out = out.str();
    p.errors = errors.str();
    return p;
}

std::string usageText() {
    std::ostringstream s;
    server::usage(s);
    return s.str();
}

// The refusal of @p argument: its line, then the usage.
std::string refusal(std::string_view argument) {
    return "espresso-server: invalid argument " + std::string(argument) + "\n" + usageText();
}

// A number option, the ends of its range, a value just beyond each end,
// and the field it sets.
struct numberOption {
    std::string_view name;
    std::string_view lo;
    std::string_view hi;
    std::string_view below;
    std::string_view above;
    bool integer = true;
    std::function<double(const server::options&)> field;
};

std::vector<numberOption> numberOptions() {
    return {
        {.name = "--port",
         .lo = "1",
         .hi = "65535",
         .below = "0",
         .above = "65536",
         .integer = true,
         .field = [](const auto& o) { return static_cast<double>(o.port); }},
        {.name = "--sample-rate",
         .lo = "8000",
         .hi = "384000",
         .below = "7999.5",
         .above = "384000.5",
         .integer = false,
         .field = [](const auto& o) { return o.sampleRate; }},
        {.name = "--block-size",
         .lo = "16",
         .hi = "8192",
         .below = "15",
         .above = "8193",
         .integer = true,
         .field = [](const auto& o) { return static_cast<double>(o.blockSize); }},
        {.name = "--inputs",
         .lo = "0",
         .hi = "64",
         .below = "-1",
         .above = "65",
         .integer = true,
         .field = [](const auto& o) { return static_cast<double>(o.inputs); }},
        {.name = "--outputs",
         .lo = "0",
         .hi = "64",
         .below = "-1",
         .above = "65",
         .integer = true,
         .field = [](const auto& o) { return static_cast<double>(o.outputs); }},
        {.name = "--seconds",
         .lo = "0",
         .hi = "86400",
         .below = "-0.5",
         .above = "86400.5",
         .integer = false,
         .field = [](const auto& o) { return o.seconds; }},
        {.name = "--buffer-memory",
         .lo = "1",
         .hi = "1048576",
         .below = "0",
         .above = "1048577",
         .integer = true,
         .field = [](const auto& o) { return static_cast<double>(o.bufferMemory); }},
        {.name = "--threads",
         .lo = "1",
         .hi = "64",
         .below = "0",
         .above = "65",
         .integer = true,
         .field = [](const auto& o) { return static_cast<double>(o.threads); }},
        {.name = "--worker-spin",
         .lo = "0",
         .hi = "1000000",
         .below = "-1",
         .above = "1000001",
         .integer = true,
         .field = [](const auto& o) { return static_cast<double>(o.workerSpin); }},
    };
}

// The options that take a value: the number options and the text options.
constexpr std::array valueOptions = {
    "--port"sv,    "--sample-rate"sv,   "--block-size"sv, "--inputs"sv,      "--outputs"sv,
    "--seconds"sv, "--buffer-memory"sv, "--threads"sv,    "--worker-spin"sv, "--device"sv,
    "--render"sv,  "--out"sv,           "--play"sv,       "--root"sv,
};

}  // namespace

TEST_CASE("the server's options default to a live engine on the default port") {
    const auto p = parseArgs({});
    CHECK_FALSE(p.code);
    CHECK(p.out.empty());
    CHECK(p.errors.empty());
    const auto& o = p.o;
    CHECK(o.port == protocol::defaultServerPort);
    CHECK(o.port == 57190);
    CHECK(o.device.empty());
    CHECK(o.sampleRate == 48000.0);
    CHECK(o.blockSize == 256);
    CHECK(o.inputs == 2);
    CHECK(o.outputs == 2);
    CHECK_FALSE(o.noAudio);
    CHECK_FALSE(o.pageLan);
    CHECK_FALSE(o.verbose);
    CHECK_FALSE(o.listDevices);
    CHECK(o.render.empty());
    CHECK(o.play.empty());
    CHECK(o.out.empty());
    CHECK(o.seconds == 10.0);
    CHECK_FALSE(o.profile);
    CHECK(o.root == ".");
    CHECK(o.bufferMemory == 1024);
    CHECK(o.threads == server::defaultThreads());
    CHECK(o.workerSpin == primitives::defaultWorkerSpin.count());
}

TEST_CASE("every option sets its value") {
    const auto device = parseArgs({"--port", "9000", "--device", "Built-in Output", "--sample-rate", "44100",
                                   "--block-size", "512", "--inputs", "0", "--outputs", "8", "--no-audio"});
    const auto engine =
        parseArgs({"--page-lan", "--verbose", "--list-devices", "--play", "piece.ir.json", "--root",
                   "project", "--buffer-memory", "64", "--threads", "3", "--worker-spin", "200"});
    const auto render =
        parseArgs({"--render", "in.ir.json", "--out", "out.wav", "--seconds", "2.5", "--profile"});
    for (const auto& p : {device, engine, render}) {
        CHECK_FALSE(p.code);
        CHECK(p.out.empty());
        CHECK(p.errors.empty());
    }
    CHECK(device.o.port == 9000);
    CHECK(device.o.device == "Built-in Output");
    CHECK(device.o.sampleRate == 44100.0);
    CHECK(device.o.blockSize == 512);
    CHECK(device.o.inputs == 0);
    CHECK(device.o.outputs == 8);
    CHECK(device.o.noAudio);
    CHECK(engine.o.pageLan);
    CHECK(engine.o.verbose);
    CHECK(engine.o.listDevices);
    CHECK(engine.o.play == "piece.ir.json");
    CHECK(engine.o.root == "project");
    CHECK(engine.o.bufferMemory == 64);
    CHECK(engine.o.threads == 3);
    CHECK(engine.o.workerSpin == 200);
    CHECK(render.o.render == "in.ir.json");
    CHECK(render.o.out == "out.wav");
    CHECK(render.o.seconds == 2.5);
    CHECK(render.o.profile);
}

TEST_CASE("the server has no video options") {
    for (const auto option : {"--no-video", "--frames", "--fps", "--width", "--height"}) {
        INFO(option);
        CHECK(parseArgs({option, "1"}).code == 2);
    }
}

TEST_CASE("an option given twice keeps its last value") {
    const auto p = parseArgs({"--port", "9000", "--port", "9001", "--root", "a", "--root", "b"});
    CHECK_FALSE(p.code);
    CHECK(p.o.port == 9001);
    CHECK(p.o.root == "b");
}

TEST_CASE("each number option takes the ends of its range and refuses values beyond them") {
    for (const auto& n : numberOptions()) {
        INFO(n.name);
        for (const auto end : {n.lo, n.hi}) {
            const auto p = parseArgs({n.name, end});
            CHECK_FALSE(p.code);
            CHECK(p.errors.empty());
            CHECK(n.field(p.o) == std::stod(std::string(end)));
        }
        for (const auto beyond : {n.below, n.above}) {
            const auto p = parseArgs({n.name, beyond});
            CHECK(p.code == 2);
            CHECK(p.out.empty());
            CHECK(p.errors == refusal(std::string(n.name) + " " + std::string(beyond)));
        }
    }
}

TEST_CASE("number options refuse text and values that are not finite") {
    for (const auto& n : numberOptions()) {
        INFO(n.name);
        for (const std::string_view bad : {"nan", "inf", "-inf", "1x", "abc", "0x10", " 1", ""}) {
            INFO('"' << bad << '"');
            const auto p = parseArgs({n.name, bad});
            CHECK(p.code == 2);
            CHECK(p.errors == refusal(std::string(n.name) + " " + std::string(bad)));
        }
        // An integer option takes integers only; the others take fractions.
        const std::string half = std::string(n.lo) + ".5";
        const auto fraction = parseArgs({n.name, half});
        if (n.integer) {
            CHECK(fraction.code == 2);
            CHECK(fraction.errors == refusal(std::string(n.name) + " " + half));
        } else {
            CHECK_FALSE(fraction.code);
            CHECK(n.field(fraction.o) == std::stod(half));
        }
    }
}

TEST_CASE("an option without its value is refused with the usage") {
    for (const std::string_view name : valueOptions) {
        INFO(name);
        const auto p = parseArgs({name});
        CHECK(p.code == 2);
        CHECK(p.out.empty());
        CHECK(p.errors == refusal(name));
    }
}

TEST_CASE("an unknown argument is refused with the usage") {
    for (const std::string_view bad : {"--bogus", "piece.ir.json", "-p", "--Port", "--port=9000", ""}) {
        INFO('"' << bad << '"');
        const auto p = parseArgs({bad});
        CHECK(p.code == 2);
        CHECK(p.out.empty());
        CHECK(p.errors == refusal(bad));
    }
}

TEST_CASE("parsing stops at the first refusal") {
    CHECK(parseArgs({"--bogus", "--port", "0"}).errors == refusal("--bogus"));
    CHECK(parseArgs({"--port", "0", "--bogus"}).errors == refusal("--port 0"));
    // Options before the refusal are read.
    const auto p = parseArgs({"--threads", "3", "--buffer-memory", "0"});
    CHECK(p.code == 2);
    CHECK(p.o.threads == 3);
    CHECK(p.o.bufferMemory == 1024);
}

TEST_CASE("a render needs an output file") {
    const auto missing = parseArgs({"--render", "in.ir.json"});
    CHECK(missing.code == 2);
    CHECK(missing.out.empty());
    CHECK(missing.errors == "espresso-server: --render needs --out\n");  // without the usage
    CHECK_FALSE(parseArgs({"--render", "in.ir.json", "--out", "out.wav"}).code);
    CHECK_FALSE(parseArgs({"--out", "out.wav", "--render", "in.ir.json"}).code);
    CHECK_FALSE(parseArgs({"--out", "out.wav"}).code);                   // read only by a render
    CHECK_FALSE(parseArgs({"--render", "-", "--out", "out.wav"}).code);  // IR from standard input
}

TEST_CASE("help writes the usage to standard output and stops") {
    for (const std::string_view help : {"--help", "-h"}) {
        INFO(help);
        const auto p = parseArgs({help});
        CHECK(p.code == 0);
        CHECK(p.out == usageText());
        CHECK(p.errors.empty());
    }
    // Arguments after it are not read; a refusal before it stops first.
    CHECK(parseArgs({"--help", "--bogus"}).code == 0);
    CHECK(parseArgs({"--help", "--render", "in.ir.json"}).code == 0);
    CHECK(parseArgs({"--bogus", "--help"}).code == 2);
}

TEST_CASE("version writes the version line to standard output and stops") {
    const auto p = parseArgs({"--version"});
    CHECK(p.code == 0);
    CHECK(p.out == std::string(version) + "\n");
    CHECK(p.errors.empty());
    CHECK(parseArgs({"--version", "--bogus"}).code == 0);
    CHECK(parseArgs({"--version", "--help"}).out == std::string(version) + "\n");
    CHECK(parseArgs({"--port", "0", "--version"}).code == 2);
}

TEST_CASE("the usage names every option and the defaults it states") {
    const auto text = usageText();
    CHECK(text.starts_with("usage: espresso-server [--port N] [--device NAME]"));
    CHECK(text.ends_with("An IR_FILE of - is read from standard input.\n"));
    for (const std::string_view name : valueOptions) {
        INFO(name);
        CHECK(text.find(name) != std::string::npos);
    }
    for (const std::string_view flag :
         {"--no-audio", "--page-lan", "--verbose", "--list-devices", "--profile"}) {
        INFO(flag);
        CHECK(text.find(flag) != std::string::npos);
    }
    CHECK(text.find("127.0.0.1:N (default 57190).") != std::string::npos);
    CHECK(text.find("(1 to 64;") != std::string::npos);
    CHECK(text.find("at most 16, here " + std::to_string(server::defaultThreads()) + ");") !=
          std::string::npos);
    CHECK(text.find("microseconds (default 50):") != std::string::npos);
}

TEST_CASE("the engine options follow the server's options") {
    server::options o;
    o.bufferMemory = 64;
    o.profile = true;
    o.threads = 3;
    o.workerSpin = 200;
    o.render = "in.ir.json";
    const auto e = server::engineOptionsFor(o, 44100.0, 128);
    CHECK(e.audio.sampleRate == 44100.0);
    CHECK(e.audio.blockSize == 128);
    CHECK(e.bufferMemory == std::size_t{64} * 1024 * 1024);
    CHECK(e.profile);
    CHECK(e.threads == 3);
    CHECK(e.workerSpin == std::chrono::microseconds(200));
    CHECK(static_cast<bool>(e.loadSample));
    CHECK_FALSE(static_cast<bool>(e.workerStart));  // an offline render's workers keep their priority
}

TEST_CASE("a live engine's workers ask for real-time priority") {
    const server::options o;
    const auto e = server::engineOptionsFor(o, 48000.0, 256);
    CHECK(static_cast<bool>(e.workerStart));  // not called here: it would change this thread's scheduling
    CHECK(e.threads == server::defaultThreads());
    CHECK(e.bufferMemory == std::size_t{1024} * 1024 * 1024);
    CHECK(e.workerSpin == primitives::defaultWorkerSpin);
    CHECK_FALSE(e.profile);
}
