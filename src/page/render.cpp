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

// The look of a page without its own: flat panels on a dark ground, after
// the editor's Carbon theme; a project restyles it with the --es-*
// properties (dsl-ui.md §8).
constexpr std::string_view style =
    R"(  :root { color-scheme: dark; --es-accent: #78a9ff; --es-heading: #78a9ff; }
  body { margin: 0; min-height: 100vh; display: grid; place-items: center; background: #161616;
         font: 13px/1.3 system-ui, sans-serif; color: #f4f4f4; }
  .es-page { display: grid; gap: 24px; justify-items: center; padding: 48px 24px 72px; }
  es-panel { background: #262626; border-radius: 8px; box-shadow: 0 16px 48px #0008; column-gap: 0; }
  es-section + es-section { margin-left: 24px; padding-left: 24px; border-left: 1px solid #ffffff1a; }
  .es-title { height: 48px; display: flex; align-items: center; border-bottom: 1px solid #ffffff1a;
              font: 700 18px/1 system-ui, sans-serif; letter-spacing: 0.2em; text-transform: uppercase; }
  .es-status { position: fixed; bottom: 14px; left: 0; right: 0; margin: 0; text-align: center;
               color: #8d8d8d; font-size: 12px; }
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

std::string render(const pageNode& layout) {
    writer w;
    w.element(layout, 1);
    std::string out =
        "<!doctype html>\n"
        "<!-- The piece's page, drawn from its layout (dsl-ui.md §7.1). A project's own\n"
        "     ui/index.html takes its place. -->\n"
        "<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "<title>" +
        escaped(titleOf(layout)) + "</title>\n<style>\n" + std::string(style) +
        "</style>\n</head>\n<body>\n<main class=\"es-page\">\n" + w.text +
        "</main>\n<p class=\"es-status\" role=\"status\">Connecting…</p>\n"
        "<script src=\"/espresso.js\"></script>\n<script src=\"/espresso-ui.js\"></script>\n"
        "<script>\n"
        "  espresso.onOpen(() => { document.querySelector(\".es-status\").textContent = \"Connected\"; });\n"
        "  // The server asks for the page again when an Update changes it.\n"
        "  espresso.on(\"" +
        std::string(reloadAddress) +
        "\", () => location.reload());\n"
        "</script>\n</body>\n</html>\n";
    return out;
}

}  // namespace page
