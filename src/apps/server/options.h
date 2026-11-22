// The server's command-line options: their defaults, the usage text, the
// parser, and the engine options they give. Has no JUCE dependency, so the
// server's tests link it.

#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

#include <espresso/primitives/engine.h>
#include <espresso/protocol/messages.h>

#include "cores.h"

namespace server {

/// The server's options, each at its default until parse() reads it.
struct options {
    std::uint16_t port = protocol::defaultServerPort;  ///< `--port`: the protocol's UDP port
    std::string device;                                ///< `--device`: the audio device's name
    double sampleRate = 48000.0;                       ///< `--sample-rate`, in hertz
    int blockSize = 256;                               ///< `--block-size`, in samples
    int inputs = 2;                                    ///< `--inputs`: device input channels
    int outputs = 2;                                   ///< `--outputs`: output channels
    bool noAudio = false;                              ///< `--no-audio`: the engine runs on a timer
    bool noVideo = false;                              ///< `--no-video`: no window shows the visual layer
    bool pageLan = false;      ///< `--page-lan`: the page listener binds every interface (io.md §12.8)
    bool verbose = false;      ///< `--verbose`: protocol traffic and OSC output on standard output
    bool listDevices = false;  ///< `--list-devices`
    std::string render;        ///< `--render`: the IR file to render offline; empty: serve
    std::string play;          ///< `--play`: the IR file to play at startup; empty: none
    std::string out;           ///< `--out`: the WAVE file a render writes
    double seconds = 10.0;     ///< `--seconds`: a render's length
    std::string frames;        ///< `--frames`: a render's frame directory; empty: none
    double fps = 60.0;         ///< `--fps`: a render's frames per second
    bool profile = false;      ///< `--profile`: a render prints where the nodes' time went
    int width = 1280;          ///< `--width`: the frame width, in pixels
    int height = 720;          ///< `--height`: the frame height, in pixels
    /// `--root`: the project root, against which image and sample paths
    /// resolve.
    std::string root = ".";
    /// `--buffer-memory`: the bound on live buffers, in MiB (samples.md
    /// §5.6).
    int bufferMemory = 1024;
    /// `--threads`: the threads that run a block (threads.md §7.1).
    int threads = defaultThreads();
    /// `--worker-spin`, in microseconds (threads.md §6.5).
    int workerSpin = static_cast<int>(primitives::defaultWorkerSpin.count());
};

/// Writes the usage text to @p out.
void usage(std::ostream& out);

/// Reads @p args, the arguments of main() with the program's name first,
/// into @p o. Returns the exit code when the program should stop: 0 after
/// `-h`, `--help`, or `--version`, which write the usage, or @p version and
/// a line feed, to @p out; 2 after an option it refuses, which it names on
/// @p errors with the value refused, followed by the usage, or after a
/// `--render` without `--out`. Options after the one that stops are not
/// read. Nothing when the program should go on.
std::optional<int> parse(std::span<const std::string_view> args, options& o, std::string_view version,
                         std::ostream& out, std::ostream& errors);

/// The engine's options for @p o at @p sampleRate and @p blockSize: the
/// frame size, 16 video pipes (a frame each), images and samples under
/// `--root`, the buffer memory bound, and the threads. Workers are given
/// real-time priority in a live engine, not in an offline render.
primitives::engineOptions engineOptionsFor(const options& o, double sampleRate, int blockSize);

}  // namespace server
