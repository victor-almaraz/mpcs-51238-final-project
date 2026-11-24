// The client's command line (commandLine.h): the editor's options,
// --help, --version, and --compile with its project file, overrides, and
// errors (interpreter.md §11.1).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <espresso/protocol/messages.h>
#include <espresso/tui/app.h>

#include "commandLine.h"
#include "support/tempDir.h"

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

namespace {

// What a command line did: its outcome, and what it wrote to standard
// output and standard error.
struct ran {
    client::outcome outcome;
    std::string out;
    std::string err;
};

// Runs `espresso-client` with @p args.
ran run(std::initializer_list<std::string> args) {
    std::vector<std::string_view> line{"espresso-client"};
    line.insert(line.end(), args.begin(), args.end());
    std::ostringstream out;
    std::ostringstream err;
    auto outcome = client::run(line, out, err);
    return {.outcome = std::move(outcome), .out = out.str(), .err = err.str()};
}

// The exit status of @p r; fails the test when @p r opens the editor.
int status(const ran& r) {
    REQUIRE(std::holds_alternative<int>(r.outcome));
    return std::get<int>(r.outcome);
}

// The editor's options of @p r; fails the test when @p r exits instead.
const tui::appOptions& options(const ran& r) {
    REQUIRE(std::holds_alternative<tui::appOptions>(r.outcome));
    return std::get<tui::appOptions>(r.outcome);
}

// The file @p relative of the command-line tests' projects.
std::string cliFile(const std::string& relative) {
    return (fs::path(ESPRESSO_SOURCE_DIR) / "tests" / "cli" / relative).string();
}

// A piece with two entries, which differ by oscillator.
constexpr std::string_view twoPieces = "(defpiece main ()\n  (sine-osc :out (pipe :audio)))\n"
                                       "(defpiece alt ()\n  (saw-osc :out (pipe :audio)))\n";

}  // namespace

TEST_CASE("help and version write to standard output and exit with 0") {
    for (const std::string flag : {"--help", "-h"}) {
        INFO(flag);
        const auto r = run({flag});
        CHECK(status(r) == 0);
        CHECK_THAT(r.out, StartsWith("usage: espresso-client [--settings FILE]"));
        CHECK_THAT(r.out, ContainsSubstring("127.0.0.1:N (default " +
                                            std::to_string(protocol::defaultServerPort) + ")"));
        CHECK(r.err.empty());
    }
    const auto version = run({"--version"});
    CHECK(status(version) == 0);
    CHECK_THAT(version.out, StartsWith("espresso-client "));
    CHECK(version.err.empty());
    // The editor sends the version the client prints in /hello.
    CHECK(version.out == "espresso-client " + options(run({})).link.clientVersion + "\n");
}

TEST_CASE("the editor opens the current directory unless given a project and files") {
    const auto plain = run({});
    CHECK(options(plain).projectRoot == ".");
    CHECK(options(plain).files.empty());
    CHECK(options(plain).settingsFile.empty());
    CHECK(options(plain).link.server.serverPort == protocol::defaultServerPort);
    CHECK(plain.out.empty());
    CHECK(plain.err.empty());

    const support::tempDir dir;
    const auto given =
        run({"--settings", "my.lisp", "--port", "9000", dir.path().string(), "a.lisp", "lib/b.lisp"});
    const auto& o = options(given);
    CHECK(o.settingsFile == "my.lisp");
    CHECK(o.link.server.serverPort == 9000);
    CHECK(o.projectRoot == dir.path());
    CHECK(o.files == std::vector<fs::path>{"a.lisp", "lib/b.lisp"});
    CHECK(given.out.empty());
    CHECK(given.err.empty());
}

TEST_CASE("the editor refuses options it does not take and a project that is not a directory") {
    const auto noSettings = run({"--settings"});
    CHECK(status(noSettings) == 2);
    CHECK_THAT(noSettings.err, StartsWith("usage: espresso-client"));
    for (const std::string port : {"0", "65536", "9000.5", "x"}) {
        INFO(port);
        const auto r = run({"--port", port});
        CHECK(status(r) == 2);
        CHECK(r.err == "espresso-client: --port needs a number from 1 to 65535\n");
    }
    CHECK(run({"--port"}).err == "espresso-client: --port needs a number from 1 to 65535\n");
    const auto unknown = run({"--bogus"});
    CHECK(status(unknown) == 2);
    CHECK_THAT(unknown.err, StartsWith("espresso-client: unknown option --bogus\nusage: espresso-client"));
    CHECK(unknown.out.empty());

    const support::tempDir dir;
    dir.write("main.lisp", "");
    for (const fs::path& root : {dir / "main.lisp", dir / "missing"}) {
        const auto r = run({root.string()});
        CHECK(status(r) == 2);
        CHECK(r.err == "espresso-client: " + root.string() + " is not a directory\n");
    }
}

TEST_CASE("compile writes the IR of a file's piece with its imports") {
    const auto r = run({"--compile", cliFile("project/main.lisp")});
    CHECK(status(r) == 0);
    CHECK_THAT(r.out, ContainsSubstring(R"("format": "espresso-ir")"));
    // Node IDs name the file relative to its folder.
    CHECK_THAT(r.out, ContainsSubstring(R"("id": "main.lisp/main/audio-out@)"));
    CHECK(r.err.empty());
}

TEST_CASE("compile takes the piece seed and page port from project.lisp and --entry and --seed override it") {
    const auto paged = run({"--compile", cliFile("paged/main.lisp")});
    CHECK(status(paged) == 0);
    CHECK_THAT(paged.out, ContainsSubstring(R"("page": {)"));
    CHECK_THAT(paged.out, ContainsSubstring(R"("port": 8000)"));

    const support::tempDir dir;
    dir.write("project.lisp", "(project :entry alt :seed 7)\n");
    dir.write("main.lisp", twoPieces);
    const std::string piece = (dir / "main.lisp").string();
    const auto project = run({"--compile", piece});
    CHECK(status(project) == 0);
    CHECK_THAT(project.out, ContainsSubstring(R"("seed": 7)"));
    CHECK_THAT(project.out, ContainsSubstring(R"("type": "sawOsc")"));
    const auto overridden = run({"--compile", piece, "--entry", "main", "--seed", "9"});
    CHECK(status(overridden) == 0);
    CHECK_THAT(overridden.out, ContainsSubstring(R"("seed": 9)"));
    CHECK_THAT(overridden.out, ContainsSubstring(R"("type": "sineOsc")"));
}

TEST_CASE("compile names the file and resolves imports from --root") {
    const support::tempDir dir;
    dir.write("voices.lisp", "(module voices (export tone))\n(define (tone out) (sine-osc :out out))\n");
    dir.write("pieces/main.lisp", "(module main)\n(import \"voices\")\n\n(defpiece main ()\n"
                                  "  (let ((osc (pipe :audio)))\n    (tone osc)\n"
                                  "    (audio-out :in osc :channel 0)))\n");
    const std::string piece = (dir / "pieces" / "main.lisp").string();
    const auto rooted = run({"--compile", piece, "--root", dir.path().string()});
    INFO(rooted.err);
    CHECK(status(rooted) == 0);
    CHECK_THAT(rooted.out, ContainsSubstring(R"("id": "pieces/main.lisp/main/audio-out@7:5)"));
    // Without --root, imports resolve against the file's folder.
    const auto unrooted = run({"--compile", piece});
    CHECK(status(unrooted) == 1);
    CHECK_THAT(unrooted.err,
               ContainsSubstring("pieces/main.lisp:2:9: cannot import voices.lisp: cannot open "));
    CHECK(unrooted.out.empty());
}

TEST_CASE("compile refuses arguments it does not take") {
    const std::string piece = cliFile("project/main.lisp");
    const auto bogus = run({"--compile", piece, "--bogus"});
    CHECK(status(bogus) == 2);
    CHECK_THAT(bogus.err, StartsWith("espresso-client: invalid argument --bogus\nusage: espresso-client"));
    CHECK(bogus.out.empty());
    // An option without its value is refused as it stands.
    CHECK_THAT(run({"--compile"}).err, StartsWith("espresso-client: invalid argument --compile\n"));
    CHECK_THAT(run({"--compile", piece, "--root"}).err,
               StartsWith("espresso-client: invalid argument --root\n"));
    const auto seed = run({"--compile", piece, "--seed", "1x"});
    CHECK(status(seed) == 2);
    CHECK(seed.err ==
          "espresso-client: invalid --seed 1x; it needs a whole number from 0 to 18446744073709551615\n");
}

TEST_CASE("compile reports a file or project file it cannot read or that is not valid") {
    const support::tempDir dir;
    const auto missing = run({"--compile", (dir / "missing.lisp").string()});
    CHECK(status(missing) == 1);
    CHECK_THAT(missing.err, StartsWith("espresso-client: cannot open " + (dir / "missing.lisp").string()));

    dir.write("main.lisp", twoPieces);
    dir.write("project.lisp", "\xff\xfe");
    const auto unreadable = run({"--compile", (dir / "main.lisp").string()});
    CHECK(status(unreadable) == 1);
    CHECK(unreadable.err == "espresso-client: " + (dir / "project.lisp").string() + " is not valid UTF-8\n");
    CHECK(unreadable.out.empty());

    const auto invalid = run({"--compile", cliFile("bad/main.lisp")});
    CHECK(status(invalid) == 1);
    CHECK_THAT(invalid.err,
               ContainsSubstring("bad/project.lisp:1:21: :page-port takes a port from 1024 to 65535\n"));
    CHECK(invalid.out.empty());
}

TEST_CASE("compile reports errors and warnings at their file") {
    const std::string piece = cliFile("project/main.lisp");
    const auto error = run({"--compile", piece, "--entry", "nosuch"});
    CHECK(status(error) == 1);
    CHECK_THAT(error.err, ContainsSubstring("project/main.lisp:"));
    CHECK_THAT(error.err, ContainsSubstring(": no piece named nosuch"));
    CHECK(error.out.empty());
    const auto warning = run({"--compile", piece, "--entry", "loud"});
    CHECK(status(warning) == 0);
    CHECK_THAT(warning.err, ContainsSubstring("project/main.lisp:14:5: warning: a gain of 100"));
    CHECK_THAT(warning.out, ContainsSubstring(R"("format": "espresso-ir")"));
}
