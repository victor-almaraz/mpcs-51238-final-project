// The page's layout in the IR (dsl-ui.md §7.1): reading and writing it,
// the schema, and the semantic checks against its kinds' table and the
// nodes on the page port.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <espresso/ir/ir.h>
#include <espresso/metadata/graph.h>
#include <espresso/metadata/pageSpec.h>

#include "support/files.h"
#include "support/pipes.h"
#include "support/unwrap.h"
#include "support/validation.h"

using Catch::Matchers::ContainsSubstring;
using metadata::graphSpec;
using metadata::pageNode;
using metadata::paramValue;
using support::control;
using support::errors;
using params = std::map<std::string, paramValue, std::less<>>;

namespace {

constexpr int port = 8000;

pageNode element(std::string kind, std::string label = "", std::string address = "", params p = {},
                 std::vector<pageNode> children = {}) {
    return {.kind = std::move(kind),
            .label = std::move(label),
            .address = std::move(address),
            .params = std::move(p),
            .children = std::move(children),
            .src = std::nullopt};
}

// A page of one panel of one section holding @p controls.
pageNode page(std::vector<pageNode> controls) {
    return element(
        "page", "", "", {},
        {element("panel", "Synth", "", {}, {element("section", "Filter", "", {}, std::move(controls))})});
}

// A graph on page port 8000 whose oscIn reads /cutoff into control pipe 1
// and whose oscOut sends control pipe 1 to /level, laid out as @p layout.
graphSpec piece(std::optional<pageNode> layout, std::string readAddress = "/cutoff") {
    graphSpec g;
    g.pagePort = port;
    g.pipes = {{.pipe = control(1), .channels = 1, .feedback = false}};
    g.nodes.push_back({.id = "in",
                       .type = "oscIn",
                       .params = {{"port", static_cast<double>(port)}, {"address", std::move(readAddress)}},
                       .inputs = {},
                       .outputs = {{"value", {control(1)}}},
                       .src = std::nullopt});
    g.nodes.push_back({.id = "out",
                       .type = "oscOut",
                       .params = {{"port", static_cast<double>(port)}, {"address", std::string("/level")}},
                       .inputs = {{"value", {control(1)}}},
                       .outputs = {},
                       .src = std::nullopt});
    g.layout = std::move(layout);
    return g;
}

pageNode cutoff(params p = {
                    {"min", 20.0}, {"max", 20000.0}, {"scale", std::string("exp")}, {"initial", 800.0}}) {
    return element("knob", "Cutoff", "/cutoff", std::move(p));
}

std::string errorsOf(std::vector<pageNode> controls) { return errors(piece(page(std::move(controls)))); }

}  // namespace

TEST_CASE("a layout reads and writes with its graph (dsl-ui.md 7.1)", "[ir][layout]") {
    auto layout = page({cutoff(), element("meter", "Level", "/level", {{"max", 0.25}})});
    layout.children[0].children[0].params.emplace("columns", 6.0);
    layout.children[0].children[0].children[0].params.emplace("span", std::vector<double>{4, 4});
    layout.src =
        metadata::sourceSpan{.file = "main.lisp", .line = 3, .column = 5, .offset = 40, .length = 12};
    const graphSpec g = piece(layout);
    REQUIRE(errors(g).empty());

    const std::string text = ir::write(g);
    CHECK_THAT(text, ContainsSubstring(
                         R"("page":{"layout":{"children":[{"children":[{"children":[{"address":"/cutoff")"));
    const auto read = ir::read(text);
    INFO(read.text());
    REQUIRE(read.graph);
    CHECK(support::unwrap(read.graph) == g);
}

TEST_CASE("a layout is optional, and absent from a graph without one", "[ir][layout]") {
    const graphSpec g = piece(std::nullopt);
    REQUIRE(errors(g).empty());
    CHECK_THAT(ir::write(g), ContainsSubstring(R"("page":{"port":8000})"));
}

TEST_CASE("the schema admits a layout element's members and no others", "[ir][layout]") {
    const auto document = [](const std::string& layout) {
        return R"({"format":"espresso-ir","version":1,"page":{"port":8000,"layout":)" + layout +
               R"(},"pipes":[{"pipe":{"control":1}}],"nodes":[{"id":"in","type":"oscIn",)"
               R"("params":{"port":8000},"outputs":{"value":{"control":1}}}]})";
    };
    const auto good = ir::read(document(R"({"kind":"page","children":[{"kind":"panel","children":[)"
                                        R"({"kind":"section","children":[{"kind":"toggle","label":"On",)"
                                        R"("address":"/on","params":{"initial":1}}]}]}]})"));
    INFO(good.text());
    REQUIRE(good.graph);
    const pageNode& toggle =
        support::unwrap(support::unwrap(good.graph).layout).children[0].children[0].children[0];
    CHECK(toggle.label == "On");
    CHECK(toggle.params.at("initial") == paramValue(1.0));

    CHECK_THAT(ir::read(document(R"({"kind":"page","color":"red"})")).text(),
               ContainsSubstring("/page/layout"));
    CHECK_THAT(ir::read(document(R"({"label":"no kind"})")).text(), ContainsSubstring("/page/layout"));
    CHECK_THAT(ir::read(document(R"({"kind":"page","children":{}})")).text(),
               ContainsSubstring("/page/layout/children"));
}

TEST_CASE("a layout needs a page port and a page at its root", "[ir][layout]") {
    graphSpec g = piece(page({cutoff()}));
    g.pagePort.reset();
    g.nodes.clear();
    CHECK_THAT(errors(g), ContainsSubstring("layout: page: a layout needs a page port"));
    CHECK_THAT(errors(piece(element("panel"))),
               ContainsSubstring("the layout's root must be a page, not panel"));
}

TEST_CASE("each element is of a kind its parent holds", "[ir][layout]") {
    CHECK_THAT(errorsOf({element("dial", "Odd", "/cutoff")}),
               ContainsSubstring(
                   "layout: page > panel \"Synth\" > section \"Filter\" > dial \"Odd\": unknown kind dial"));
    CHECK_THAT(errorsOf({element("section")}),
               ContainsSubstring("a section holds knob, slider, toggle, button, select, xy, keys, "
                                 "footswitch, drawbar, meter, lamp, value, "
                                 "not a section"));
    CHECK_THAT(errors(piece(element("page", "", "", {}, {element("section")}))),
               ContainsSubstring("a page holds panel, synth, pedalboard, not a section"));
    CHECK_THAT(errors(piece(element("page", "", "", {}, {element("panel", "", "", {}, {cutoff()})}))),
               ContainsSubstring("a panel holds section, not a knob"));
    auto holding = cutoff();
    holding.children.push_back(cutoff());
    CHECK_THAT(errorsOf({holding}), ContainsSubstring("a knob holds no elements"));
    auto addressed = page({cutoff()});
    addressed.children[0].address = "/panel";
    CHECK_THAT(errors(piece(addressed)), ContainsSubstring("panel \"Synth\": a group has no address"));
}

TEST_CASE("an element's parameters are its kind's, of their types", "[ir][layout]") {
    CHECK_THAT(errorsOf({cutoff({{"colour", 1.0}})}), ContainsSubstring("unknown parameter colour for knob"));
    CHECK_THAT(errorsOf({cutoff({{"scale", std::string("log")}})}),
               ContainsSubstring("parameter scale must be one of: linear, exp, db"));
    CHECK_THAT(errorsOf({cutoff({{"initial", std::string("high")}})}),
               ContainsSubstring("parameter initial must be a number"));
    CHECK_THAT(errorsOf({cutoff({{"min", control(1)}})}),
               ContainsSubstring("parameter min takes no pipe or buffer"));
    CHECK_THAT(errorsOf({element("toggle", "On", "/cutoff", {{"initial", 2.0}})}),
               ContainsSubstring("parameter initial is 2; expected 0 to 1"));
    CHECK_THAT(errorsOf({element("select", "Wave", "/cutoff")}),
               ContainsSubstring("missing parameter options"));
    CHECK(errorsOf({element("select", "Wave", "/cutoff",
                            {{"options", std::vector<std::string>{"sine", "saw"}}, {"initial", 1.0}})})
              .empty());
}

TEST_CASE("every element but the root takes a place of two whole numbers", "[ir][layout]") {
    CHECK(
        errorsOf({cutoff({{"span", std::vector<double>{4, 5}}, {"at", std::vector<double>{1, 2}}})}).empty());
    CHECK_THAT(errorsOf({cutoff({{"span", std::vector<double>{4}}})}),
               ContainsSubstring("parameter span must be two whole numbers from 1"));
    CHECK_THAT(errorsOf({cutoff({{"at", std::vector<double>{0, 1.5}}})}),
               ContainsSubstring("parameter at must be two whole numbers from 1"));
    auto root = page({cutoff()});
    root.params.emplace("span", std::vector<double>{1, 1});
    CHECK_THAT(errors(piece(root)), ContainsSubstring("layout: page: unknown parameter span for page"));
}

TEST_CASE("a control sends to an address an oscIn on the page port reads", "[ir][layout]") {
    CHECK_THAT(errorsOf({element("knob", "Cutoff")}), ContainsSubstring("knob \"Cutoff\": needs an address"));
    CHECK_THAT(errorsOf({element("knob", "Cutoff", "cutoff")}),
               ContainsSubstring("address cutoff must start with /"));
    CHECK_THAT(errorsOf({element("knob", "Q", "/q")}),
               ContainsSubstring("knob \"Q\": sends to /q, which no oscIn on the page port reads"));
    // An oscIn on another port does not read the page's messages.
    graphSpec elsewhere = piece(page({cutoff()}));
    elsewhere.nodes[0].params["port"] = 9000.0;
    CHECK_THAT(errors(elsewhere),
               ContainsSubstring("sends to /cutoff, which no oscIn on the page port reads"));
    // An oscIn without an address reads every message: a keyboard's notes.
    CHECK(errors(piece(page({element("keys"), element("knob", "Q", "/q")}), "")).empty());
    CHECK_THAT(errorsOf({element("keys")}), ContainsSubstring("keys: sends to /note, which no oscIn"));
}

TEST_CASE("a display shows an address an oscOut on the page port sends", "[ir][layout]") {
    CHECK(errorsOf({element("lamp", "Level", "/level")}).empty());
    CHECK_THAT(errorsOf({element("meter", "Peak", "/peak")}),
               ContainsSubstring("meter \"Peak\": shows /peak, which no oscOut on the page port sends"));
    CHECK_THAT(errorsOf({element("value", "Peak")}), ContainsSubstring("value \"Peak\": needs an address"));
}

TEST_CASE("a control starts within its range", "[ir][layout]") {
    CHECK_THAT(errorsOf({cutoff({{"initial", 800.0}})}), ContainsSubstring("initial 800 is outside 0 to 1"));
    CHECK_THAT(errorsOf({cutoff({{"min", 1.0}, {"max", 1.0}})}),
               ContainsSubstring("min 1 must be below max 1"));
    CHECK_THAT(errorsOf({element("xy", "Pad", "/cutoff", {{"initial", std::vector<double>{0.5}}})}),
               ContainsSubstring("initial must be two numbers, x and y"));
    CHECK_THAT(errorsOf({element("xy", "Pad", "/cutoff", {{"initial", std::vector<double>{0.5, 2}}})}),
               ContainsSubstring("initial 2 is outside 0 to 1"));
    CHECK_THAT(errorsOf({element("select", "Wave", "/cutoff",
                                 {{"options", std::vector<std::string>{"sine", "saw"}}, {"initial", 2.0}})}),
               ContainsSubstring("initial 2 is not the index of an option"));
    CHECK_THAT(errors(piece(page({element("keys", "", "", {{"low", 72.0}, {"high", 60.0}})}), "")),
               ContainsSubstring("low 72 is above high 60"));
}

TEST_CASE("a layout's errors point at the element's definition", "[ir][layout]") {
    auto odd = element("knob", "Q", "/q");
    odd.src = metadata::sourceSpan{.file = "main.lisp", .line = 7, .column = 9, .offset = 0, .length = 0};
    CHECK_THAT(errorsOf({odd}), ContainsSubstring("main.lisp:7:9: layout: page > panel"));
}

TEST_CASE("every kind of the table is drawn by an element espresso-ui.js defines", "[ir][layout]") {
    const std::string library = support::readText(std::filesystem::path(ESPRESSO_SOURCE_DIR) / "src" /
                                                  "apps" / "server" / "espresso-ui.js");
    for (const auto& kind : metadata::pageKinds()) {
        INFO(kind.kind);
        if (kind.kind == metadata::pageRoot) continue;
        const std::string name(kind.element);
        CHECK((library.find('"' + name + "\": ") != std::string::npos ||
               library.find("customElements.define(\"" + name + '"') != std::string::npos));
        for (const auto child : kind.children) CHECK(metadata::findPageKind(child));
    }
}
