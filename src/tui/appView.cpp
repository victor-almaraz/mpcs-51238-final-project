// Rendering of the editor: header bar, panes, popovers, dialogs, and
// overlays. State and input live in app.cpp.

#include <proj/tui/app.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>

#include <ftxui/screen/terminal.hpp>

#include <proj/tui/editorView.h>
#include <proj/tui/utf8.h>

namespace fs = std::filesystem;
using namespace ftxui;

namespace tui {

// ---------------------------------------------------------------------
// Rendering
//
// The layout adapts the GNOME HIG to a terminal: a header bar with the
// run controls, the document title, and a main menu at the end; a
// sidebar and a console pane beside the content, with their headings on
// the same rows as the tab bar; popovers for menus; centered alert,
// preferences, shortcuts, and about dialogs. Colors are the terminal's
// named palette, so the terminal theme (light, dark, high contrast)
// applies.

namespace {

// Below this width the sidebar collapses and the header bar drops its
// labels (adaptive layout).
constexpr int narrowColumns = 72;

// Styles by role, for the active theme. Views ask for a role and never
// for a literal color, so every theme applies everywhere.
struct palette {
    const theme& t;

    Element suggested(Element e) const { return e | bgcolor(t.accent) | color(t.onAccent) | bold; }
    Element destructive(Element e) const {
        return e | bgcolor(t.destructive) | color(t.onDestructive) | bold;
    }
    Element field(Element e) const { return e | bgcolor(t.field) | color(t.fieldText); }
    Element selected(Element e) const {
        return e | bgcolor(t.selection) | color(t.selectionText) | bold;
    }
    Element secondary(Element e) const {
        return t.faintSecondary ? e | dim : e | color(t.secondary);
    }
    Element rule(Element e) const { return e | color(t.rule); }

    // Raised surfaces: every popover, dialog, overlay, and the open prompt
    // line has its own background, border included, so it stands apart
    // from the window behind it. Controls on a surface keep their
    // contrast against it.
    Element surface(Element e) const { return e | color(t.surfaceText) | bgcolor(t.surface); }
    Element surfaceButton(Element e) const {
        return e | bgcolor(t.surfaceField) | color(t.surfaceFieldText);
    }
    Element surfaceSecondary(Element e) const { return e | color(t.surfaceSecondary); }
    Element surfaceRule(Element e) const { return e | color(t.surfaceRule); }
    Decorator border() const { return borderStyled(ROUNDED, t.surfaceRule); }
};

// Clears a one-cell margin around a bordered box, so that its border
// never joins the lines of the content behind it.
Element margin(Element box, Color background) {
    return vbox({text(""), hbox({text(" "), std::move(box), text(" ")}), text("")}) |
           bgcolor(background) | clear_under;
}

// Popovers hang below their trigger, so their margin covers the sides
// and bottom only; the row above stays visible.
Element popoverMargin(Element box, Color background) {
    return vbox({hbox({text(" "), std::move(box), text(" ")}), text("")}) | bgcolor(background) |
           clear_under;
}

// A modal box centered over the window.
Element centered(Element box, Color background) {
    return vbox({filler(), hbox({filler(), margin(std::move(box), background), filler()}),
                 filler()});
}

std::string titleCase(std::string_view word) {
    std::string out(word);
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

// Key sequences for display. Emacs and vim users read their own notation;
// the other schemes use the "Ctrl+S" style of GNOME documentation.
std::string displayKeys(std::string_view keys, keyScheme scheme) {
    if (scheme == keyScheme::emacs || scheme == keyScheme::vim) return std::string(keys);
    std::string out;
    std::size_t start = 0;
    while (start < keys.size()) {
        std::size_t end = keys.find(' ', start);
        if (end == std::string_view::npos) end = keys.size();
        std::string_view k = keys.substr(start, end - start);
        std::string mods;
        while (k.size() > 2 && k[1] == '-' && (k[0] == 'C' || k[0] == 'M' || k[0] == 'S')) {
            mods += k[0] == 'C' ? "Ctrl+" : k[0] == 'M' ? "Alt+" : "Shift+";
            k.remove_prefix(2);
        }
        std::string name;
        if (k.size() > 2 && k.front() == '<' && k.back() == '>') {
            const std::string_view n = k.substr(1, k.size() - 2);
            if (n == "prior") name = "Page Up";
            else if (n == "next") name = "Page Down";
            else if (n == "left") name = "←";
            else if (n == "right") name = "→";
            else if (n == "up") name = "↑";
            else if (n == "down") name = "↓";
            else if (n[0] == 'f') name = "F" + std::string(n.substr(1));
            else name = titleCase(n);
        } else if (k == "RET") {
            name = "Enter";
        } else if (k == "DEL") {
            name = "Backspace";
        } else if (k == "TAB") {
            name = "Tab";
        } else if (k == "ESC") {
            name = "Esc";
        } else if (k == "SPC") {
            name = "Space";
        } else {
            name = mods.empty() ? std::string(k) : titleCase(k);
        }
        if (!out.empty()) out += ' ';
        out += mods + name;
        start = end + 1;
    }
    return out;
}

// Header-capitalized names of commands, for menus and the shortcuts
// dialog.
std::string_view commandLabel(std::string_view command) {
    static const std::map<std::string_view, std::string_view> labels{
        {"openFile", "Open File…"},          {"switchBuffer", "Switch Buffer…"},
        {"save", "Save"},                    {"saveAs", "Save As…"},
        {"closeBuffer", "Close Tab"},        {"nextBuffer", "Next Tab"},
        {"previousBuffer", "Previous Tab"},  {"quit", "Quit"},
        {"undo", "Undo"},                    {"redo", "Redo"},
        {"cut", "Cut"},                      {"copy", "Copy"},
        {"paste", "Paste"},                  {"selectAll", "Select All"},
        {"setMark", "Set Mark"},             {"killLine", "Delete to End of Line"},
        {"searchForward", "Find…"},          {"searchBackward", "Find Backward…"},
        {"searchNext", "Find Next"},         {"searchPrevious", "Find Previous"},
        {"play", "Play"},                    {"stop", "Stop"},
        {"evalLastForm", "Evaluate Form"},   {"toggleFileList", "Show File List"},
        {"toggleConsole", "Show Console"},   {"mainMenu", "Main Menu"},
        {"contextMenu", "Context Menu"},     {"showPreferences", "Preferences"},
        {"showShortcuts", "Keyboard Shortcuts"},
    };
    const auto it = labels.find(command);
    return it == labels.end() ? command : it->second;
}

struct shortcutGroup {
    std::string_view title;
    std::vector<std::string_view> commands;
};

const std::vector<shortcutGroup>& shortcutGroups() {
    static const std::vector<shortcutGroup> groups{
        {"Files", {"openFile", "switchBuffer", "save", "saveAs", "closeBuffer", "nextBuffer",
                   "previousBuffer", "quit"}},
        {"Editing", {"undo", "redo", "cut", "copy", "paste", "selectAll", "setMark", "killLine"}},
        {"Find", {"searchForward", "searchBackward", "searchNext", "searchPrevious"}},
        {"Run", {"play", "stop", "evalLastForm"}},
        {"View", {"toggleFileList", "toggleConsole", "mainMenu", "contextMenu",
                  "showPreferences", "showShortcuts"}},
    };
    return groups;
}

} // namespace

// The window's width from the last layout, or the terminal's before the
// first one.
int app::windowWidth() const {
    const int width = rootBox_.x_max - rootBox_.x_min + 1;
    return width > 1 ? width : Terminal::Size().dimx;
}

bool app::narrow() const { return windowWidth() < narrowColumns; }

Element app::clickable(Element e, std::function<void(const Mouse&)> press,
                       std::function<void(int)> wheel, std::function<void(const Mouse&)> drag,
                       std::string tooltip) {
    targets_.push_back({{}, std::move(press), std::move(drag), std::move(wheel), clipTo_,
                        std::move(tooltip)});
    return e | reflect(targets_.back().box);
}

std::string app::keyHint(std::string_view command) const {
    const keyScheme scheme = keys_.scheme();
    const editMode mode = scheme == keyScheme::vim ? editMode::normal : editMode::insert;
    for (const auto& b : schemeBindings(scheme, mode)) {
        if (b.command == command) return displayKeys(b.keys, scheme);
    }
    if (scheme == keyScheme::vim) {
        for (const auto& e : exCommands()) {
            if (e.command == command) return ":" + std::string(e.name);
        }
    }
    for (const auto& b : commonBindings()) {
        if (b.command == command) return displayKeys(b.keys, scheme);
    }
    return {};
}

app::menuEntry app::item(std::string label, const char* command) {
    return {std::move(label), [this, command] { runCommand(command); }, keyHint(command), {}};
}

// The main menu holds app-wide actions; Preferences, Keyboard Shortcuts,
// and About come last. Quit is the header bar's close button.
std::vector<app::menuEntry> app::mainMenu() {
    menuEntry fileList = item("Show File List", "toggleFileList");
    fileList.checked = settings_.fileList;
    menuEntry consolePane = item("Show Console", "toggleConsole");
    consolePane.checked = settings_.console;
    return {item("Open File…", "openFile"),
            item("Switch Buffer…", "switchBuffer"),
            item("Save", "save"),
            item("Save As…", "saveAs"),
            item("Close Tab", "closeBuffer"),
            {},
            item("Find…", "searchForward"),
            item("Evaluate Form", "evalLastForm"),
            {},
            fileList,
            consolePane,
            {},
            item("Preferences", "showPreferences"),
            item("Keyboard Shortcuts", "showShortcuts"),
            item("About proj", "showAbout")};
}

std::vector<app::menuEntry> app::contextMenu() {
    return {item("Undo", "undo"),
            item("Redo", "redo"),
            {},
            item("Cut", "cut"),
            item("Copy", "copy"),
            item("Paste", "paste"),
            item("Select All", "selectAll"),
            {},
            item("Find…", "searchForward"),
            item("Find Backward…", "searchBackward"),
            item("Find Next", "searchNext"),
            item("Find Previous", "searchPrevious"),
            {},
            item("Evaluate Form", "evalLastForm")};
}

void app::openMenu(std::vector<menuEntry> entries, int x, int y, bool alignRight) {
    menu_ = std::move(entries);
    menuX_ = x;
    menuY_ = y;
    menuAlignRight_ = alignRight;
    menuFocus_ = 0;
}

Element app::renderMenu() {
    [[maybe_unused]] const palette S{*theme_};
    clipTo_ = &menuBox_;
    std::size_t width = 0;
    for (const auto& e : *menu_) {
        width = std::max(width, utf8::length(e.label) + utf8::length(e.hint) + 6);
    }
    Elements rows;
    for (std::size_t i = 0; i < menu_->size(); ++i) {
        const menuEntry& entry = (*menu_)[i];
        if (entry.label.empty()) {
            rows.push_back(S.surfaceRule(separatorLight()));
            continue;
        }
        const std::string check = entry.checked ? (*entry.checked ? "✓ " : "  ") : "  ";
        const bool focused = i == menuFocus_;
        Element hint = text(entry.hint.empty() ? "" : "  " + entry.hint);
        Element row = hbox({text(check), text(entry.label), filler(),
                            focused ? hint : S.surfaceSecondary(hint), text(" ")});
        if (focused) row = S.suggested(row);  // accent highlight across the whole row
        rows.push_back(clickable(row, [this, i](const Mouse&) {
            menuFocus_ = i;
            handleMenuKey("RET");
        }));
    }
    clipTo_ = nullptr;
    const int w = static_cast<int>(width) + 2;  // plus the border
    Element popup = S.surface(vbox(std::move(rows)) | size(WIDTH, EQUAL, w - 2) | S.border()) |
                    clear_under | reflect(menuBox_);
    // The margin starts one cell left of the popover.
    const int x = std::max((menuAlignRight_ ? menuX_ - w + 1 : menuX_) - 1, 0);
    return vbox({
        text("") | size(HEIGHT, EQUAL, std::max(menuY_, 0)),
        hbox({text("") | size(WIDTH, EQUAL, x), popoverMargin(popup, S.t.window), filler()}),
        filler(),
    });
}

// Width in cells of a row of text pieces.
static int cells(std::initializer_list<std::string_view> pieces) {
    std::size_t n = 0;
    for (auto p : pieces) n += utf8::length(p);
    return static_cast<int>(n);
}

Element app::renderHeaderBar() {
    [[maybe_unused]] const palette S{*theme_};
    const bool compact = narrow();
    const std::string play = " ▶ Play ";
    const std::string stopLabel = " ■ Stop ";
    Elements start{
        clickable(S.suggested(text(play)), [this](const Mouse&) { runCommand("play"); }, {}, {},
                  "Compile and play the project · " + keyHint("play")),
        text(" "),
        clickable(text(stopLabel), [this](const Mouse&) { runCommand("stop"); }, {}, {},
                  "Stop after the fade · " + keyHint("stop")),
    };
    const int startWidth = cells({play, " ", stopLabel});

    const std::string fadeLabel = compact ? "" : "Fade ";
    const std::string fadeValue = " " + formatMilliseconds(settings_.fade, true) + " ";
    const std::string quantLabel = compact ? "" : "Quantize ";
    const std::string quantValue = " " + titleCase(toString(settings_.quant)) + " ▾ ";
    // ≡ rather than ☰: U+2630 is double width in Unicode 16 and would
    // misalign the row in terminals that follow it.
    const std::string menuButton = " ≡ ";
    const std::string closeButton = " × ";
    Elements end{
        S.secondary(text(fadeLabel)),
        clickable(
            S.field(text(fadeValue)),
            [this](const Mouse&) {
                openPrompt(promptKind::fade, "Fade (ms): ", formatMilliseconds(settings_.fade));
            },
            [this](int dir) { setFade(settings_.fade - dir * 0.01); }, {},
            "Swap crossfade · click to type, scroll to adjust"),
        text("  "),
        S.secondary(text(quantLabel)),
        clickable(
            S.field(text(quantValue)),
            [this](const Mouse& m) {
                std::vector<menuEntry> entries;
                for (auto q : {quantization::none, quantization::beat, quantization::bar}) {
                    entries.push_back({titleCase(toString(q)),
                                       [this, q] {
                                           settings_.quant = q;
                                           persistSettings();
                                       },
                                       {},
                                       settings_.quant == q});
                }
                openMenu(std::move(entries), m.x, m.y + 2);  // below the header bar's rule
            },
            {}, {}, "Grid that swaps wait for"),
        text("   "),
        clickable(text(menuButton), [this](const Mouse&) { runCommand("mainMenu"); }, {}, {},
                  "Main menu · " + keyHint("mainMenu")),
        clickable(text(closeButton), [this](const Mouse&) { runCommand("quit"); }, {}, {},
                  "Quit · " + keyHint("quit")),
    };
    const int endWidth = cells({fadeLabel, fadeValue, "  ", quantLabel, quantValue, "   ",
                                menuButton, closeButton});

    // The title names the document, centered, with the project as the
    // subtitle. It shows only when it fits between the two groups.
    const buffer& b = *buffers_[current_];
    const std::string title = displayName(b) + (b.dirty() ? " •" : "");
    const std::string subtitle = "  " + root_.filename().string();
    const int titleWidth = cells({title, subtitle});
    Element titleRow = text("");
    if (2 * std::max(startWidth, endWidth) + titleWidth + 4 <= windowWidth()) {
        titleRow = hbox({filler(), text(title) | bold, S.secondary(text(subtitle)), filler()});
    }
    return dbox({titleRow, hbox({hbox(std::move(start)), filler(), hbox(std::move(end))})});
}

Element app::renderSidebar() {
    [[maybe_unused]] const palette S{*theme_};
    clipTo_ = &fileListBox_;
    Elements rows;
    const fs::path currentPath = current().path();
    const std::size_t first = std::min(fileListScroll_, projectFiles_.size());
    auto wheel = [this](int dir) {
        if (dir < 0 && fileListScroll_ > 0) --fileListScroll_;
        if (dir > 0 && fileListScroll_ + 1 < projectFiles_.size()) ++fileListScroll_;
    };
    for (std::size_t i = first; i < projectFiles_.size(); ++i) {
        const fs::path& f = projectFiles_[i];
        Element row = hbox({text(" " + f.string()), filler()});
        if (resolve(f) == currentPath) row = S.selected(row);
        rows.push_back(clickable(row, [this, f](const Mouse&) { openFile(f); }, wheel));
    }
    clipTo_ = nullptr;
    Element list = rows.empty()
                       ? vbox({text(""), text("No Files") | bold | hcenter,
                               S.secondary(paragraphAlignCenter("Open a file to start")), filler()})
                       : vbox(std::move(rows));
    // The heading row lines up with the tab bar, and its rule with the
    // rule under the tabs.
    return vbox({hbox({text(" " + root_.filename().string()) | bold, filler()}), S.rule(separator()),
                 list | yframe | flex}) |
           size(WIDTH, EQUAL, settings_.fileListWidth) | reflect(fileListBox_);
}

Element app::renderTabBar() {
    [[maybe_unused]] const palette S{*theme_};
    clipTo_ = &tabBarBox_;
    Elements tabs;
    for (std::size_t i = 0; i < buffers_.size(); ++i) {
        const buffer& b = *buffers_[i];
        Element name = clickable(text(" " + displayName(b) + (b.dirty() ? " •" : "") + " "),
                                 [this, i](const Mouse&) { selectBuffer(i); });
        Element close = clickable(text("× "), [this, i](const Mouse&) { closeBuffer(i); }, {}, {},
                                  "Close tab · " + keyHint("closeBuffer"));
        Element tab = hbox({name, close});
        tabs.push_back(i == current_ ? S.selected(tab) : S.secondary(tab));
        tabs.push_back(text(" "));
    }
    tabs.push_back(clickable(text(" + "), [this](const Mouse&) { runCommand("openFile"); }, {}, {},
                             "Open a file in a new tab · " + keyHint("openFile")));
    clipTo_ = nullptr;
    tabs.push_back(filler());
    return hbox(std::move(tabs)) | reflect(tabBarBox_);
}

Element app::renderEditorPane() {
    [[maybe_unused]] const palette S{*theme_};
    buffer& b = current();
    const int rows = editorRows();
    const int columns = editorColumns();
    if (followCursor_) {
        ensureCursorVisible(b, rows, columns - gutterWidth(b));
        followCursor_ = false;
    }
    const bool focused = !minibuffer_.active() && !dialog_ && overlay_ == overlayKind::none;
    Element view = renderEditor(b, highlights(b), rows, columns, {focused, true, theme_});
    auto at = [this](const Mouse& m) {
        return hitTest(current(), m.y - editorBox_.y_min, m.x - editorBox_.x_min);
    };
    targets_.push_back(
        {{},
         [this, at](const Mouse& m) {
             buffer& cur = current();
             cur.setCursor(at(m));
             cur.setMark();
             shiftSelecting_ = true;
         },
         [this, at](const Mouse& m) {
             current().setCursor(at(m));
             if (m.y < editorBox_.y_min) current().moveUp();
             if (m.y > editorBox_.y_max) current().moveDown();
             followCursor_ = true;
         },
         [this](int dir) {
             buffer& cur = current();
             const std::size_t step = 3;
             if (dir < 0) cur.setTopLine(cur.topLine() - std::min(step, cur.topLine()));
             if (dir > 0) {
                 cur.setTopLine(std::min(cur.topLine() + step, cur.lineCount() - 1));
             }
         },
         nullptr,
         {}});
    // yframe clips rows beyond the pane after a resize.
    return view | yframe | reflect(targets_.back().box) | reflect(editorBox_) | flex;
}

Element app::renderConsole() {
    [[maybe_unused]] const palette S{*theme_};
    // Pane heading; dragging it resizes the console.
    auto drag = [this](const Mouse& m) {
        settings_.consoleHeight = std::clamp(consoleBox_.y_max - m.y, 1, 200);
        persistOnRelease_ = true;
    };
    Element clear = clickable(text(" Clear "), [this](const Mouse&) { runCommand("clearConsole"); },
                              {}, {}, "Remove all console messages");
    Element heading = hbox({S.rule(text("─ ")), text("Console") | bold, text(" "),
                            S.rule(separatorCharacter("─")) | flex, clear, S.rule(text("─"))});
    heading = clickable(heading, [](const Mouse&) {}, {}, drag, "Drag to resize the console");

    clipTo_ = &consoleBox_;
    const auto& entries = console_.entries();
    const int height = std::max(settings_.consoleHeight, 1);
    const std::size_t visible = static_cast<std::size_t>(height);
    const std::size_t end = entries.size() - std::min(consoleScroll_, entries.size());
    const std::size_t begin = end > visible ? end - visible : 0;
    auto wheel = [this](int dir) {
        if (dir < 0 && consoleScroll_ + 1 < console_.size()) ++consoleScroll_;
        if (dir > 0 && consoleScroll_ > 0) --consoleScroll_;
    };
    Elements rows;
    for (std::size_t i = begin; i < end; ++i) {
        const consoleEntry& e = entries[i];
        // Errors carry a mark as well as a color.
        Element row = text((e.kind == consoleKind::error ? " ✕ " : "   ") + e.text);
        switch (e.kind) {
        case consoleKind::error: row = row | color(S.t.error); break;
        case consoleKind::server: row = row | color(S.t.info); break;
        case consoleKind::info: break;
        }
        if (e.location) {
            const sourceLocation loc = *e.location;
            row = clickable(hbox({row | underlined, filler()}),
                            [this, loc](const Mouse&) { jumpTo(loc); }, wheel, {},
                            "Go to " + loc.path.string() + ":" + std::to_string(loc.line));
        } else {
            row = clickable(row, {}, wheel);
        }
        rows.push_back(row);
    }
    clipTo_ = nullptr;
    Element body = rows.empty() ? vbox({filler(), S.secondary(text("No Messages")) | hcenter, filler()})
                                : vbox(std::move(rows));
    return vbox({heading, body | yframe | size(HEIGHT, EQUAL, height) | reflect(consoleBox_)});
}

Element app::renderStatusBar() {
    [[maybe_unused]] const palette S{*theme_};
    const serverStatus& s = status_;
    // The dot's shape and the text both carry the state, not color alone.
    const std::string server = s.connected ? s.server : "Not connected";
    Element state = hbox({s.connected ? text(" ● ") | color(S.t.success) : S.secondary(text(" ○ ")),
                          text(server)});

    std::ostringstream detail;
    detail << "   " << s.bpm << " BPM · Beat ";
    if (s.beat) {
        detail << *s.beat;
    } else {
        detail << "—";
    }
    if (s.connected) detail << " · Plan " << (s.activePlan != 0 ? std::to_string(s.activePlan) : "—");
    detail << " · Missed " << s.missed << " · Xruns " << s.xruns << " · Drops " << s.drops;

    const buffer& b = *buffers_[current_];
    std::ostringstream right;
    right << titleCase(toString(keys_.scheme()));
    if (keys_.scheme() == keyScheme::vim) right << " " << titleCase(toString(keys_.mode()));
    right << " · Ln " << b.cursor().line + 1 << ", Col " << b.cursor().column + 1 << ' ';

    // Drop the counters rather than truncate the connection state.
    const bool fits = cells({"   ", server, detail.str(), right.str()}) + 2 <= windowWidth();
    return hbox({state, fits ? S.secondary(text(detail.str())) : text(""), filler(),
                 S.secondary(text(right.str()))});
}

Element app::renderMessageLine() {
    [[maybe_unused]] const palette S{*theme_};
    clipTo_ = &messageBox_;
    Element line = [this, &S]() -> Element {
        if (minibuffer_.active()) {
            const std::string& t = minibuffer_.text();
            const std::size_t at = utf8::byteOffset(t, minibuffer_.cursor());
            const std::size_t len =
                at < t.size() ? utf8::sequenceLength(static_cast<unsigned char>(t[at])) : 0;
            Elements parts{text(" " + minibuffer_.prompt()) | bold, text(t.substr(0, at)),
                           text(len ? t.substr(at, len) : " ") | inverted,
                           text(at + len < t.size() ? t.substr(at + len) : "")};
            const auto matches = minibuffer_.matches();
            if (!matches.empty() && promptKind_ != promptKind::search) {
                parts.push_back(text("   "));
                std::size_t shown = 0;
                for (const auto& m : matches) {
                    if (++shown > 12) {
                        parts.push_back(S.surfaceSecondary(text(" …")));
                        break;
                    }
                    parts.push_back(clickable(S.surfaceSecondary(text(m)), [this, m](const Mouse&) {
                        minibuffer_.setText(m);
                        submitPrompt();
                    }));
                    parts.push_back(text("  "));
                }
            }
            parts.push_back(filler());
            return S.surface(hbox(std::move(parts)));  // an open prompt is a surface too
        }
        const std::string pending = keys_.pendingKeys();
        if (!message_.empty()) return text(" " + message_);
        if (!pending.empty()) return text(" " + pending + "-");
        return S.secondary(text(" " + hoverTip_));
    }();
    clipTo_ = nullptr;
    return line | reflect(messageBox_);
}

// An alert dialog: heading, body, and responses with Cancel first, the
// destructive response in red, and the default response highlighted.
Element app::renderDialog() {
    [[maybe_unused]] const palette S{*theme_};
    clipTo_ = &dialogBox_;
    auto respond = [this](char c) { return [this, c](const Mouse&) { answerDialog(c); }; };
    Element cancel = clickable(S.surfaceButton(text("  Cancel  ")), respond('c'), {}, {},
                               "Keep editing · Esc");
    Element discard = clickable(S.destructive(text("  Discard  ")), respond('d'), {}, {},
                                "Close without saving · D");
    Element saveButton = clickable(S.suggested(text("  Save  ")), respond('s'), {}, {},
                                   "Save, then close · Enter");
    clipTo_ = nullptr;
    Element box = vbox({
                      text(""),
                      text(dialog_->heading) | bold | hcenter,
                      text(""),
                      hbox({text("  "), paragraphAlignCenter(dialog_->body) | flex, text("  ")}),
                      text(""),
                      hbox({filler(), cancel, text("  "), discard, text("  "), saveButton, filler()}),
                      text(""),
                  }) |
                  size(WIDTH, EQUAL, 54);
    box = S.surface(box | S.border()) | clear_under | reflect(dialogBox_);
    return centered(box, S.t.window);
}

std::vector<app::preferenceRow> app::preferenceRows() {
    auto cycle = [](auto value, std::initializer_list<decltype(value)> all, int dir) {
        std::vector<decltype(value)> v(all);
        const auto it = std::find(v.begin(), v.end(), value);
        const auto i = static_cast<int>(it - v.begin());
        const int n = static_cast<int>(v.size());
        return v[static_cast<std::size_t>(((i + dir) % n + n) % n)];
    };
    auto toggle = [this](bool& flag) {
        return [this, &flag](int) {
            flag = !flag;
            persistSettings();
        };
    };
    auto switchText = [](bool on) { return std::string(on ? "━━● On " : "●━━ Off"); };
    return {
        {"Appearance", "Theme", std::string(theme_->title) + " ▾",
         [this](int dir) {
             const auto all = themes();
             const auto n = static_cast<int>(all.size());
             const auto i = static_cast<int>(theme_ - all.data());
             setTheme(all[static_cast<std::size_t>(((i + dir) % n + n) % n)]);
         }},
        {"Keyboard", "Key Bindings", titleCase(toString(settings_.keys)) + " ▾",
         [this, cycle](int dir) {
             setKeyScheme(cycle(settings_.keys,
                                {keyScheme::emacs, keyScheme::cua, keyScheme::vim, keyScheme::nano},
                                dir));
         }},
        {"Playback", "Fade", "−  " + formatMilliseconds(settings_.fade, true) + "  +",
         [this](int dir) { setFade(settings_.fade + dir * 0.01); }},
        {"Playback", "Quantize", titleCase(toString(settings_.quant)) + " ▾",
         [this, cycle](int dir) {
             settings_.quant = cycle(settings_.quant,
                                     {quantization::none, quantization::beat, quantization::bar}, dir);
             persistSettings();
         }},
        {"Playback", "Rebuild on Save", switchText(settings_.rebuildOnSave),
         toggle(settings_.rebuildOnSave)},
        {"Layout", "Show File List", switchText(settings_.fileList), toggle(settings_.fileList)},
        {"Layout", "Show Console", switchText(settings_.console), toggle(settings_.console)},
    };
}

// Preferences apply immediately; there is no Apply or OK button.
Element app::renderPreferences() {
    [[maybe_unused]] const palette S{*theme_};
    const auto rows = preferenceRows();
    overlayFocus_ = std::min(overlayFocus_, rows.size() - 1);
    Elements content;
    std::string group;
    Elements groupRows;
    auto flush = [&] {
        if (groupRows.empty()) return;
        content.push_back(text(" " + group) | bold);
        content.push_back(vbox(std::move(groupRows)) | S.border());
        groupRows.clear();
    };
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const preferenceRow& r = rows[i];
        if (r.group != group) {
            flush();
            group = r.group;
        }
        if (!groupRows.empty()) groupRows.push_back(S.surfaceRule(separatorLight()));
        auto change = r.change;
        Element value = text(r.value);
        const bool focused = i == overlayFocus_;
        if (!focused && r.value.find("On") != std::string::npos) value = value | color(S.t.accentText);
        if (!focused && r.value.find("Off") != std::string::npos) value = S.surfaceSecondary(value);
        Element row = hbox({text(" " + r.title), filler(), value, text(" ")});
        if (focused) row = S.suggested(row);
        groupRows.push_back(clickable(
            row,
            [this, i, change](const Mouse& m) {
                overlayFocus_ = i;
                // On a stepper, the left half decreases and the right half increases.
                const bool stepper = preferenceRows()[i].value.starts_with("−");
                change(stepper && m.x < (overlayBox_.x_min + overlayBox_.x_max) / 2 + 12 ? -1 : 1);
            },
            [change](int dir) { change(-dir); }));
    }
    flush();
    return vbox(std::move(content));
}

Element app::renderShortcuts() {
    [[maybe_unused]] const palette S{*theme_};
    struct line {
        std::string left;
        std::string right;
        bool heading;
    };
    std::vector<line> lines;
    for (const auto& g : shortcutGroups()) {
        lines.push_back({std::string(g.title), {}, true});
        for (auto command : g.commands) {
            const std::string keys = keyHint(command);
            if (!keys.empty()) lines.push_back({std::string(commandLabel(command)), keys, false});
        }
    }
    const int height = std::max((rootBox_.y_max - rootBox_.y_min + 1) - 16, 6);
    overlayScroll_ = std::min(overlayScroll_, lines.size() > static_cast<std::size_t>(height)
                                                  ? lines.size() - static_cast<std::size_t>(height)
                                                  : std::size_t{0});
    Elements rows;
    for (std::size_t i = overlayScroll_;
         i < lines.size() && rows.size() < static_cast<std::size_t>(height); ++i) {
        const line& l = lines[i];
        if (l.heading) {
            rows.push_back(text(" " + l.left) | bold);
        } else {
            rows.push_back(hbox({text("   " + l.left), filler(), S.surfaceButton(text(" " + l.right + " ")),
                                 text(" ")}));
        }
    }
    auto wheel = [this](int dir) {
        if (dir > 0) ++overlayScroll_;
        if (dir < 0 && overlayScroll_ > 0) --overlayScroll_;
    };
    const std::string scheme = titleCase(toString(keys_.scheme())) + " key bindings";
    return clickable(vbox({S.surfaceSecondary(text(" " + scheme)), text(""), vbox(std::move(rows))}), {},
                     wheel);
}

Element app::renderAbout() {
    [[maybe_unused]] const palette S{*theme_};
    Element close = clickable(S.suggested(text("  Close  ")),
                              [this](const Mouse&) { overlay_ = overlayKind::none; });
    return vbox({
        text(""),
        text("proj") | bold | hcenter,
        S.surfaceSecondary(text(options_.link.clientVersion)) | hcenter,
        text(""),
        paragraphAlignCenter("A workstation for acousmatic, aleatory, and automatic music"),
        text(""),
        S.surfaceSecondary(paragraphAlignCenter("GNU Affero General Public License, version 3")),
        text(""),
        hbox({filler(), close, filler()}),
    });
}

Element app::renderOverlay() {
    [[maybe_unused]] const palette S{*theme_};
    clipTo_ = &overlayBox_;
    std::string title;
    Element content;
    int width = 60;
    switch (overlay_) {
    case overlayKind::preferences:
        title = "Preferences";
        content = renderPreferences();
        break;
    case overlayKind::shortcuts:
        title = "Keyboard Shortcuts";
        content = renderShortcuts();
        width = 56;
        break;
    case overlayKind::about:
        title = "About proj";
        content = renderAbout();
        width = 50;
        break;
    case overlayKind::none:
        break;
    }
    Element close = clickable(text(" × "), [this](const Mouse&) { overlay_ = overlayKind::none; },
                              {}, {}, "Close · Esc");
    clipTo_ = nullptr;
    Element box = vbox({dbox({hbox({filler(), text(title) | bold, filler()}),
                              hbox({filler(), close})}),
                        S.surfaceRule(separatorLight()), content}) |
                  size(WIDTH, EQUAL, width);
    box = S.surface(box | S.border()) | clear_under | reflect(overlayBox_);
    return centered(box, S.t.window);
}

Element app::render() {
    [[maybe_unused]] const palette S{*theme_};
    collectReplies();
    targets_.clear();

    Elements mainColumn{renderTabBar(), S.rule(separator()), renderEditorPane()};
    if (settings_.console) mainColumn.push_back(renderConsole());

    Elements body;
    if (settings_.fileList && !narrow()) {
        body.push_back(renderSidebar());
        auto drag = [this](const Mouse& m) {
            settings_.fileListWidth = std::clamp(m.x - fileListBox_.x_min, 8, 200);
            persistOnRelease_ = true;
        };
        body.push_back(clickable(S.rule(separator()), [](const Mouse&) {}, {}, drag,
                                 "Drag to resize the file list"));
    }
    body.push_back(vbox(std::move(mainColumn)) | flex);

    Elements layers{vbox({
                         renderHeaderBar(),
                         S.rule(separator()),
                         hbox(std::move(body)) | flex,
                         S.rule(separator()),
                         renderStatusBar(),
                         renderMessageLine(),
                     }) |
                     color(S.t.text) | bgcolor(S.t.window) | reflect(rootBox_)};
    // A modal dims the window behind it.
    if (dialog_ || overlay_ != overlayKind::none) layers[0] = layers[0] | dim;
    if (overlay_ != overlayKind::none) layers.push_back(renderOverlay());
    if (dialog_) layers.push_back(renderDialog());
    if (menu_) layers.push_back(renderMenu());
    return dbox(std::move(layers));
}

} // namespace tui
