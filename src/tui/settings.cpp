#include <proj/tui/settings.h>

#include <cmath>
#include <cstdlib>
#include <sstream>
#include <system_error>

#include <proj/interpreter/reader.h>
#include <proj/tui/fileStore.h>

namespace fs = std::filesystem;

namespace tui {

using interpreter::datum;
using interpreter::datumKind;
using interpreter::unitSuffix;

std::string_view toString(keyScheme scheme) noexcept {
    switch (scheme) {
    case keyScheme::emacs: return "emacs";
    case keyScheme::cua: return "cua";
    case keyScheme::vim: return "vim";
    case keyScheme::nano: return "nano";
    }
    return "emacs";
}

std::string_view toString(quantization quant) noexcept {
    switch (quant) {
    case quantization::none: return "none";
    case quantization::beat: return "beat";
    case quantization::bar: return "bar";
    }
    return "none";
}

fs::path settingsPath(const char* xdgConfigHome, const char* home) {
    fs::path base;
    if (xdgConfigHome != nullptr && *xdgConfigHome != '\0' && fs::path(xdgConfigHome).is_absolute()) {
        base = xdgConfigHome;
    } else {
        base = fs::path(home != nullptr ? home : "") / ".config";
    }
    return base / appDirectoryName / "settings.lisp";
}

fs::path settingsPathFromEnvironment() {
    return settingsPath(std::getenv("XDG_CONFIG_HOME"), std::getenv("HOME"));
}

namespace {

std::string where(std::string_view fileName, const datum& d) {
    std::ostringstream out;
    out << fileName << ':' << d.span.line << ':' << d.span.column;
    return out.str();
}

class parser {
public:
    parser(std::string_view fileName, settingsResult& result) : file_(fileName), result_(result) {}

    void error(const datum& at, const std::string& message) {
        result_.errors.push_back(where(file_, at) + ": " + message);
    }

    bool boolean(const datum& value, const std::string& key, bool& out) {
        if (value.kind != datumKind::boolean) {
            error(value, "setting :" + key + " expects #t or #f");
            return false;
        }
        out = value.boolean;
        return true;
    }

    bool positiveInteger(const datum& value, const std::string& key, int& out) {
        if (value.kind != datumKind::integer || value.integer <= 0 || value.integer > 1000) {
            error(value, "setting :" + key + " expects an integer from 1 to 1000");
            return false;
        }
        out = static_cast<int>(value.integer);
        return true;
    }

    bool seconds(const datum& value, const std::string& key, double& out) {
        double s = -1.0;
        if (value.kind == datumKind::integer) {
            s = static_cast<double>(value.integer);
        } else if (value.kind == datumKind::floating) {
            s = value.floating;
        } else if (value.kind == datumKind::quantity) {
            switch (value.quantity.unit) {
            case unitSuffix::s: s = value.quantity.value; break;
            case unitSuffix::ms: s = value.quantity.value * 1e-3; break;
            case unitSuffix::us: s = value.quantity.value * 1e-6; break;
            default: break;
            }
        }
        if (!(s >= 0.0) || !std::isfinite(s)) {
            error(value, "setting :" + key + " expects a non-negative time, such as 50ms");
            return false;
        }
        out = s;
        return true;
    }

    void entry(const datum& keyDatum, const datum& value) {
        const std::string& key = keyDatum.text;
        settings& s = result_.values;
        if (key == "keys") {
            const std::string name = value.kind == datumKind::keyword ? value.text : "";
            if (name == "emacs") s.keys = keyScheme::emacs;
            else if (name == "cua") s.keys = keyScheme::cua;
            else if (name == "vim") s.keys = keyScheme::vim;
            else if (name == "nano") s.keys = keyScheme::nano;
            else error(value, "setting :keys expects :emacs, :cua, :vim, or :nano");
        } else if (key == "quant") {
            const std::string name = value.kind == datumKind::keyword ? value.text : "";
            if (name == "none") s.quant = quantization::none;
            else if (name == "beat") s.quant = quantization::beat;
            else if (name == "bar") s.quant = quantization::bar;
            else error(value, "setting :quant expects :none, :beat, or :bar");
        } else if (key == "theme") {
            if (value.kind == datumKind::keyword) {
                s.theme = value.text;  // the editor checks the name against its themes
            } else {
                error(value, "setting :theme expects a theme name, such as :adwaita-dark");
            }
        } else if (key == "file-list") {
            boolean(value, key, s.fileList);
        } else if (key == "console") {
            boolean(value, key, s.console);
        } else if (key == "rebuild-on-save") {
            boolean(value, key, s.rebuildOnSave);
        } else if (key == "fade") {
            seconds(value, key, s.fade);
        } else if (key == "file-list-width") {
            positiveInteger(value, key, s.fileListWidth);
        } else if (key == "console-height") {
            positiveInteger(value, key, s.consoleHeight);
        } else {
            error(keyDatum, "unknown setting :" + key + " in " + std::string(file_));
        }
    }

private:
    std::string_view file_;
    settingsResult& result_;
};

} // namespace

settingsResult parseSettings(std::string_view text, std::string_view fileName) {
    settingsResult result;
    const auto read = interpreter::read(text);
    if (read.error) {
        std::ostringstream out;
        out << fileName << ':' << read.error->span.line << ':' << read.error->span.column << ": "
            << read.error->message;
        result.errors.push_back(out.str());
        return result;
    }
    parser p(fileName, result);
    for (const datum& form : read.data) {
        if (form.kind != datumKind::list || form.items.empty() || !form.items[0].isSymbol("settings")) {
            p.error(form, "expected a (settings ...) form");
            continue;
        }
        const auto& items = form.items;
        for (std::size_t i = 1; i < items.size(); i += 2) {
            if (items[i].kind != datumKind::keyword) {
                p.error(items[i], "expected a setting name such as :keys");
                continue;
            }
            if (i + 1 >= items.size()) {
                p.error(items[i], "setting :" + items[i].text + " has no value");
                break;
            }
            p.entry(items[i], items[i + 1]);
        }
    }
    return result;
}

settingsResult loadSettings(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return {};
    const auto file = fileStore::read(path);
    if (!file.text) {
        settingsResult result;
        result.errors.push_back(file.error);
        return result;
    }
    return parseSettings(*file.text, path.string());
}

std::string formatSettings(const settings& v) {
    auto flag = [](bool b) { return b ? "#t" : "#f"; };
    std::ostringstream out;
    const double ms = std::round(v.fade * 1000.0 * 1000.0) / 1000.0;
    out << "(settings\n"
        << "  :keys            :" << toString(v.keys) << '\n'
        << "  :theme           :" << v.theme << '\n'
        << "  :file-list       " << flag(v.fileList) << '\n'
        << "  :console         " << flag(v.console) << '\n'
        << "  :fade            " << ms << "ms\n"
        << "  :quant           :" << toString(v.quant) << '\n'
        << "  :rebuild-on-save " << flag(v.rebuildOnSave) << '\n'
        << "  :file-list-width " << v.fileListWidth << '\n'
        << "  :console-height  " << v.consoleHeight << ")\n";
    return out.str();
}

std::string saveSettings(const fs::path& path, const settings& values) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return "cannot create " + path.parent_path().string() + ": " + ec.message();
    const auto error = fileStore::writeAtomic(path, formatSettings(values));
    return error.value_or("");
}

} // namespace tui
