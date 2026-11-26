// The sidecar mode (serve.h, tauri-editor.md §4): requests in, one JSON-RPC
// 2.0 message a line, and the responses and notifications that come out.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <espresso/metadata/version.h>
#include <espresso/workspace/fileStore.h>
#include <espresso/workspace/highlighter.h>
#include <espresso/workspace/interpreterLink.h>

#include "serve.h"
#include "support/tempDir.h"
#include "support/testServer.h"
#include "support/timing.h"

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using nlohmann::json;

namespace {

// A link that reaches no server and does not try until a run request.
workspace::linkOptions noServer() {
    return {.server = {.serverPort = 1, .timeoutMs = 5, .retries = 0},
            .clientVersion = "test",
            .connectOnStart = false};
}

// A link to the test server on @p port.
workspace::linkOptions serverAt(std::uint16_t port) {
    return {.server = {.serverPort = port, .timeoutMs = support::replyTimeoutMs, .retries = 3},
            .clientVersion = "test",
            .connectOnStart = true};
}

// Serves @p lines and gives every line written, each parsed: every line
// must be one JSON message (§4.2).
std::vector<json> serveLines(const std::vector<std::string>& lines,
                             workspace::linkOptions link = noServer()) {
    std::string input;
    for (const auto& line : lines) input += line + "\n";
    std::istringstream in(input);
    std::ostringstream out;
    const int status = client::serve(in, out, {.link = std::move(link)});
    REQUIRE(status == 0);
    std::vector<json> messages;
    std::istringstream written(out.str());
    for (std::string line; std::getline(written, line);) {
        const json m = json::parse(line, nullptr, false);
        INFO(line);
        REQUIRE(m.is_object());
        REQUIRE(m.at("jsonrpc") == "2.0");
        messages.push_back(m);
    }
    return messages;
}

// A request line.
std::string request(const json& id, std::string_view method, json params = json::object()) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}}.dump();
}

// The responses among @p messages whose id is @p id.
std::vector<json> responsesTo(const std::vector<json>& messages, const json& id) {
    std::vector<json> found;
    for (const auto& m : messages) {
        if (m.contains("id") && m.at("id") == id) found.push_back(m);
    }
    return found;
}

// The one response to @p id.
json responseTo(const std::vector<json>& messages, const json& id) {
    const auto found = responsesTo(messages, id);
    INFO("id " << id.dump());
    REQUIRE(found.size() == 1);
    return found.front();
}

// The error code of the one response to @p id, or 0 for a result.
int errorOf(const std::vector<json>& messages, const json& id) {
    const json r = responseTo(messages, id);
    return r.contains("error") ? r.at("error").at("code").get<int>() : 0;
}

// The index in @p messages of the first console notification whose text
// contains @p text, or messages.size().
std::size_t consoleLine(const std::vector<json>& messages, std::string_view text) {
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const json& m = messages[i];
        if (m.value("method", "") == "console" &&
            m.at("params").at("text").get<std::string>().find(text) != std::string::npos) {
            return i;
        }
    }
    return messages.size();
}

std::size_t indexOf(const std::vector<json>& messages, const json& id) {
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].contains("id") && messages[i].at("id") == id) return i;
    }
    return messages.size();
}

struct project : support::tempDir {
    project() {
        write("main.lisp", "(module main)\n(defpiece main () nil)\n");
        write("lib/tone.lisp", "(module tone)\n");
        write(".hidden", "x");
    }
    json open() const { return {{"root", path().string()}}; }
};

}  // namespace

TEST_CASE("the sidecar answers hello and refuses lines that are not requests (tauri-editor.md 4.3)",
          "[client][serve]") {
    const auto out = serveLines({
        request(1, "hello", {{"version", "editor-test"}}),
        "",  // ignored
        "not json",
        "[1, 2]",
        R"({"jsonrpc": "2.0", "id": true, "method": "hello"})",
        R"({"jsonrpc": "1.0", "id": 2, "method": "hello"})",
        R"({"jsonrpc": "2.0", "id": 3, "method": 7})",
        request(4, "no.such.method"),
        R"({"jsonrpc": "2.0", "id": "five", "method": "hello", "params": [1]})",
        request("six", "hello", {{"version", 6}}),
        R"({"jsonrpc": "2.0", "method": "hello"})",  // a notification: no response
    });
    const json hello = responseTo(out, 1);
    CHECK(hello.at("result") == json{{"client", "test"},
                                     {"protocol", 1},
                                     {"metadata", metadata::version},
                                     {"server", "127.0.0.1:1"}});

    // Lines that are not requests are answered with a null id.
    const auto unidentified = responsesTo(out, nullptr);
    REQUIRE(unidentified.size() == 5);
    CHECK(unidentified[0].at("error").at("code") == -32700);  // not JSON
    for (std::size_t i = 1; i < unidentified.size(); ++i) {
        CHECK(unidentified[i].at("error").at("code") == -32600);  // a batch, a bad id, jsonrpc, method
    }
    CHECK(errorOf(out, 4) == -32601);
    CHECK(errorOf(out, "five") == -32602);  // params must be an object
    const json six = responseTo(out, "six");
    CHECK(six.at("error").at("code") == -32602);
    CHECK(six.at("error").at("message") == "params.version must be a string");
    CHECK(out.size() == 9);  // one line each but the blank line and the notification
}

TEST_CASE("project.open lists the project, and file.read and file.write go through the file store "
          "(tauri-editor.md 4.5)",
          "[client][serve]") {
    const project p;
    p.write("latin1.lisp", "caf\xe9");
    const auto out = serveLines({
        request(1, "file.read", {{"path", "main.lisp"}}),  // no project yet
        request(2, "project.open", {{"root", (p / "missing").string()}}),
        request(3, "project.open", p.open()),
        request(4, "file.read", {{"path", "main.lisp"}}),
        request(5, "file.write", {{"path", "new.lisp"}, {"text", "(module new)\n"}}),
        request(6, "file.read", {{"path", "./new.lisp"}}),
        request(7, "file.read", {{"path", "../outside.lisp"}}),
        request(8, "file.read", {{"path", (p / "main.lisp").string()}}),
        request(9, "file.read", {{"path", ""}}),
        request(10, "file.read", {{"path", "latin1.lisp"}}),
        request(11, "file.read", {{"path", "absent.lisp"}}),
        request(12, "file.write", {{"path", "main.lisp"}}),
    });
    CHECK(errorOf(out, 1) == -32001);
    CHECK(errorOf(out, 2) == -32602);

    const json opened = responseTo(out, 3).at("result");
    CHECK(opened.at("root") == fs::canonical(p.path()).generic_string());
    CHECK(opened.at("files") ==
          json{"latin1.lisp", "lib/tone.lisp", "main.lisp"});  // hidden files are not listed

    CHECK(responseTo(out, 4).at("result").at("text") == "(module main)\n(defpiece main () nil)\n");
    CHECK(responseTo(out, 5).at("result").is_null());
    CHECK(responseTo(out, 6).at("result").at("text") == "(module new)\n");
    CHECK(workspace::fileStore::read(p / "new.lisp").text == "(module new)\n");

    CHECK(errorOf(out, 7) == -32602);  // outside the project
    CHECK(errorOf(out, 8) == -32602);  // absolute
    CHECK(errorOf(out, 9) == -32602);  // empty
    const json latin1 = responseTo(out, 10);
    CHECK(latin1.at("error").at("code") == -32002);
    CHECK_THAT(latin1.at("error").at("message").get<std::string>(), ContainsSubstring("UTF-8"));
    CHECK(errorOf(out, 11) == -32002);
    CHECK(errorOf(out, 12) == -32602);  // no text
    CHECK(workspace::fileStore::read(p / "main.lisp").text == "(module main)\n(defpiece main () nil)\n");
}

TEST_CASE("file.create makes a file and the folders above it, and file.rename moves one, refusing a name "
          "that exists "
          "(patterns.md 20.7)",
          "[client][serve]") {
    const project p;
    const auto out = serveLines({
        request(1, "file.create", {{"path", "new.lisp"}}),  // no project yet
        request(2, "project.open", p.open()),
        request(3, "file.create", {{"path", "new.lisp"}}),
        request(4, "file.create", {{"path", "voices/bass/line.lisp"}}),
        request(5, "file.create", {{"path", "main.lisp"}}),
        request(7, "file.create", {{"path", "../outside.lisp"}}),
        request(8, "file.rename", {{"from", "new.lisp"}, {"to", "voices/lead/line.lisp"}}),
        request(9, "file.rename", {{"from", "lib"}, {"to", "library"}}),
        request(10, "file.rename", {{"from", "main.lisp"}, {"to", "library/tone.lisp"}}),
        request(11, "file.rename", {{"from", "absent.lisp"}, {"to", "here.lisp"}}),
        request(12, "file.rename", {{"from", "main.lisp"}, {"to", "/tmp/elsewhere.lisp"}}),
    });
    CHECK(errorOf(out, 1) == -32001);
    CHECK(responseTo(out, 3).at("result").is_null());
    CHECK(responseTo(out, 4).at("result").is_null());
    CHECK(workspace::fileStore::read(p / "voices/bass/line.lisp").text == "");
    const json exists = responseTo(out, 5);
    CHECK(exists.at("error").at("code") == -32002);
    CHECK_THAT(exists.at("error").at("message").get<std::string>(),
               ContainsSubstring("\u201cmain.lisp\u201d already exists"));
    CHECK(errorOf(out, 7) == -32602);
    CHECK(responseTo(out, 8).at("result").is_null());
    CHECK(workspace::fileStore::read(p / "voices/lead/line.lisp").text == "");
    CHECK(!fs::exists(p / "new.lisp"));
    CHECK(responseTo(out, 9).at("result").is_null());
    CHECK(workspace::fileStore::read(p / "library/tone.lisp").text == "(module tone)\n");
    CHECK(errorOf(out, 10) == -32002);  // the target exists
    CHECK(workspace::fileStore::read(p / "main.lisp").text == "(module main)\n(defpiece main () nil)\n");
    CHECK(errorOf(out, 11) == -32002);
    CHECK(errorOf(out, 12) == -32602);
}

TEST_CASE("highlight answers the C++ highlighter's spans by category name (tauri-editor.md 4.5.2)",
          "[client][serve]") {
    const std::string text = "(defpiece main () ; drone\n  (sine :freq 440hz \"é\"))\n#";
    const auto out = serveLines({request(1, "highlight", {{"text", text}}), request(2, "highlight")});
    const json spans = responseTo(out, 1).at("result").at("spans");
    const auto expected = workspace::highlight(text).spans;
    REQUIRE(spans.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(spans[i].at("offset") == expected[i].offset);
        CHECK(spans[i].at("length") == expected[i].length);
    }
    std::vector<std::string> categories;
    for (const auto& s : spans) categories.push_back(s.at("category").get<std::string>());
    CHECK(categories.front() == "delimiter");
    CHECK(categories[1] == "specialForm");
    CHECK(std::ranges::find(categories, "comment") != categories.end());
    CHECK(std::ranges::find(categories, "keyword") != categories.end());
    CHECK(std::ranges::find(categories, "unitLiteral") != categories.end());
    CHECK(std::ranges::find(categories, "string") != categories.end());
    CHECK(errorOf(out, 2) == -32602);
}

TEST_CASE("run requests play the project with the editor's buffers, and answer after their console lines "
          "(tauri-editor.md 4.5.1, 4.6.1)",
          "[client][serve][server]") {
    support::testServer server{std::string(metadata::version)};
    project p;
    p.write("main.lisp", "(module main");  // on disk, unreadable; the buffer below is not
    const json buffers = {{"main.lisp", "(module main)\n(defpiece main () nil)\n"}, {"notes.txt", "("}};
    const auto out = serveLines(
        {
            request(1, "run.play"),  // no project yet
            request(2, "project.open", p.open()),
            request(3, "run.play", {{"sources", buffers}, {"file", "main.lisp"}, {"quantize", "beat"}}),
            request(4, "run.play"),  // the file on disk
            request(5, "run.eval",
                    {{"text", "(defpiece main () nil)"},
                     {"file", "main.lisp"},
                     {"line", 2},
                     {"column", 1},
                     {"sources", buffers}}),
            request(6, "run.eval", {{"text", "(x)"}, {"file", "main.lisp"}, {"line", 0}, {"column", 1}}),
            request(7, "run.play", {{"fade", -1}}),
            request(8, "run.play", {{"quantize", "phrase"}}),
            request(9, "run.play", {{"sources", {{"../x.lisp", "("}}}}),
            request(10, "run.stop", {{"fade", 0.5}}),
        },
        serverAt(server.port()));

    CHECK(errorOf(out, 1) == -32001);
    for (const int refused : {6, 7, 8, 9}) {
        INFO("id " << refused);
        CHECK(errorOf(out, refused) == -32602);
    }

    // The buffer takes the place of the file on disk, and its plan reaches
    // the server; the response follows the request's console lines.
    CHECK(responseTo(out, 3).at("result") == json{{"errors", 0}});
    const std::size_t scheduled = consoleLine(out, "server: plan 1 scheduled (fade 50ms, quant beat)");
    REQUIRE(scheduled < out.size());
    CHECK(scheduled < indexOf(out, 3));
    CHECK(!server.ir(1).empty());

    // Without the buffer, the file on disk does not read: an error that
    // opens it, and the running plan is unchanged.
    CHECK(responseTo(out, 4).at("result").at("errors") == 2);
    const std::size_t readError = consoleLine(out, "main.lisp:1:");
    REQUIRE(readError < out.size());
    CHECK(out[readError].at("params").at("kind") == "error");
    CHECK(out[readError].at("params").at("location").at("path") == "main.lisp");
    CHECK(out[readError].at("params").at("location").at("line") == 1);
    CHECK(readError < indexOf(out, 4));

    CHECK(responseTo(out, 5).at("result") == json{{"errors", 0}});
    CHECK(consoleLine(out, "eval main.lisp:2:1: read (defpiece main () nil)") < indexOf(out, 5));

    CHECK(responseTo(out, 10).at("result") == json{{"errors", 0}});
    CHECK(consoleLine(out, "server: stopping (fade 500ms)") < indexOf(out, 10));

    // The link connected when the sidecar started.
    const auto connected = std::ranges::find_if(out, [](const json& m) {
        return m.value("method", "") == "status" && m.at("params").at("connected") == true;
    });
    REQUIRE(connected != out.end());
    CHECK(connected->at("params").at("server") == "test-server");
    CHECK(connected->at("params").contains("threadLoads"));
    CHECK(connected->at("params").at("page").is_null());  // no plan with a page plays (io.md 12.2)
}

TEST_CASE("language lists the builtins, the primitives with their parameters, and the library "
          "(tauri-editor.md 4.5)",
          "[client][serve]") {
    const auto out = serveLines({request(1, "language")});
    const json result = responseTo(out, 1).at("result");
    const auto& builtins = result.at("builtins");
    CHECK(std::ranges::find(builtins, json("map")) != builtins.end());
    CHECK(std::ranges::find(builtins, json("slot-value")) != builtins.end());
    CHECK(std::ranges::find(builtins, json("sine-osc")) ==
          builtins.end());  // a primitive, listed with its parameters
    const auto& primitives = result.at("primitives");
    const auto sine =
        std::ranges::find_if(primitives, [](const json& p) { return p.at("name") == "sine-osc"; });
    REQUIRE(sine != primitives.end());
    const auto& params = sine->at("params");
    const auto freq = std::ranges::find_if(params, [](const json& p) { return p.at("name") == "freq"; });
    REQUIRE(freq != params.end());
    CHECK(freq->at("dimension") == "frequency");
    CHECK(freq->at("default").is_number());
    CHECK(std::ranges::any_of(sine->at("outputs"), [](const json& p) { return p.at("name") == "out"; }));
    CHECK(std::ranges::any_of(result.at("library"), [](const json& f) {
        return f.at("path") == "std/instruments.lisp" &&
               f.at("text").get<std::string>().find("defclass") != std::string::npos;
    }));
}

TEST_CASE("server.connect reaches the server, or says it is not reachable (tauri-editor.md 4.5)",
          "[client][serve][server]") {
    const auto absent = serveLines({request(1, "server.connect")});
    CHECK(responseTo(absent, 1).at("result") == json{{"errors", 0}});
    CHECK(consoleLine(absent, "server: not reachable at 127.0.0.1:1") < indexOf(absent, 1));

    support::testServer server{std::string(metadata::version)};
    auto link = serverAt(server.port());
    link.connectOnStart = false;  // only the request connects
    const auto out = serveLines({request(1, "server.connect")}, std::move(link));
    CHECK(responseTo(out, 1).at("result") == json{{"errors", 0}});
    const auto connected = std::ranges::find_if(out, [](const json& m) {
        return m.value("method", "") == "status" && m.at("params").at("connected") == true;
    });
    REQUIRE(connected != out.end());
    CHECK(connected - out.begin() < static_cast<std::ptrdiff_t>(indexOf(out, 1)));
}

TEST_CASE("a run request without an id runs unanswered, and shutdown ends the session (tauri-editor.md 4.7)",
          "[client][serve][server]") {
    support::testServer server{std::string(metadata::version)};
    const project p;
    const auto out = serveLines(
        {
            request(1, "project.open", p.open()),
            json{{"jsonrpc", "2.0"}, {"method", "run.play"}}.dump(),
            request(2, "shutdown"),
            request(3, "hello"),  // after shutdown: not read
        },
        serverAt(server.port()));
    CHECK(responseTo(out, 2).at("result").is_null());
    CHECK(responsesTo(out, 3).empty());
    CHECK(consoleLine(out, "server: plan 1 scheduled") < out.size());  // the notification ran
    std::size_t responses = 0;
    for (const auto& m : out) responses += m.contains("id") ? 1 : 0;
    CHECK(responses == 2);  // project.open and shutdown
}
