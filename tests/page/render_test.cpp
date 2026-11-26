// A page's layout drawn as HTML (dsl-ui.md §7.1): each element as its
// kind's es-* element, its parameters as the attributes the element reads.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/page/render.h>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using metadata::pageNode;
using params = decltype(pageNode::params);

namespace {

pageNode element(std::string kind, std::string label = "", std::string address = "", params p = {},
                 std::vector<pageNode> children = {}) {
    return {.kind = std::move(kind),
            .label = std::move(label),
            .address = std::move(address),
            .params = std::move(p),
            .children = std::move(children),
            .src = std::nullopt};
}

pageNode synth() {
    return element(
        "page", "", "", {},
        {element("panel", "WAVETABLE", "", {},
                 {element("section", "Oscillators", "", {},
                          {element("knob", "Pitch", "/oscillators/pitch",
                                   {{"min", 55.0},
                                    {"max", 440.0},
                                    {"initial", 110.0},
                                    {"scale", std::string("exp")},
                                    {"unit", std::string("hz")},
                                    {"size", std::string("big")}})}),
                  element("section", "Sweep", "", {{"columns", 9.0}},
                          {element("xy", "Pad", "/sweep/pad",
                                   {{"initial", std::vector<double>{0.5, 0.5}},
                                    {"span", std::vector<double>{8, 8}}}),
                           element("select", "Wave & <Shape>", "/sweep/wave",
                                   {{"options", std::vector<std::string>{"sine", "saw"}}, {"initial", 1.0}}),
                           element("meter", "Level", "/sweep/level",
                                   {{"hold", 0.5}, {"orientation", std::string("vertical")}})})})});
}

}  // namespace

TEST_CASE("a layout draws as es-* elements with their attributes (dsl-ui.md 7.1)", "[page]") {
    const std::string html = page::render(synth());
    CHECK_THAT(html, StartsWith("<!doctype html>\n"));
    CHECK_THAT(html, ContainsSubstring("<title>WAVETABLE</title>"));
    CHECK_THAT(html, ContainsSubstring("<es-panel label=\"WAVETABLE\">\n"
                                       "    <header class=\"es-title\">WAVETABLE</header>\n"
                                       "    <es-section label=\"Oscillators\">\n"));
    CHECK_THAT(html,
               ContainsSubstring("<es-knob label=\"Pitch\" address=\"/oscillators/pitch\" value=\"110\" "
                                 "max=\"440\" min=\"55\" scale=\"exp\" size=\"big\" unit=\"hz\"></es-knob>"));
    CHECK_THAT(html, ContainsSubstring("<es-section label=\"Sweep\" columns=\"9\">"));
    // A pad's starting value is x,y; a place is two numbers.
    CHECK_THAT(html, ContainsSubstring(
                         "<es-xy label=\"Pad\" address=\"/sweep/pad\" value=\"0.5,0.5\" span=\"8 8\">"));
    // A select's choices are its options; text is escaped.
    CHECK_THAT(html,
               ContainsSubstring("<es-select label=\"Wave &amp; &lt;Shape&gt;\" address=\"/sweep/wave\" "
                                 "value=\"1\">\n        <option>sine</option>\n"));
    // A meter holds milliseconds.
    CHECK_THAT(html, ContainsSubstring("<es-meter label=\"Level\" address=\"/sweep/level\" hold=\"500\" "
                                       "orientation=\"vertical\"></es-meter>"));
    CHECK_THAT(html,
               ContainsSubstring("<script src=\"/espresso.js\"></script>\n<script src=\"/espresso-ui.js\">"));
    CHECK_THAT(html, ContainsSubstring("espresso.on(\"/espresso/reload\", () => location.reload());"));
}

TEST_CASE("a layout draws the same every time", "[page]") {
    CHECK(page::render(synth()) == page::render(synth()));
}

TEST_CASE("a page without a panel's label is titled espresso", "[page]") {
    CHECK_THAT(page::render(element("page", "", "", {}, {element("panel")})),
               ContainsSubstring("<title>espresso</title>"));
}

TEST_CASE("numbers are written in the fewest digits that read back the same", "[page]") {
    CHECK(page::shortest(0.1) == "0.1");
    CHECK(page::shortest(110) == "110");
    CHECK(page::shortest(0.1778279410038923) == "0.1778279410038923");
    CHECK(page::shortest(-1e-7) == "-1e-07");
    CHECK(page::escaped(R"(a "b" & <c>)") == "a &quot;b&quot; &amp; &lt;c&gt;");
}
