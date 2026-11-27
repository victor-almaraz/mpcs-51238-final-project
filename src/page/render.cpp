// A page's layout drawn as HTML (dsl-ui.md §7.1): each element as the
// es-* element of its kind, its parameters as the attributes that element
// reads (docs/language.md, Page controls and Page layout).

#include <cmath>
#include <cstddef>
#include <ios>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <espresso/metadata/graph.h>
#include <espresso/metadata/pageSpec.h>
#include <espresso/page/render.h>

namespace page {

namespace {

using metadata::pageNode;
using metadata::paramValue;

// The look of a page without its own: flat panels after the editor's
// Carbon theme, dark (carbon) or light (paper). A project restyles it with
// ui/style.css, which loads after it (dsl-ui.md §8).
constexpr std::string_view style =
    R"(  :root { color-scheme: dark; --ground: #161616; --face: #262626; --ink: #f4f4f4; --dim: #8d8d8d;
          --rule: #ffffff1a; --es-accent: #78a9ff; --es-heading: #78a9ff; }
  body { margin: 0; min-height: 100vh; display: grid; place-items: center; background: var(--ground);
         font: 13px/1.3 system-ui, sans-serif; color: var(--ink); }
  .es-page { display: grid; gap: 24px; justify-items: center; padding: 48px 24px 72px; }
  es-panel { background: var(--face); border-radius: 8px; box-shadow: 0 16px 48px #0008; column-gap: 0; }
  es-panel:not([flow="column"]) es-section + es-section { margin-left: 24px; padding-left: 24px;
                                                          border-left: 1px solid var(--rule); }
  es-panel[flow="column"] { row-gap: 0; }
  es-panel[flow="column"] > .es-title { margin-bottom: 24px; }
  es-panel[flow="column"] es-section + es-section { margin-top: 24px; padding-top: 48px; border-top: 1px solid var(--rule); }
  es-panel[flow="column"] es-section + es-section::before { top: 24px; }
  .es-title { height: 48px; display: flex; align-items: center; border-bottom: 1px solid var(--rule);
              font: 700 18px/1 system-ui, sans-serif; letter-spacing: 0.2em; text-transform: uppercase; }
  es-status { position: fixed; bottom: 14px; left: 0; right: 0; text-align: center; color: var(--dim); font-size: 12px; }
)";

// The paper theme's colors, over the carbon ones.
constexpr std::string_view paper =
    R"(  :root { color-scheme: light; --ground: #f4f4f4; --face: #ffffff; --ink: #161616; --dim: #6f6f6f;
          --rule: #0000001a; --es-accent: #0f62fe; --es-heading: #0f62fe; --es-surface: #e0e0e0;
          --es-line: #c6c6c6; --es-text: #161616; --es-muted: #525252; }
  es-panel { box-shadow: 0 1px 3px #0002; }
)";

void appendNumbers(std::string& out, const std::vector<double>& values, std::string_view separator) {
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) out += separator;
        out += shortest(values[i]);
    }
}

class writer {
public:
    std::string text;

    void element(const pageNode& node, int depth) {
        const auto spec = metadata::findPageKind(node.kind);
        if (!spec) return;
        const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
        if (node.kind == metadata::pageRoot) {
            for (const auto& child : node.children) element(child, depth);
            return;
        }
        const std::string tag(spec->get().element);
        text += indent + '<' + tag;
        attribute("label", node.label);
        attribute("address", node.address);
        for (const auto& [name, value] : node.params) parameter(node, name, value);
        text += '>';
        if (node.kind == "select") {
            options(node, indent);
        } else if (!node.children.empty() || (node.kind == "panel" && !node.label.empty())) {
            text += '\n';
            if (node.kind == "panel" && !node.label.empty())
                text += indent + "  <header class=\"es-title\">" + escaped(node.label) + "</header>\n";
            for (const auto& child : node.children) element(child, depth + 1);
            text += indent;
        }
        text += "</" + tag + ">\n";
    }

private:
    void attribute(std::string_view name, std::string_view value) {
        if (value.empty()) return;
        text += ' ';
        text += name;
        text += "=\"";
        text += escaped(value);
        text += '"';
    }

    void parameter(const pageNode& node, const std::string& name, const paramValue& value) {
        if (node.kind == "select" && name == "options") return;  // its <option>s
        // Only a parameter its kind has: attribute names are never written
        // from data the table does not name.
        const auto kind = metadata::findPageKind(node.kind);
        if (!kind || !kind->get().param(name)) return;
        const std::string attr = name == "initial" ? "value" : name;
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, double>) {
                    attribute(attr,
                              name == "hold" ? shortest(v * 1000) : shortest(v));  // es-meter: milliseconds
                } else if constexpr (std::is_same_v<T, std::string>) {
                    attribute(attr, v);
                } else if constexpr (std::is_same_v<T, std::vector<double>>) {
                    std::string list;
                    appendNumbers(list, v, name == "span" || name == "at" ? " " : ",");
                    attribute(attr, list);
                } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
                    std::string list;
                    for (const auto& item : v) list += (list.empty() ? "" : ",") + item;
                    attribute(attr, list);
                }
            },
            value);
    }

    // A select's choices, as the <option>s es-select reads.
    void options(const pageNode& node, const std::string& indent) {
        const auto it = node.params.find("options");
        if (it == node.params.end() || !std::holds_alternative<std::vector<std::string>>(it->second)) return;
        text += '\n';
        for (const auto& option : std::get<std::vector<std::string>>(it->second))
            text += indent + "  <option>" + escaped(option) + "</option>\n";
        text += indent;
    }
};

// The document's title: its first panel's label.
std::string titleOf(const pageNode& layout) {
    for (const auto& panel : layout.children) {
        if (!panel.label.empty()) return panel.label;
    }
    return "espresso";
}

}  // namespace

std::string escaped(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

std::string shortest(double v) {
    // A whole number below 2^53 is written as one: 110, not 1.1e+02.
    if (v == std::trunc(v) && std::abs(v) < 0x1p53) {
        std::ostringstream out;
        out << std::fixed;
        out.precision(0);
        out << v;
        return out.str();
    }
    for (int digits = 1; digits <= std::numeric_limits<double>::max_digits10; ++digits) {
        std::ostringstream out;
        out.precision(digits);
        out << v;
        std::istringstream back(out.str());
        double read = 0;
        back >> read;
        if (read == v) return out.str();
    }
    std::ostringstream out;
    out.precision(std::numeric_limits<double>::max_digits10);
    out << v;
    return out.str();
}

// The script a served page runs: the server asks for the page again when
// an Update changes its layout, and the value controls keep across the
// reload the values the player set, which they send when the page
// connects.
std::string reloading() {
    return "<script>\n"
           "  espresso.on(\"" +
           std::string(reloadAddress) +
           "\", () => {\n"
           "    const kept = {};\n"
           "    for (const c of document.querySelectorAll(\"[address]\"))\n"
           "      if (typeof c.reset === \"function\") kept[c.getAttribute(\"address\")] = c.value;\n"
           "    sessionStorage.setItem(\"espresso-kept\", JSON.stringify(kept));\n"
           "    location.reload();\n"
           "  });\n"
           "  addEventListener(\"DOMContentLoaded\", () => {\n"
           "    const kept = JSON.parse(sessionStorage.getItem(\"espresso-kept\") || \"{}\");\n"
           "    sessionStorage.removeItem(\"espresso-kept\");\n"
           "    for (const c of document.querySelectorAll(\"[address]\")) {\n"
           "      const address = c.getAttribute(\"address\");\n"
           "      if (typeof c.reset !== \"function\" || !(address in kept)) continue;\n"
           "      c.value = kept[address];\n"
           "      espresso.send(address, ...[].concat(c.value));\n"
           "    }\n"
           "  });\n"
           "</script>\n";
}

std::string render(const pageNode& layout, bool served) {
    writer w;
    w.element(layout, 1);
    std::string look(style);
    const auto option = [&](std::string_view name) -> std::string {
        const auto it = layout.params.find(name);
        return it != layout.params.end() && std::holds_alternative<std::string>(it->second)
                   ? std::get<std::string>(it->second)
                   : std::string();
    };
    const std::string accent = option("accent");
    if (option("theme") == "paper") {
        look += paper;
        // A synth panel's face stays dark: without an accent of the page's,
        // it keeps the light blue its headings read in.
        if (!metadata::isPageColor(accent)) look += "  es-synth { --es-accent: #78a9ff; }\n";
    }
    if (metadata::isPageColor(accent))
        look += "  :root { --es-accent: " + accent + "; --es-heading: " + accent + "; }\n";
    return "<!doctype html>\n"
           "<!-- The piece's page, drawn from its layout (dsl-ui.md §7.1). A project's own\n"
           "     ui/index.html takes its place; ui/style.css and ui/script.js load after it. -->\n"
           "<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
           "<title>" +
           escaped(titleOf(layout)) + "</title>\n<style>\n" + look +
           "</style>\n<link rel=\"stylesheet\" href=\"/style.css\">\n</head>\n<body>\n<main "
           "class=\"es-page\">\n" +
           w.text +
           "</main>\n<es-status></es-status>\n"
           "<script src=\"/espresso.js\"></script>\n<script src=\"/espresso-ui.js\"></script>\n" +
           (served ? reloading() : std::string()) +
           "<script src=\"/script.js\"></script>\n</body>\n</html>\n";
}

}  // namespace page
