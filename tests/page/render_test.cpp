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
    CHECK_THAT(html, ContainsSubstring("espresso.on(\"/espresso/reload\", () => {"));
}

TEST_CASE("a layout draws the same every time", "[page]") {
    CHECK(page::render(synth()) == page::render(synth()));
}

TEST_CASE("a page without a panel's label is titled espresso", "[page]") {
    CHECK_THAT(page::render(element("page", "", "", {}, {element("panel")})),
               ContainsSubstring("<title>espresso</title>"));
}

TEST_CASE("a page names its project, the label of its root, in a meta element", "[page]") {
    const pageNode named = element("page", "auto & band", "", {}, {element("panel", "Synth")});
    CHECK_THAT(page::render(named),
               ContainsSubstring(R"(<meta name="espresso-project" content="auto &amp; band">)"));
    const pageNode unnamed = element("page", "", "", {}, {element("panel", "Synth")});
    CHECK_THAT(page::render(unnamed), !ContainsSubstring("espresso-project"));
}

TEST_CASE("numbers are written in the fewest digits that read back the same", "[page]") {
    CHECK(page::shortest(0.1) == "0.1");
    CHECK(page::shortest(110) == "110");
    CHECK(page::shortest(0.1778279410038923) == "0.1778279410038923");
    CHECK(page::shortest(-1e-7) == "-1e-07");
    CHECK(page::escaped(R"(a "b" & <c>)") == "a &quot;b&quot; &amp; &lt;c&gt;");
}

TEST_CASE("a page's theme and accent restyle it, and its own style and script load after it (dsl-ui.md 8)",
          "[page]") {
    pageNode layout = element("page", "", "", {}, {element("panel", "Synth")});
    const std::string carbon = page::render(layout);
    CHECK_THAT(carbon, ContainsSubstring("color-scheme: dark"));
    CHECK_THAT(carbon, !ContainsSubstring("color-scheme: light"));
    CHECK_THAT(carbon, ContainsSubstring("</style>\n<link rel=\"stylesheet\" href=\"/style.css\">\n</head>"));
    CHECK_THAT(carbon, ContainsSubstring("<script src=\"/script.js\"></script>\n</body>"));
    CHECK_THAT(carbon, ContainsSubstring("<es-status></es-status>"));
    layout.params = {{"theme", std::string("paper")}, {"accent", std::string("#ff8800")}};
    const std::string paper = page::render(layout);
    CHECK_THAT(paper, ContainsSubstring("color-scheme: light"));
    CHECK_THAT(paper, ContainsSubstring(":root { --es-accent: #ff8800; --es-heading: #ff8800; }"));
    // Only a color reaches the style.
    layout.params["accent"] = std::string("red; } body { display: none");
    CHECK_THAT(page::render(layout), !ContainsSubstring("display: none"));
}

TEST_CASE("a served page reloads keeping its values; a page written into a project does not reload",
          "[page]") {
    const pageNode layout = element("page", "", "", {}, {element("panel", "Synth")});
    const std::string served = page::render(layout);
    CHECK_THAT(served, ContainsSubstring("espresso.on(\"/espresso/reload\""));
    CHECK_THAT(served, ContainsSubstring("sessionStorage.setItem(\"espresso-kept\""));
    const std::string written = page::render(layout, false);
    CHECK_THAT(written, !ContainsSubstring("/espresso/reload"));
    CHECK_THAT(written, ContainsSubstring("<script src=\"/script.js\"></script>"));
}

TEST_CASE("only parameters a kind has are written as attributes", "[page]") {
    const pageNode layout =
        element("page", "", "", {},
                {element("panel", "P", "", {},
                         {element("section", "S", "", {},
                                  {element("knob", "K", "/k", {{"onfocus", std::string("x")}})})})});
    CHECK_THAT(page::render(layout), !ContainsSubstring("onfocus"));
}

TEST_CASE("sizes and places are written as attributes of units, a screen as es-screen", "[page]") {
    const pageNode layout = element(
        "page", "", "", {},
        {element(
            "panel", "P", "", {},
            {element("section", "S", "", {{"size", std::vector<double>{9, 4}}},
                     {element("screen", "Tables", "",
                              {{"span", std::vector<double>{9, 3}}, {"at", std::vector<double>{1, 1}}})})})});
    const std::string html = page::render(layout);
    CHECK_THAT(html, ContainsSubstring("<es-section label=\"S\" size=\"9 4\">"));
    CHECK_THAT(html, ContainsSubstring("<es-screen label=\"Tables\" at=\"1 1\" span=\"9 3\"></es-screen>"));
}

TEST_CASE("a page with tabs is es-tabs, each tab a canvas of its own with its routing", "[page]") {
    const pageNode layout = element("page", "", "", {},
                                    {element("tab", "Bass", "", {{"to", std::vector<std::string>{"Master"}}},
                                             {element("panel", "P", "", {}, {})}),
                                     element("tab", "Master", "",
                                             {{"from", std::vector<std::string>{"Bass", "Pad"}},
                                              {"to", std::vector<std::string>{"output"}}},
                                             {element("panel", "Q", "", {}, {})})});
    const std::string html = page::render(layout);
    CHECK_THAT(html, ContainsSubstring("<es-tabs>"));
    CHECK_THAT(html, ContainsSubstring("<es-tab label=\"Bass\" to=\"Master\">"));
    CHECK_THAT(html, ContainsSubstring("<es-tab label=\"Master\" from=\"Bass, Pad\" to=\"output\">"));
    CHECK_THAT(html, ContainsSubstring("<es-canvas name=\"Master\">"));
}
