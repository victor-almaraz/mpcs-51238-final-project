// The buffer form: declarations, IDs, and buffer values (samples.md §5, §12.3).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

#include <espresso/interpreter/compiler.h>
#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>

#include "support/compile.h"
#include "support/unwrap.h"

using Catch::Matchers::ContainsSubstring;
using support::error;
using support::ok;
using support::piece;

TEST_CASE("buffer declares a buffer with its channels length and file") {
    const auto g = ok(piece(R"(
  (let ((ring (buffer :channels 2 :seconds 20s :file "samples/rain.wav"))
        (scratch (buffer :seconds 1.5))
        (loop (buffer :file "loop.wav" :channels 1.0)))
    (sine-osc :out (pipe :audio))))"));
    REQUIRE(g.buffers.size() == 3);
    const auto& ring = g.buffers[0];
    CHECK(ring.buffer.index == 0);
    CHECK(ring.id == "test.lisp/main/buffer@4:15#0");
    CHECK(ring.channels == 2);
    CHECK(ring.seconds == 20.0);
    CHECK(ring.file == "samples/rain.wav");
    CHECK(support::unwrap(ring.src).line == 4);
    // A plain number is in seconds; the channel count defaults to 1.
    CHECK(g.buffers[1].buffer.index == 1);
    CHECK(g.buffers[1].channels == 1);
    CHECK(g.buffers[1].seconds == 1.5);
    CHECK_FALSE(g.buffers[1].file.has_value());
    CHECK_FALSE(g.buffers[2].seconds.has_value());
    CHECK(ir::validate(g).empty());
}

TEST_CASE("buffers made at one call site are told apart by occurrence or by key") {
    const auto g = ok(piece(R"(
  (map (fn (n) (buffer :seconds n)) [1 2])
  (map (fn (k) (buffer :seconds 1 :key k)) ['a 'b]))"));
    REQUIRE(g.buffers.size() == 4);
    CHECK(g.buffers[0].id == "test.lisp/main/buffer@4:16#0");
    CHECK(g.buffers[1].id == "test.lisp/main/buffer@4:16#1");
    CHECK(g.buffers[2].id == "test.lisp/main/buffer@5:16#a");
    CHECK(g.buffers[3].id == "test.lisp/main/buffer@5:16#b");
}

TEST_CASE("a buffer's ID survives reordering when it has a key") {
    const auto ids = [](const std::string& keys) {
        const auto g = ok(piece("(map (fn (k) (buffer :seconds 1 :key k)) " + keys + ")"));
        return std::vector<std::string>{g.buffers[0].id, g.buffers[1].id};
    };
    const auto forward = ids("['a 'b]");
    const auto backward = ids("['b 'a]");
    CHECK(forward[0] == backward[1]);
    CHECK(forward[1] == backward[0]);
}

TEST_CASE("buffer values print compare and bind like pipes") {
    const auto g = ok(piece(R"(
  (let ((a (buffer :seconds 1))
        (b (buffer :seconds 1)))
    (lfo :offset (if (= a a) (if (= a b) 2 1) 0) :out (pipe :control))))"));
    CHECK(std::get<double>(support::find(g, "lfo").params.at("offset")) == 1.0);
    CHECK_THAT(error(piece("(lfo :offset (+ 1 (buffer :seconds 1)) :out (pipe :control))")),
               ContainsSubstring("buffer 0"));
}

TEST_CASE("buffer refuses bad options where they are written") {
    CHECK_THAT(error(piece("(buffer :channels 9 :seconds 1)")),
               ContainsSubstring("test.lisp:3:19: channels must be an integer from 1 to 8"));
    CHECK_THAT(error(piece("(buffer :channels 1.5 :seconds 1)")),
               ContainsSubstring("channels must be an integer from 1 to 8"));
    CHECK_THAT(error(piece("(buffer :seconds 440hz)")),
               ContainsSubstring(":seconds expects a time, such as 20s, got a frequency"));
    CHECK_THAT(error(piece("(buffer :seconds 2beat)")),
               ContainsSubstring("a buffer's length cannot follow the tempo"));
    CHECK_THAT(error(piece("(buffer :seconds 0)")), ContainsSubstring(":seconds must be a positive time"));
    CHECK_THAT(error(piece("(buffer :seconds -1s)")), ContainsSubstring(":seconds must be a positive time"));
    CHECK_THAT(error(piece("(buffer :file 'rain)")), ContainsSubstring(":file expects a path"));
    CHECK_THAT(error(piece("(buffer :file \"\")")), ContainsSubstring(":file expects a path"));
    CHECK_THAT(error(piece("(buffer :frames 10)")),
               ContainsSubstring("buffer has no option :frames; expected :channels, :seconds, or :file"));
    CHECK_THAT(error(piece("(buffer :channels 2)")),
               ContainsSubstring("test.lisp:3:1: buffer needs :seconds, :file, or both"));
    CHECK_THAT(error(piece("(buffer 2)")), ContainsSubstring("buffer takes keyword arguments"));
    CHECK_THAT(error(piece("(buffer :seconds 1 :seconds 2)")), ContainsSubstring(":seconds is given twice"));
}

TEST_CASE("buffers are made while a piece is built") {
    CHECK_THAT(
        error(interpreter::compile("(define b (buffer :seconds 1))\n(defpiece main () nil)\n", "test.lisp")),
        ContainsSubstring("buffers are made while a piece is built"));
}

TEST_CASE("a buffer is not a pipe or a number") {
    CHECK_THAT(error(piece("(sine-osc :freq (buffer :seconds 1) :out (pipe :audio))")),
               ContainsSubstring("parameter :freq does not take a buffer"));
    CHECK_THAT(error(piece("(gain :in (buffer :seconds 1) :out (pipe :audio))")),
               ContainsSubstring("expected a pipe or a vector of pipes, got buffer 0"));
}

TEST_CASE("buffer primitives take a buffer and record before they are read") {
    const auto g = ok(piece(R"(
  (let ((ring (buffer :seconds 2))
        (in (pipe :audio))
        (out (pipe :audio)))
    (buffer-read :buffer ring :position 0.5s :out out)
    (audio-in :out in)
    (buffer-write :buffer ring :in in :keep 0.5)))"));
    const auto& w = support::find(g, "bufferWrite");
    CHECK(std::get<metadata::bufferRef>(w.params.at("buffer")).index == 0);
    // The reader is written first; the sort puts the writer before it.
    std::vector<std::string> types;
    types.reserve(g.nodes.size());
    for (const auto& n : g.nodes) types.push_back(n.type);
    CHECK(types == std::vector<std::string>{"audioIn", "bufferWrite", "bufferRead"});
    CHECK_THAT(ir::write(g), ContainsSubstring(R"("buffer":{"buffer":0})"));
}

TEST_CASE("a buffer primitive needs its buffer") {
    CHECK_THAT(error(piece("(buffer-read :position 0 :out (pipe :audio))")),
               ContainsSubstring("buffer-read needs :buffer, a buffer made with (buffer ...)"));
    CHECK_THAT(error(piece("(buffer-read :buffer 2 :out (pipe :audio))")),
               ContainsSubstring("parameter :buffer expects a buffer, made with (buffer ...), got 2"));
    CHECK_THAT(error(piece("(buffer-read :buffer (pipe :audio) :out (pipe :audio))")),
               ContainsSubstring("parameter :buffer expects a buffer"));
}

TEST_CASE("fft and ifft compile through a frame pipe and refuse a size that is not a power of two") {
    const auto g = ok(piece(R"(
  (let ((in (pipe :audio)) (frames (pipe :event)) (out (pipe :audio)))
    (audio-in :out in)
    (fft :in in :size 1024 :overlap 2 :out frames)
    (ifft :in frames :out out)))"));
    CHECK(support::find(g, "ifft").inputs.at("in") == support::find(g, "fft").outputs.at("out"));
    CHECK_THAT(error(piece("(fft :in (pipe :audio) :size 1000 :out (pipe :event))")),
               ContainsSubstring("fft: :size must be a power of two"));
}
