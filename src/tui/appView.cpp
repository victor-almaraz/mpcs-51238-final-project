// Rendering of the editor: header bar, panes, popovers, dialogs, and
// overlays. State and input live in app.cpp.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include <espresso/tui/app.h>
#include <espresso/tui/buffer.h>
#include <espresso/tui/editorView.h>
#include <espresso/tui/keymap.h>
#include <espresso/tui/theme.h>
#include <espresso/workspace/consoleLog.h>
#include <espresso/workspace/interpreterLink.h>
#include <espresso/workspace/settings.h>
#include <espresso/workspace/utf8.h>

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
// preferences, shortcuts, and about dialogs. Colors come from the active
// theme; the default theme uses the terminal's named palette, so the
// terminal's own theme (light, dark, high contrast) applies.

namespace {

// Below this width the sidebar collapses and the header bar drops its
// labels (adaptive layout).
constexpr int narrowColumns = 72;

// Styles by role, for the active theme. Views ask for a role and never
// for a literal color, so every theme applies everywhere.
struct palette {
    const theme& t;

    Element suggested(Element e) const {
        return std::move(e) | bgcolor(t.accent.color) | color(t.onAccent.color) | bold;
    }
    Element destructive(Element e) const {
        return std::move(e) | bgcolor(t.destructive.color) | color(t.onDestructive.color) | bold;
    }
    Element field(Element e) const {
        return std::move(e) | bgcolor(t.field.color) | color(t.fieldText.color);
    }
    Element selected(Element e) const {
        return std::move(e) | bgcolor(t.selection.color) | color(t.selectionText.color) | bold;
    }
    Element secondary(const Element& e) const {
        return t.faintSecondary ? e | dim : e | color(t.secondary.color);
    }
    Element rule(Element e) const { return std::move(e) | color(t.rule.color); }

    // Raised surfaces: every popover, dialog, overlay, and the open prompt
    // line has its own background, border included, so it stands apart
    // from the window behind it. Controls on a surface keep their
    // contrast against it.
    Element surface(Element e) const {
        return std::move(e) | color(t.surfaceText.color) | bgcolor(t.surface.color);
    }
    Element surfaceButton(Element e) const {
        return std::move(e) | bgcolor(t.surfaceField.color) | color(t.surfaceFieldText.color);
    }
    Element surfaceSecondary(Element e) const { return std::move(e) | color(t.surfaceSecondary.color); }
    Element surfaceRule(Element e) const { return std::move(e) | color(t.surfaceRule.color); }
    Decorator border() const { return borderStyled(ROUNDED, t.surfaceRule.color); }
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
std::string displayKeys(std::string_view keys, workspace::keyScheme scheme) {
    if (scheme == workspace::keyScheme::emacs || scheme == workspace::keyScheme::vim)
        return std::string(keys);
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
        {"openFile", "Open File…"},
        {"switchBuffer", "Switch Buffer…"},
        {"save", "Save"},
        {"saveAs", "Save As…"},
        {"closeBuffer", "Close Tab"},
        {"nextBuffer", "Next Tab"},
        {"previousBuffer", "Previous Tab"},
        {"quit", "Quit"},
        {"undo", "Undo"},
        {"redo", "Redo"},
        {"cut", "Cut"},
        {"copy", "Copy"},
        {"paste", "Paste"},
        {"selectAll", "Select All"},
        {"setMark", "Set Mark"},
        {"killLine", "Delete to End of Line"},
        {"searchForward", "Find…"},
        {"searchBackward", "Find Backward…"},
        {"searchNext", "Find Next"},
        {"searchPrevious", "Find Previous"},
        {"play", "Play"},
        {"stop", "Stop"},
        {"evalLastForm", "Evaluate Form"},
        {"toggleFileList", "Show File List"},
        {"toggleConsole", "Show Console"},
        {"mainMenu", "Main Menu"},
        {"contextMenu", "Context Menu"},
        {"showPreferences", "Preferences"},
        {"showShortcuts", "Keyboard Shortcuts"},
        {"showAbout", "About espresso"},
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
        {.title = "Files",
         .commands = {"openFile", "switchBuffer", "save", "saveAs", "closeBuffer", "nextBuffer",
                      "previousBuffer", "quit"}},
        {.title = "Editing",
         .commands = {"undo", "redo", "cut", "copy", "paste", "selectAll", "setMark", "killLine"}},
        {.title = "Find", .commands = {"searchForward", "searchBackward", "searchNext", "searchPrevious"}},
        {.title = "Run", .commands = {"play", "stop", "evalLastForm"}},
        {.title = "View",
         .commands = {"toggleFileList", "toggleConsole", "mainMenu", "contextMenu", "showPreferences",
                      "showShortcuts"}},
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

namespace {

// Paints over its child once the child is drawn. A control's state
// depends on which control is under the pointer, which is known only
// after layout, so it is drawn here rather than chosen while building
// the element.
template <typename Paint>
class paintAfter : public Node {
public:
    paintAfter(Element child, Paint paint) : Node(Elements{std::move(child)}), paint_(std::move(paint)) {}

    void SetBox(Box box) override {
        Node::SetBox(box);
        children_.front()->SetBox(box);
    }

    void Render(Screen& screen) override {
        Node::Render(screen);
        paint_(Box::Intersection(box_, screen.stencil), screen);
    }

private:
    Paint paint_;
};

template <typename Paint>
Element paintedAfter(Element child, Paint paint) {
    return std::make_shared<paintAfter<Paint>>(std::move(child), std::move(paint));
}

template <typename Paint>
void eachCell(const Box& box, Screen& screen, Paint paint) {
    for (int y = box.y_min; y <= box.y_max; ++y) {
        for (int x = box.x_min; x <= box.x_max; ++x) paint(screen.CellAt(x, y));
    }
}

// The rows of @p text at @p width cells, or one row per line when the
// width is not yet known. A row ends at a line break, or at the last
// space that fits, which it drops; a word longer than the row breaks
// where the row ends.
std::vector<std::string> wrapText(std::string_view text, int width) {
    std::vector<std::string> rows;
    for (std::size_t start = 0;;) {
        const std::size_t newline = text.find('\n', start);
        const std::string_view line =
            text.substr(start, newline == std::string_view::npos ? newline : newline - start);
        // One glyph per cell; a wide glyph is followed by an empty one.
        const std::vector<std::string> glyphs = Utf8ToGlyphs(line);
        const std::size_t w = width > 0 ? static_cast<std::size_t>(width) : glyphs.size() + 1;
        auto join = [&glyphs](std::size_t from, std::size_t to) {
            std::string row;
            for (std::size_t i = from; i < to; ++i) row += glyphs[i];
            return row;
        };
        std::size_t i = 0;
        while (glyphs.size() - i > w) {
            std::size_t end = i + w;  // the glyph after the row
            while (end > i && glyphs[end] != " ") --end;
            if (end > i) {
                rows.push_back(join(i, end));
                i = end + 1;
                continue;
            }
            end = i + w;
            if (glyphs[end].empty() && end > i + 1) --end;  // keep a wide glyph whole
            rows.push_back(join(i, end));
            i = end;
        }
        rows.push_back(join(i, glyphs.size()));
        if (newline == std::string_view::npos) return rows;
        start = newline + 1;
    }
}

// Text that wraps at the width the layout gives it (wrapText). Its
// height depends on its width, so a change of width asks the layout for
// another pass. @p decorate styles each row's text, not the cells after
// it.
class wrapped : public Node {
public:
    wrapped(std::string text, Decorator decorate) : text_(std::move(text)), decorate_(std::move(decorate)) {}

    void ComputeRequirement() override {
        Elements rows;
        for (auto& row : wrapText(text_, width_))
            rows.push_back(hbox({decorate_(ftxui::text(std::move(row))), filler()}));
        children_ = {vbox(std::move(rows))};
        children_.front()->ComputeRequirement();
        requirement_ = children_.front()->requirement();
        requirement_.min_x = 1;  // any width: the rows follow it
    }

    void SetBox(Box box) override {
        Node::SetBox(box);
        const int width = box.x_max - box.x_min + 1;
        stale_ = width != width_;
        width_ = width;
        children_.front()->SetBox(box);
    }

    void Check(Status* status) override {
        Node::Check(status);
        status->need_iteration |= std::exchange(stale_, false);
    }

private:
    std::string text_;
    Decorator decorate_;
    int width_ = 0;  // from the last layout; 0 before the first
    bool stale_ = false;
};

// Limits drawing on @p screen to @p box, within the current limit, for
// the scope.
class clip {
public:
    clip(Screen& screen, const Box& box) : screen_(screen), saved_(screen.stencil) {
        screen.stencil = Box::Intersection(box, saved_);
    }
    clip(const clip&) = delete;
    clip& operator=(const clip&) = delete;
    ~clip() { screen_.stencil = saved_; }

private:
    Screen& screen_;
    Box saved_;
};

// Shows the bottom of its child, @p scroll rows up from its last row, or
// the whole child from the top when it fits. Writes the largest useful
// scroll to @p maxScroll at each layout.
class bottomFrame : public Node {
public:
    bottomFrame(Element child, std::size_t scroll, std::size_t& maxScroll)
        : Node(Elements{std::move(child)}), scroll_(scroll), maxScroll_(maxScroll) {}

    void SetBox(Box box) override {
        Node::SetBox(box);
        const int content = children_.front()->requirement().min_y;
        const int height = box.y_max - box.y_min + 1;
        maxScroll_.get() = static_cast<std::size_t>(std::max(content - height, 0));
        const int scroll = static_cast<int>(std::min(scroll_, maxScroll_.get()));
        Box inner = box;
        if (content > height) {
            inner.y_max = box.y_max + scroll;
            inner.y_min = inner.y_max - content + 1;
        }
        children_.front()->SetBox(inner);
    }

    void Render(Screen& screen) override {
        const clip scope(screen, box_);
        Node::Render(screen);
    }

private:
    std::size_t scroll_;
    std::reference_wrapper<std::size_t> maxScroll_;
};

}  // namespace

// Registers a control (`tui.md` §4.3). A control that cannot be used is
// drawn in the secondary color; the one under the pointer is drawn in its
// state by paintLit, once the frame is laid out.
Element app::clickable(Element e, std::function<void(const Mouse&)> press, control options) {
    const bool sensitive = options.sensitive;
    targets_.push_back(
        {.press = std::move(press), .within = clipTo_, .surface = surfaces_, .options = std::move(options)});
    e = std::move(e) | reflect(targets_.back().box);
    if (sensitive) return e;
    const theme& t = theme_.get();
    // Popovers, dialogs, overlays, and the open prompt are surfaces.
    const bool onSurface = isPopover(clipTo_) || clipTo_ == region::message;
    if (!onSurface && t.faintSecondary) return std::move(e) | dim;
    const Color dimmed = (onSurface ? t.surfaceSecondary : t.secondary).color;
    return paintedAfter(std::move(e), [dimmed](const Box& box, Screen& screen) {
        eachCell(box, screen, [&](Cell& cell) { cell.foreground_color = dimmed; });
    });
}

void app::paintLit(Screen& screen) const {
    const auto lit = litControl();
    if (!lit) return;
    const auto& [index, state] = *lit;
    const target& c = targets_[index];
    const theme& t = theme_.get();
    const Box box = Box::Intersection(Box::Intersection(c.box, boxOf(c.within)), screen.stencil);
    if (c.options.kind == look::handle) {  // a divider lights up; it has no fill
        eachCell(box, screen, [&](Cell& cell) { cell.foreground_color = t.accentText.color; });
        return;
    }
    const controlStates& states =
        statesFor(t, c.options.kind, isPopover(c.within) || c.within == region::message);
    const stateColors& colors = state == controlState::hover ? states.hover : states.pressed;
    eachCell(box, screen, [&](Cell& cell) {
        cell.background_color = colors.background.color;
        cell.foreground_color = colors.text.color;
        cell.dim = false;
    });
}

const controlStates& app::statesFor(const theme& t, look kind, bool onSurface) {
    switch (kind) {
    case look::field: return t.fieldStates;
    case look::button: return t.buttonStates;
    case look::accent: return t.accentStates;
    case look::destructive: return t.destructiveStates;
    case look::selection: return t.selectionStates;
    case look::none:
    case look::flat:
    case look::handle: break;
    }
    return onSurface ? t.surfaceFlatStates : t.flatStates;
}

std::string app::keyHint(std::string_view command) const {
    const workspace::keyScheme scheme = keys_.scheme();
    const editMode mode = scheme == workspace::keyScheme::vim ? editMode::normal : editMode::insert;
    for (const auto& b : schemeBindings(scheme, mode)) {
        if (b.command == command) return displayKeys(b.keys, scheme);
    }
    if (scheme == workspace::keyScheme::vim) {
        for (const auto& e : exCommands()) {
            if (e.command == command) return ":" + std::string(e.name);
        }
    }
    for (const auto& b : commonBindings()) {
        if (b.command == command) return displayKeys(b.keys, scheme);
    }
    return {};
}

app::menuEntry app::item(std::string_view command) {
    return {.label = std::string(commandLabel(command)),
            .action = [this, name = std::string(command)] { runCommand(name); },
            .hint = keyHint(command),
            .checked = {}};
}

// The main menu holds app-wide actions; Preferences, Keyboard Shortcuts,
// and About come last. Quit is the header bar's close button.
std::vector<app::menuEntry> app::mainMenu() {
    menuEntry fileList = item("toggleFileList");
    fileList.checked = settings_.fileList;
    menuEntry consolePane = item("toggleConsole");
    consolePane.checked = settings_.console;
    return {item("openFile"),
            item("switchBuffer"),
            item("save"),
            item("saveAs"),
            item("closeBuffer"),
            {},
            item("searchForward"),
            item("evalLastForm"),
            {},
            fileList,
            consolePane,
            {},
            item("showPreferences"),
            item("showShortcuts"),
            item("showAbout")};
}

// Items with nothing to act on are drawn dimmed and do nothing.
std::vector<app::menuEntry> app::contextMenu() {
    auto when = [](menuEntry e, bool sensitive) {
        e.sensitive = sensitive;
        return e;
    };
    const buffer& b = current();
    const bool searched = !lastSearch_.empty();
    return {when(item("undo"), b.undoDepth() > 0),
            when(item("redo"), b.canRedo()),
            {},
            item("cut"),
            item("copy"),
            when(item("paste"), !clipboard_.empty()),
            item("selectAll"),
            {},
            item("searchForward"),
            item("searchBackward"),
            when(item("searchNext"), searched),
            when(item("searchPrevious"), searched),
            {},
            item("evalLastForm")};
}

void app::openMenu(std::vector<menuEntry> entries, int x, int y, bool alignRight) {
    ++surfaces_;
    menu_ = std::move(entries);
    menuX_ = x;
    menuY_ = y;
    menuAlignRight_ = alignRight;
    // Focus starts on the first item that can be used.
    const auto first = std::ranges::find_if(*menu_, [](const menuEntry& e) { return e.usable(); });
    menuFocus_ = first == menu_->end() ? 0 : static_cast<std::size_t>(first - menu_->begin());
}

Element app::renderMenu(const std::vector<menuEntry>& entries) {
    const palette S{theme_.get()};
    clipTo_ = region::menu;
    std::size_t width = 0;
    for (const auto& e : entries) {
        width = std::max(width, workspace::utf8::length(e.label) + workspace::utf8::length(e.hint) + 6);
    }
    Elements rows;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const menuEntry& entry = entries[i];
        if (entry.label.empty()) {
            rows.push_back(S.surfaceRule(separatorLight()));
            continue;
        }
        const std::string check = entry.checked ? (*entry.checked ? "✓ " : "  ") : "  ";
        const bool focused = i == menuFocus_ && entry.sensitive;
        Element hint = text(entry.hint.empty() ? "" : "  " + entry.hint);
        Element row = hbox({text(check), text(entry.label), filler(),
                            focused ? hint : S.surfaceSecondary(hint), text(" ")});
        if (focused) row = S.suggested(row);  // accent highlight across the whole row
        // The focused row is the one under the pointer, as in a popover.
        rows.push_back(clickable(row,
                                 [this, i](const Mouse&) {
                                     menuFocus_ = i;
                                     handleMenuKey("RET");
                                 },
                                 {.sensitive = entry.sensitive, .enter = [this, i] { menuFocus_ = i; }}));
    }
    clipTo_ = region::none;
    const int w = static_cast<int>(width) + 2;  // plus the border
    Element popup = S.surface(vbox(std::move(rows)) | size(WIDTH, EQUAL, w - 2) | S.border()) |
                    clear_under | reflect(menuBox_);
    // The margin starts one cell left of the popover.
    const int x = std::max((menuAlignRight_ ? menuX_ - w + 1 : menuX_) - 1, 0);
    return vbox({
        text("") | size(HEIGHT, EQUAL, std::max(menuY_, 0)),
        hbox({text("") | size(WIDTH, EQUAL, x), popoverMargin(popup, S.t.window.color), filler()}),
        filler(),
    });
}

namespace {

// Width in cells of a row of text pieces.
int cells(std::initializer_list<std::string_view> pieces) {
    std::size_t n = 0;
    for (auto p : pieces) n += workspace::utf8::length(p);
    return static_cast<int>(n);
}

}  // namespace

Element app::renderHeaderBar() {
    const palette S{theme_.get()};
    const bool compact = narrow();
    const std::string play = " ▶ Play ";
    const std::string stopLabel = " ■ Stop ";
    Elements start{
        clickable(S.suggested(text(play)), [this](const Mouse&) { runCommand("play"); },
                  {.kind = look::accent, .tooltip = "Compile and play the project · " + keyHint("play")}),
        text(" "),
        // Stop has nothing to act on until a plan plays.
        clickable(text(stopLabel), [this](const Mouse&) { runCommand("stop"); },
                  {.kind = look::flat,
                   .tooltip = "Stop after the fade · " + keyHint("stop"),
                   .sensitive = status_.connected && status_.activePlan != 0}),
    };
    const int startWidth = cells({play, " ", stopLabel});

    const std::string fadeLabel = compact ? "" : "Fade ";
    const std::string fadeValue = " " + workspace::formatMilliseconds(settings_.fade, true) + " ";
    const std::string quantLabel = compact ? "" : "Quantize ";
    const std::string quantValue = " " + titleCase(toString(settings_.quant)) + " ▾ ";
    // ≡ rather than ☰: U+2630 is double width in Unicode 16 and would
    // misalign the row in terminals that follow it.
    const std::string menuButton = " ≡ ";
    const std::string closeButton = " × ";
    Elements end{
        S.secondary(text(fadeLabel)),
        clickable(S.field(text(fadeValue)),
                  [this](const Mouse&) {
                      openPrompt(promptKind::fade,
                                 "Fade (ms): ", workspace::formatMilliseconds(settings_.fade));
                  },
                  {.kind = look::field,
                   .wheel =
                       [this](int dir) {
                           settings_.fade = std::max(settings_.fade - dir * 0.01, 0.0);
                           persistPending_ = true;  // written once scrolling ends, not per tick
                       },
                   .tooltip = "Swap crossfade · click to type, scroll to adjust"}),
        text("  "),
        S.secondary(text(quantLabel)),
        clickable(
            S.field(text(quantValue)),
            [this](const Mouse& m) {
                std::vector<menuEntry> entries;
                for (auto q : {workspace::quantization::none, workspace::quantization::beat,
                               workspace::quantization::bar}) {
                    entries.push_back({.label = titleCase(toString(q)),
                                       .action =
                                           [this, q] {
                                               settings_.quant = q;
                                               persistSettings();
                                           },
                                       .hint = {},
                                       .checked = settings_.quant == q});
                }
                openMenu(std::move(entries), m.x, m.y + 2);  // below the header bar's rule
            },
            {.kind = look::field, .tooltip = "Grid that swaps wait for"}),
        text("   "),
        clickable(text(menuButton), [this](const Mouse&) { runCommand("mainMenu"); },
                  {.kind = look::flat, .tooltip = "Main menu · " + keyHint("mainMenu")}),
        clickable(text(closeButton), [this](const Mouse&) { runCommand("quit"); },
                  {.kind = look::flat, .tooltip = "Quit · " + keyHint("quit")}),
    };
    const int endWidth = cells({fadeLabel, fadeValue, "  ", quantLabel, quantValue, "   ",
                                menuButton, closeButton});

    // The title names the document, centered, with the project as the
    // subtitle. It shows only when it fits between the two groups.
    const buffer& b = current();
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
    const palette S{theme_.get()};
    clipTo_ = region::fileList;
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
        const bool open = resolve(f) == currentPath;
        if (open) row = S.selected(row);
        rows.push_back(clickable(row, [this, f](const Mouse&) { openFile(f); },
                                 {.kind = open ? look::selection : look::flat, .wheel = wheel}));
    }
    clipTo_ = region::none;
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
    const palette S{theme_.get()};
    clipTo_ = region::tabBar;
    Elements tabs;
    for (std::size_t i = 0; i < buffers_.size(); ++i) {
        const buffer& b = buffers_[i]->text;
        const look kind = i == current_ ? look::selection : look::flat;
        Element name = clickable(text(" " + displayName(b) + (b.dirty() ? " •" : "") + " "),
                                 [this, i](const Mouse&) { selectBuffer(i); }, {.kind = kind});
        Element close = clickable(text("× "), [this, i](const Mouse&) { closeBuffer(i); },
                                  {.kind = kind, .tooltip = "Close tab · " + keyHint("closeBuffer")});
        Element tab = hbox({name, close});
        tabs.push_back(i == current_ ? S.selected(tab) : S.secondary(tab));
        tabs.push_back(text(" "));
    }
    tabs.push_back(
        clickable(text(" + "), [this](const Mouse&) { runCommand("openFile"); },
                  {.kind = look::flat, .tooltip = "Open a file in a new tab · " + keyHint("openFile")}));
    clipTo_ = region::none;
    tabs.push_back(filler());
    return hbox(std::move(tabs)) | reflect(tabBarBox_);
}

Element app::renderEditorPane() {
    buffer& b = current();
    const int rows = editorRows();
    const int columns = editorColumns();
    if (followCursor_) {
        ensureCursorVisible(b, rows, columns - gutterWidth(b));
        followCursor_ = false;
    }
    const bool focused = !minibuffer_.active() && !dialog_ && overlay_ == overlayKind::none;
    Element view = renderEditor(b, highlights(), rows, columns,
                                {.showCursor = focused, .showGutter = true, .colors = theme_});
    auto at = [this](const Mouse& m) {
        return hitTest(current(), m.y - editorBox_.y_min, m.x - editorBox_.x_min);
    };
    auto press = [this, at](const Mouse& m) {
        buffer& cur = current();
        cur.setCursor(at(m));
        cur.setMark();
        shiftSelecting_ = true;
    };
    auto drag = [this, at](const Mouse& m) {
        current().setCursor(at(m));
        if (m.y < editorBox_.y_min) current().moveUp();
        if (m.y > editorBox_.y_max) current().moveDown();
        followCursor_ = true;
    };
    auto wheel = [this](int dir) {
        buffer& cur = current();
        const std::size_t step = 3;
        if (dir < 0) cur.setTopLine(cur.topLine() - std::min(step, cur.topLine()));
        if (dir > 0) cur.setTopLine(std::min(cur.topLine() + step, cur.lineCount() - 1));
    };
    // yframe clips rows beyond the pane after a resize.
    return clickable(std::move(view) | yframe, press, {.wheel = wheel, .drag = drag}) | reflect(editorBox_) |
           flex;
}

Element app::renderConsole() {
    const palette S{theme_.get()};
    // Pane heading; dragging it resizes the console.
    auto drag = [this](const Mouse& m) {
        settings_.consoleHeight = std::clamp(consoleBox_.y_max - m.y, 1, 200);
        persistPending_ = true;
    };
    const std::string clearLabel = " Clear ";
    Element handle =
        hbox({S.rule(text("─ ")), text("Console") | bold, text(" "), S.rule(separatorCharacter("─")) | flex,
              text(std::string(clearLabel.size(), ' ')), S.rule(text("─"))});
    handle = clickable(handle, [](const Mouse&) {},
                       {.kind = look::handle, .drag = drag, .tooltip = "Drag to resize the console"});
    // Clear sits over the heading, so a press on it reaches Clear and not
    // the heading's drag.
    Element clear = clickable(text(clearLabel) | color(S.t.text.color),
                              [this](const Mouse&) { runCommand("clearConsole"); },
                              {.kind = look::flat, .tooltip = "Remove all console messages"});
    Element heading = dbox({handle, hbox({filler(), clear, text("─")})});

    clipTo_ = region::console;
    const auto& entries = console_.entries();
    const int height = std::max(settings_.consoleHeight, 1);
    // Long entries wrap, so the console scrolls by rows. Every entry takes
    // a row at least: the newest scroll + height + 1 entries fill the pane
    // and leave one more row to scroll to, if there is one.
    const std::size_t shown = consoleScroll_ + static_cast<std::size_t>(height) + 1;
    const std::size_t begin = entries.size() > shown ? entries.size() - shown : 0;
    auto wheel = [this](int dir) {
        consoleScroll_ = std::min(consoleScroll_, consoleMaxScroll_);
        if (dir < 0 && consoleScroll_ < consoleMaxScroll_) ++consoleScroll_;
        if (dir > 0 && consoleScroll_ > 0) --consoleScroll_;
    };
    Elements rows;
    for (std::size_t i = begin; i < entries.size(); ++i) {
        const workspace::consoleEntry& e = entries[i];
        // Errors carry a mark as well as a color. Wrapped rows line up
        // after the mark, so an entry reads as one.
        Decorator decorate = nothing;
        if (e.location) decorate = underlined;
        Element row = hbox({text(e.kind == workspace::consoleKind::error ? " ✕ " : "   "),
                            std::make_shared<wrapped>(e.text, std::move(decorate)) | flex});
        switch (e.kind) {
        case workspace::consoleKind::error: row = row | color(S.t.error.color); break;
        case workspace::consoleKind::server: row = row | color(S.t.info.color); break;
        case workspace::consoleKind::info: break;
        }
        if (e.location) {
            const workspace::sourceLocation loc = *e.location;
            row = clickable(row, [this, loc](const Mouse&) { jumpTo(loc); },
                            {.kind = look::flat,
                             .wheel = wheel,
                             .tooltip = "Go to " + loc.path.string() + ":" + std::to_string(loc.line)});
        } else {
            row = clickable(row, {}, {.wheel = wheel});
        }
        rows.push_back(row);
    }
    clipTo_ = region::none;
    Element body = rows.empty() ? vbox({filler(), S.secondary(text("No Messages")) | hcenter, filler()})
                                : std::make_shared<bottomFrame>(vbox(std::move(rows)), consoleScroll_,
                                                                consoleMaxScroll_);
    return vbox({heading, body | size(HEIGHT, EQUAL, height) | reflect(consoleBox_)});
}

Element app::renderStatusBar() {
    const palette S{theme_.get()};
    const workspace::serverStatus& s = status_;
    // The dot's shape and the text both carry the state, not color alone.
    const std::string server = s.connected ? s.server : "Not connected";
    Element state =
        hbox({s.connected ? text(" ● ") | color(S.t.success.color) : S.secondary(text(" ○ ")), text(server)});

    std::ostringstream detail;
    detail << "   " << s.tempoText() << " · Beat " << s.beatText();
    if (s.connected) detail << " · Plan " << (s.activePlan != 0 ? std::to_string(s.activePlan) : "—");
    if (s.connected) detail << " · " << s.loadText();
    detail << " · Missed " << s.missed << " · Xruns " << s.xruns << " · Drops " << s.drops;

    const buffer& b = current();
    std::ostringstream right;
    right << titleCase(toString(keys_.scheme()));
    if (keys_.scheme() == workspace::keyScheme::vim) right << " " << titleCase(toString(keys_.mode()));
    right << " · Ln " << b.cursor().line + 1 << ", Col " << b.cursor().column + 1 << ' ';

    // Drop the counters rather than truncate the connection state.
    const bool fits = cells({"   ", server, detail.str(), right.str()}) + 2 <= windowWidth();
    return hbox({state, fits ? S.secondary(text(detail.str())) : text(""), filler(),
                 S.secondary(text(right.str()))});
}

Element app::renderMessageLine() {
    const palette S{theme_.get()};
    clipTo_ = region::message;
    Element line = [this, &S]() -> Element {
        if (minibuffer_.active()) {
            const std::string& t = minibuffer_.text();
            const std::size_t at = workspace::utf8::byteOffset(t, minibuffer_.cursor());
            const std::size_t len =
                at < t.size() ? workspace::utf8::sequenceLength(static_cast<unsigned char>(t[at])) : 0;
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
                    parts.push_back(clickable(S.surfaceSecondary(text(m)),
                                              [this, m](const Mouse&) {
                                                  minibuffer_.setText(m);
                                                  submitPrompt();
                                              },
                                              {.kind = look::flat}));
                    parts.push_back(text("  "));
                }
            }
            parts.push_back(filler());
            return S.surface(hbox(std::move(parts)));  // an open prompt is a surface too
        }
        const std::string pending = keys_.pendingKeys();
        // A long message wraps instead of losing its end at the edge.
        if (!message_.empty()) return hbox({text(" "), std::make_shared<wrapped>(message_, nothing) | flex});
        if (!pending.empty()) return text(" " + pending + "-");
        return S.secondary(text(" " + hoverTip_));
    }();
    clipTo_ = region::none;
    return line | reflect(messageBox_);
}

// An alert dialog: heading, body, and responses with Cancel first, the
// destructive response in the destructive color, and the default response
// highlighted.
Element app::renderDialog(const dialog& d) {
    const palette S{theme_.get()};
    clipTo_ = region::dialog;
    auto respond = [this](char c) { return [this, c](const Mouse&) { answerDialog(c); }; };
    Element cancel = clickable(S.surfaceButton(text("  Cancel  ")), respond('c'),
                               {.kind = look::button, .tooltip = "Keep editing · Esc"});
    Element discard = clickable(S.destructive(text("  Discard  ")), respond('d'),
                                {.kind = look::destructive, .tooltip = "Close without saving · D"});
    Element saveButton = clickable(S.suggested(text("  Save  ")), respond('s'),
                                   {.kind = look::accent, .tooltip = "Save, then close · Enter"});
    clipTo_ = region::none;
    Element box = vbox({
                      text(""),
                      text(d.heading) | bold | hcenter,
                      text(""),
                      hbox({text("  "), paragraphAlignCenter(d.body) | flex, text("  ")}),
                      text(""),
                      hbox({filler(), cancel, text("  "), discard, text("  "), saveButton, filler()}),
                      text(""),
                  }) |
                  size(WIDTH, EQUAL, 54);
    box = S.surface(box | S.border()) | clear_under | reflect(dialogBox_);
    return centered(box, S.t.window.color);
}

std::vector<app::preferenceRow> app::preferenceRows() {
    auto cycle = [](auto value, std::initializer_list<decltype(value)> all, int dir) {
        const std::span<const decltype(value)> values(all.begin(), all.end());
        const auto i = static_cast<std::size_t>(std::ranges::find(values, value) - values.begin());
        return values[wrapIndex(i, dir, values.size())];
    };
    auto toggle = [this](bool& flag) {
        return [this, &flag](int) {
            flag = !flag;
            persistSettings();
        };
    };
    auto toggleRow = [toggle](std::string group, std::string title, bool& flag) {
        return preferenceRow{.group = std::move(group),
                             .title = std::move(title),
                             .value = flag ? "━━● On " : "●━━ Off",
                             .change = toggle(flag),
                             .kind = preferenceRow::control::toggle,
                             .on = flag};
    };
    return {
        {.group = "Appearance",
         .title = "Theme",
         .value = std::string(theme_.get().title) + " ▾",
         .change =
             [this](int dir) {
                 const auto all = themes();
                 const auto at =
                     std::ranges::find_if(all, [&](const theme& t) { return t.name == theme_.get().name; });
                 setTheme(all[wrapIndex(static_cast<std::size_t>(at - all.begin()), dir, all.size())]);
             }},
        {.group = "Keyboard",
         .title = "Key Bindings",
         .value = titleCase(toString(settings_.keys)) + " ▾",
         .change =
             [this, cycle](int dir) {
                 setKeyScheme(cycle(settings_.keys,
                                    {workspace::keyScheme::emacs, workspace::keyScheme::cua,
                                     workspace::keyScheme::vim, workspace::keyScheme::nano},
                                    dir));
             }},
        {.group = "Playback",
         .title = "Fade",
         .value = "−  " + workspace::formatMilliseconds(settings_.fade, true) + "  +",
         .change = [this](int dir) { setFade(settings_.fade + dir * 0.01); },
         .kind = preferenceRow::control::stepper},
        {.group = "Playback",
         .title = "Quantize",
         .value = titleCase(toString(settings_.quant)) + " ▾",
         .change =
             [this, cycle](int dir) {
                 settings_.quant = cycle(settings_.quant,
                                         {workspace::quantization::none, workspace::quantization::beat,
                                          workspace::quantization::bar},
                                         dir);
                 persistSettings();
             }},
        toggleRow("Playback", "Rebuild on Save", settings_.rebuildOnSave),
        toggleRow("Layout", "Show File List", settings_.fileList),
        toggleRow("Layout", "Show Console", settings_.console),
    };
}

// Preferences apply immediately; there is no Apply or OK button.
Element app::renderPreferences() {
    const palette S{theme_.get()};
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
        if (!focused && r.kind == preferenceRow::control::toggle) {
            value = r.on ? value | color(S.t.accentText.color) : S.surfaceSecondary(value);
        }
        Element row = hbox({text(" " + r.title), filler(), value, text(" ")});
        if (focused) row = S.suggested(row);
        groupRows.push_back(clickable(
            row,
            [this, i, change, stepper = r.kind == preferenceRow::control::stepper](const Mouse& m) {
                overlayFocus_ = i;
                // On a stepper, the left half decreases and the right half increases.
                change(stepper && m.x < (overlayBox_.x_min + overlayBox_.x_max) / 2 + 12 ? -1 : 1);
            },
            {.kind = focused ? look::accent : look::flat, .wheel = [change](int dir) { change(-dir); }}));
    }
    flush();
    return vbox(std::move(content));
}

Element app::renderShortcuts() {
    const palette S{theme_.get()};
    struct line {
        std::string left;
        std::string right;
        bool heading = false;
    };
    std::vector<line> lines;
    for (const auto& g : shortcutGroups()) {
        lines.push_back({.left = std::string(g.title), .right = {}, .heading = true});
        for (auto command : g.commands) {
            const std::string keys = keyHint(command);
            if (!keys.empty())
                lines.push_back(
                    {.left = std::string(commandLabel(command)), .right = keys, .heading = false});
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
                     {.wheel = wheel});
}

Element app::renderAbout() {
    const palette S{theme_.get()};
    Element close = clickable(S.suggested(text("  Close  ")),
                              [this](const Mouse&) { overlay_ = overlayKind::none; }, {.kind = look::accent});
    return vbox({
        text(""),
        text("espresso") | bold | hcenter,
        S.surfaceSecondary(text(clientVersion_)) | hcenter,
        text(""),
        paragraphAlignCenter("A workstation for acousmatic, aleatory, and automatic music"),
        text(""),
        S.surfaceSecondary(paragraphAlignCenter("GNU Affero General Public License, version 3")),
        text(""),
        hbox({filler(), close, filler()}),
    });
}

Element app::renderOverlay() {
    const palette S{theme_.get()};
    clipTo_ = region::overlay;
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
        title = "About espresso";
        content = renderAbout();
        width = 50;
        break;
    case overlayKind::none:
        break;
    }
    Element close = clickable(text(" × "), [this](const Mouse&) { overlay_ = overlayKind::none; },
                              {.kind = look::flat, .tooltip = "Close · Esc"});
    clipTo_ = region::none;
    Element box = vbox({dbox({hbox({filler(), text(title) | bold, filler()}),
                              hbox({filler(), close})}),
                        S.surfaceRule(separatorLight()), content}) |
                  size(WIDTH, EQUAL, width);
    box = S.surface(box | S.border()) | clear_under | reflect(overlayBox_);
    return centered(box, S.t.window.color);
}

Element app::render() {
    const palette S{theme_.get()};
    collectReplies();
    targets_.clear();

    Elements mainColumn{renderTabBar(), S.rule(separator()), renderEditorPane()};
    if (settings_.console) mainColumn.push_back(renderConsole());

    Elements body;
    if (settings_.fileList && !narrow()) {
        body.push_back(renderSidebar());
        auto drag = [this](const Mouse& m) {
            settings_.fileListWidth = std::clamp(m.x - fileListBox_.x_min, 8, 200);
            persistPending_ = true;
        };
        body.push_back(
            clickable(S.rule(separator()), [](const Mouse&) {},
                      {.kind = look::handle, .drag = drag, .tooltip = "Drag to resize the file list"}));
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
                    color(S.t.text.color) | bgcolor(S.t.window.color) | reflect(rootBox_)};
    // A modal dims the window behind it.
    if (dialog_ || overlay_ != overlayKind::none) layers[0] = layers[0] | dim;
    if (overlay_ != overlayKind::none) layers.push_back(renderOverlay());
    if (dialog_) layers.push_back(renderDialog(*dialog_));
    if (menu_) layers.push_back(renderMenu(*menu_));
    // The control under the pointer is drawn in its state over the whole frame.
    return paintedAfter(dbox(std::move(layers)), [this](const Box&, Screen& screen) { paintLit(screen); });
}

} // namespace tui
