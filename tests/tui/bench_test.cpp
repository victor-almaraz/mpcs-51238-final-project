// Timing of the editor's hot paths. Hidden; run with: tui_tests "[.bench]"
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <ratio>
#include <string>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <espresso/tui/app.h>
#include <espresso/tui/buffer.h>
#include <espresso/workspace/highlighter.h>
#include <espresso/workspace/interpreterLink.h>

#include "support/tempDir.h"

namespace {

std::string bigSource(int lines) {
    std::string s;
    for (int i = 0; i < lines; ++i) {
        s += "(svf :in osc-" + std::to_string(i) + " :cutoff 1.2khz :q 0.7 :mode :low :out out) ; c\n";
    }
    return s;
}

template <typename F>
double msPer(int n, F&& f) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / n;
}

} // namespace

TEST_CASE("hot paths", "[.bench]") {
    const std::string src = bigSource(20000);  // ~1.4 MB
    tui::buffer b(src);
    b.moveBufferEnd();
    b.moveUp(10000);
    std::cout << "bytes " << src.size() << "\n";
    std::cout << "insert + positionOf:   " << msPer(200, [&] { b.insert("x"); }) << " ms\n";
    std::cout << "lineCount after edit:  " << msPer(200, [&] { b.insert("x"); (void)b.lineCount(); }) << " ms\n";
    std::cout << "text() copy:           " << msPer(200, [&] { (void)b.text(); }) << " ms\n";
    std::cout << "highlight (re-lex):    " << msPer(50, [&] { (void)workspace::highlight(src); }) << " ms\n";

    const support::tempDir dir;
    dir.write("main.lisp", src);
    tui::app a({.projectRoot = dir.path(),
                .files = {},
                .settingsFile = dir / "s.lisp",
                .persistSettings = false,
                .link = workspace::linkOptions{.server = {.serverPort = 1, .timeoutMs = 5, .retries = 0},
                                               .clientVersion = "b",
                                               .connectOnStart = false}});
    a.handleKey("M->");
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(120), ftxui::Dimension::Fixed(40));
    std::cout << "render, unchanged:     " << msPer(50, [&] { ftxui::Render(screen, a.render()); }) << " ms\n";
    std::cout << "keystroke + render:    "
              << msPer(50, [&] { a.handleKey("a"); ftxui::Render(screen, a.render()); }) << " ms\n";
}
