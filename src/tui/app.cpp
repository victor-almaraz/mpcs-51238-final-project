// The editor's state, commands, and input. Rendering lives in appView.cpp.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/terminal.hpp>

#include <espresso/tui/app.h>
#include <espresso/tui/buffer.h>
#include <espresso/tui/consoleLog.h>
#include <espresso/tui/editorView.h>
#include <espresso/tui/fileStore.h>
#include <espresso/tui/highlighter.h>
#include <espresso/tui/interpreterLink.h>
#include <espresso/tui/keyNames.h>
#include <espresso/tui/keymap.h>
#include <espresso/tui/settings.h>
#include <espresso/tui/theme.h>
#include <espresso/tui/utf8.h>

namespace fs = std::filesystem;
using namespace ftxui;

namespace tui {

namespace {

constexpr std::size_t indentWidth = 2;

// The cursor's line with its line break, as line-wise commands copy it.
std::string cursorLine(const buffer& b) { return b.line(b.cursor().line) + "\n"; }

// Removes the cursor's line and returns it as cursorLine() does. On the
// last line the preceding line break goes instead, so no empty line stays.
std::string removeCursorLine(buffer& b) {
    const std::size_t line = b.cursor().line;
    std::string removed = cursorLine(b);
    std::size_t first = b.offsetOf({.line = line, .column = 0});
    std::size_t end = b.byteSize();
    if (line + 1 < b.lineCount()) {
        end = b.offsetOf({.line = line + 1, .column = 0});
    } else if (line > 0) {
        first -= 1;
    }
    b.replace(first, end - first, {});
    b.setCursor({.line = std::min(line, b.lineCount() - 1), .column = 0});
    return removed;
}

// Moves the cursor to the first non-blank character of its line.
void moveFirstNonBlank(buffer& b) {
    const std::string line = b.line(b.cursor().line);
    const std::size_t i = line.find_first_not_of(" \t");
    b.setCursor(
        {.line = b.cursor().line, .column = i == std::string::npos ? 0 : utf8::length(line.substr(0, i))});
}

// Wakes the UI thread to collect interpreter replies. FTXUI's own
// Event::Custom is the NUL byte, which is also what terminals send for
// Ctrl+Space (Set Mark), so the app uses a private event instead.
const Event& wakeEvent() {
    static const Event wake = Event::Special("\x1b[espresso-wake]");
    return wake;
}

// The FTXUI component that hosts the app.
class host : public ComponentBase {
public:
    explicit host(app& a) : app_(a) {}

    Element OnRender() override { return app_.render(); }

    bool OnEvent(Event event) override {
        if (event == wakeEvent()) {
            app_.collectReplies();
            return true;
        }
        if (event.is_mouse()) return app_.handleMouse(event.mouse());
        const std::string key = keyNameFromInput(event.input());
        if (key.empty()) return false;
        return app_.handleKey(key);
    }

    bool Focusable() const override { return true; }

private:
    app& app_;
};

} // namespace

// ---------------------------------------------------------------------
// Construction

app::app(appOptions options)
    : clientVersion_(options.link.clientVersion),
      root_(fs::absolute(options.projectRoot).lexically_normal()),
      settingsFile_(options.settingsFile.empty() ? settingsPathFromEnvironment()
                                                 : std::optional(options.settingsFile)),
      link_(options.link),
      persist_(options.persistSettings) {
    if (settingsFile_) {
        auto loaded = loadSettings(*settingsFile_);
        settings_ = loaded.values;
        for (auto& e : loaded.errors) console_.error(std::move(e));
    } else {
        console_.error("settings: neither XDG_CONFIG_HOME nor HOME is an absolute path; "
                       "using defaults, and changes are not saved");
    }
    keys_ = keySequencer(settings_.keys);
    if (const auto found = findTheme(settings_.theme)) {
        theme_ = *found;
    } else {
        std::string names;
        for (const auto& t : themes()) names += (names.empty() ? ":" : ", :") + std::string(t.name);
        console_.error("settings: unknown theme :" + settings_.theme + " (themes: " + names +
                       "); using " + std::string(defaultTheme().title));
    }
    registerCommands();
    refreshFileList();

    for (const auto& f : options.files) openFile(f);
    if (buffers_.empty() && fs::exists(root_ / "main.lisp")) openFile("main.lisp");
    if (buffers_.empty()) addBuffer(buffer());
    current_ = 0;
}

app::~app() {
    link_.setWake({});
    persistPending();
}

int app::run() {
    auto screen = ScreenInteractive::Fullscreen();
    screen.ForceHandleCtrlC(false);
    screen.ForceHandleCtrlZ(false);
    exit_ = screen.ExitLoopClosure();
    link_.setWake([&screen] { screen.PostEvent(wakeEvent()); });
    auto component = Make<host>(*this);
    screen.Loop(component);
    link_.setWake({});
    exit_ = {};
    return 0;
}

// ---------------------------------------------------------------------
// Helpers

fs::path app::resolve(const fs::path& p) const {
    return (p.is_absolute() ? p : root_ / p).lexically_normal();
}

fs::path app::relative(const fs::path& p) const {
    const fs::path rel = p.lexically_relative(root_);
    return rel.empty() || *rel.begin() == ".." ? p : rel;
}

std::string app::displayName(const buffer& b) const {
    return b.path().empty() ? "*scratch*" : relative(b.path()).string();
}

const highlightResult& app::highlights() {
    auto& entry = *buffers_[current_];
    if (entry.highlightedVersion != entry.text.version()) {
        entry.highlights = highlight(entry.text.text());  // whole-buffer re-lex (`tui.md` §6.3)
        entry.highlightedVersion = entry.text.version();
    }
    return entry.highlights;
}

// The editor's size is known only after the first layout; until then
// the terminal size is an upper bound.
int app::editorRows() const {
    const int rows = editorBox_.y_max - editorBox_.y_min + 1;
    return rows > 1 ? rows : std::max(Terminal::Size().dimy, 10);
}

int app::editorColumns() const {
    const int columns = editorBox_.x_max - editorBox_.x_min + 1;
    return columns > 1 ? columns : std::max(Terminal::Size().dimx, 40);
}

void app::refreshFileList() { projectFiles_ = fileStore::listProject(root_); }

void app::persistPending() {
    if (std::exchange(persistPending_, false)) persistSettings();
}

void app::persistSettings() {
    if (!persist_ || !settingsFile_) return;
    const std::string error = saveSettings(*settingsFile_, settings_);
    if (!error.empty()) console_.error("settings: " + error);
}

bool app::openFile(const fs::path& path) {
    const fs::path full = resolve(path);
    for (std::size_t i = 0; i < buffers_.size(); ++i) {
        if (buffers_[i]->text.path() == full) {
            selectBuffer(i);
            return true;
        }
    }
    std::string text;
    std::error_code ec;
    if (fs::exists(full, ec)) {
        auto file = fileStore::read(full);
        if (!file.text) {
            console_.error(file.error);
            message_ = file.error;
            return false;
        }
        text = std::move(*file.text);
        message_.clear();
    } else {
        message_ = "(New file) " + relative(full).string();
    }
    // Replace an untouched scratch buffer instead of keeping it around.
    if (buffers_.size() == 1) {
        const buffer& only = buffers_[0]->text;
        if (only.path().empty() && !only.dirty() && only.byteSize() == 0) buffers_.clear();
    }
    addBuffer(buffer(text, full));
    selectBuffer(buffers_.size() - 1);
    return true;
}

void app::addBuffer(buffer b) {
    buffers_.push_back(std::make_unique<openBuffer>(openBuffer{
        .text = std::move(b), .id = nextBufferId_++, .highlightedVersion = std::nullopt, .highlights = {}}));
}

std::optional<std::size_t> app::indexOf(std::uint64_t bufferId) const {
    const auto it = std::ranges::find_if(buffers_, [&](const auto& b) { return b->id == bufferId; });
    if (it == buffers_.end()) return std::nullopt;
    return static_cast<std::size_t>(it - buffers_.begin());
}

void app::selectBuffer(std::size_t index) {
    if (index >= buffers_.size()) return;
    current_ = index;
    followCursor_ = true;
    shiftSelecting_ = false;
}

void app::jumpTo(const sourceLocation& location) {
    if (!location.path.empty() && !openFile(location.path)) return;
    buffer& b = current();
    const std::size_t line = location.line > 0 ? location.line - 1 : 0;
    const std::size_t column = location.column > 0 ? location.column - 1 : 0;
    b.setCursor({.line = line, .column = column});
    b.clearMark();
    followCursor_ = true;
}

// ---------------------------------------------------------------------
// Commands

void app::movement(bool shifted, const std::function<void(buffer&)>& move) {
    buffer& b = current();
    if (keys_.mode() == editMode::visual) {
        move(b);
        return;
    }
    if (shifted) {
        if (!b.mark() || !shiftSelecting_) b.setMark();
        shiftSelecting_ = true;
    } else if (shiftSelecting_) {
        b.clearMark();
        shiftSelecting_ = false;
    }
    move(b);  // movement keeps the mark, so a set mark extends the region
}

void app::registerCommands() {
    auto& c = commands_;
    auto move = [this](std::function<void(buffer&)> step) {
        return [this, step = std::move(step)](bool shifted) { movement(shifted, step); };
    };

    // Files and buffers
    c["openFile"] = [this](bool) {
        std::vector<std::string> names;
        names.reserve(projectFiles_.size());
        for (const auto& f : projectFiles_) names.push_back(f.string());
        openPrompt(promptKind::openFile, "Open file: ", {}, std::move(names));
    };
    c["save"] = [this](bool) { save(); };
    c["saveAs"] = [this](bool) { saveAs(); };
    c["closeBuffer"] = [this](bool) { closeBuffer(current_); };
    c["switchBuffer"] = [this](bool) {
        std::vector<std::string> names;
        names.reserve(buffers_.size());
        for (const auto& b : buffers_) names.push_back(displayName(b->text));
        openPrompt(promptKind::switchBuffer, "Switch to buffer: ", {}, std::move(names));
    };
    c["nextBuffer"] = [this](bool) { selectBuffer((current_ + 1) % buffers_.size()); };
    c["previousBuffer"] = [this](bool) {
        selectBuffer((current_ + buffers_.size() - 1) % buffers_.size());
    };
    c["quit"] = [this](bool) { quitNext(); };
    c["saveAndQuit"] = [this](bool) { save([this] { quitNext(); }); };
    c["quitWithoutSaving"] = [this](bool) {
        quit_ = true;
        if (exit_) exit_();
    };
    c["cancel"] = [this](bool) {
        current().clearMark();
        shiftSelecting_ = false;
        keys_.reset();
        menu_.reset();
        overlay_ = overlayKind::none;
        message_ = "Quit";
    };

    // Editing
    c["undo"] = [this](bool) {
        if (!current().undo()) message_ = "No further undo information";
    };
    c["redo"] = [this](bool) {
        if (!current().redo()) message_ = "No further redo information";
    };
    c["setMark"] = [this](bool) {
        current().setMark();
        shiftSelecting_ = false;
        message_ = "Mark set";
    };
    c["selectAll"] = [this](bool) {
        buffer& b = current();
        b.moveBufferEnd();
        b.setMark({.line = 0, .column = 0});
        shiftSelecting_ = true;
    };
    c["cut"] = [this](bool) {
        buffer& b = current();
        clipboard_ = b.hasSelection() ? b.deleteSelection() : removeCursorLine(b);  // no selection: the line
        if (keys_.mode() == editMode::visual) keys_.setMode(editMode::normal);
    };
    c["copy"] = [this](bool) {
        buffer& b = current();
        clipboard_ = b.hasSelection() ? b.selectedText() : cursorLine(b);
        b.clearMark();
        shiftSelecting_ = false;
        if (keys_.mode() == editMode::visual) keys_.setMode(editMode::normal);
        message_ = "Copied";
    };
    c["paste"] = [this](bool) {
        buffer& b = current();
        if (clipboard_.empty()) return;
        const bool linewise = clipboard_.back() == '\n';
        if (keys_.scheme() == keyScheme::vim && keys_.mode() == editMode::normal && linewise) {
            const std::size_t line = b.cursor().line;
            if (line + 1 < b.lineCount()) {
                b.setCursor({.line = line + 1, .column = 0});
                b.insert(clipboard_);
                b.setCursor({.line = line + 1, .column = 0});
            } else {
                b.moveLineEnd();
                b.insert("\n" + clipboard_.substr(0, clipboard_.size() - 1));
                b.setCursor({.line = line + 1, .column = 0});
            }
            return;
        }
        b.insert(clipboard_);
    };
    c["killLine"] = [this](bool) {
        std::string killed = current().killLine();
        clipboard_ = lastCommand_ == "killLine" ? clipboard_ + killed : killed;
    };
    c["deleteLine"] = [this](bool) { clipboard_ = removeCursorLine(current()); };
    c["yankLine"] = [this](bool) {
        clipboard_ = cursorLine(current());
        message_ = "Yanked line";
    };
    c["deleteBackward"] = [this](bool) { current().deleteBackward(); };
    c["deleteForward"] = [this](bool) { current().deleteForward(); };
    c["newline"] = [this](bool) { current().insertLineBreak(); };
    c["indent"] = [this](bool) { current().insert(std::string(indentWidth, ' ')); };
    c["joinLines"] = [this](bool) { current().joinLines(); };

    // Search
    c["searchForward"] = [this](bool) { startSearch(true); };
    c["searchBackward"] = [this](bool) { startSearch(false); };
    c["searchNext"] = [this](bool) { findNext(searchForward_, false); };
    c["searchPrevious"] = [this](bool) { findNext(!searchForward_, false); };

    // Movement
    c["forwardChar"] = move([](buffer& b) { b.moveRight(); });
    c["backwardChar"] = move([](buffer& b) { b.moveLeft(); });
    c["nextLine"] = move([](buffer& b) { b.moveDown(); });
    c["previousLine"] = move([](buffer& b) { b.moveUp(); });
    c["forwardWord"] = move([](buffer& b) { b.moveWordForward(); });
    c["backwardWord"] = move([](buffer& b) { b.moveWordBackward(); });
    c["lineStart"] = move([](buffer& b) { b.moveLineStart(); });
    c["lineEnd"] = move([](buffer& b) { b.moveLineEnd(); });
    c["firstNonBlank"] = move(moveFirstNonBlank);
    const auto pageRows = [this] { return static_cast<std::size_t>(std::max(editorRows() - 2, 1)); };
    c["pageDown"] = move([pageRows](buffer& b) {
        const std::size_t rows = pageRows();
        b.moveDown(rows);
        b.setTopLine(b.topLine() + rows);
    });
    c["pageUp"] = move([pageRows](buffer& b) {
        const std::size_t rows = pageRows();
        b.moveUp(rows);
        b.setTopLine(b.topLine() - std::min(rows, b.topLine()));
    });
    c["bufferStart"] = move([](buffer& b) { b.moveBufferStart(); });
    c["bufferEnd"] = move([](buffer& b) { b.moveBufferEnd(); });

    // Run controls
    c["evalLastForm"] = [this](bool) { evalLastForm(); };
    c["play"] = [this](bool) { play(); };
    c["stop"] = [this](bool) { stop(); };

    // Panes
    c["toggleFileList"] = [this](bool) {
        settings_.fileList = !settings_.fileList;
        persistSettings();
    };
    c["toggleConsole"] = [this](bool) {
        settings_.console = !settings_.console;
        persistSettings();
    };
    c["clearConsole"] = [this](bool) {
        console_.clear();
        consoleScroll_ = 0;
    };
    c["mainMenu"] = [this](bool) {
        const int right = rootBox_.x_max > 0 ? rootBox_.x_max : editorColumns();
        openMenu(mainMenu(), right, 2, true);  // below the header bar's rule
    };
    c["contextMenu"] = [this](bool) {
        const auto& b = current();
        const auto col = static_cast<int>(displayColumn(b, b.cursor()) - b.leftColumn());
        const auto row = static_cast<int>(b.cursor().line - std::min(b.topLine(), b.cursor().line));
        openMenu(contextMenu(), editorBox_.x_min + gutterWidth(b) + col, editorBox_.y_min + row + 1);
    };
    c["showPreferences"] = [this](bool) { openOverlay(overlayKind::preferences); };
    c["showShortcuts"] = [this](bool) { openOverlay(overlayKind::shortcuts); };
    c["showAbout"] = [this](bool) { openOverlay(overlayKind::about); };

    // Vim modes
    c["insertMode"] = [this](bool) { keys_.setMode(editMode::insert); };
    c["appendMode"] = [this](bool) {
        buffer& b = current();
        if (b.cursor().column < b.lineLength(b.cursor().line)) b.moveRight();
        keys_.setMode(editMode::insert);
    };
    c["appendLineEnd"] = [this](bool) {
        current().moveLineEnd();
        keys_.setMode(editMode::insert);
    };
    c["insertLineStart"] = [this](bool) {
        moveFirstNonBlank(current());
        keys_.setMode(editMode::insert);
    };
    c["openLineBelow"] = [this](bool) {
        current().moveLineEnd();
        current().insertLineBreak();
        keys_.setMode(editMode::insert);
    };
    c["openLineAbove"] = [this](bool) {
        buffer& b = current();
        b.moveLineStart();
        b.insertLineBreak();
        b.moveUp();
        keys_.setMode(editMode::insert);
    };
    c["normalMode"] = [this](bool) {
        buffer& b = current();
        if (keys_.mode() == editMode::insert && b.cursor().column > 0) b.moveLeft();
        b.clearMark();
        keys_.setMode(editMode::normal);
    };
    c["visualMode"] = [this](bool) {
        current().setMark();
        keys_.setMode(editMode::visual);
    };
    c["commandLine"] = [this](bool) { startCommandLine(); };
}

bool app::runCommand(std::string_view name, bool shifted) {
    const auto it = commands_.find(name);
    if (it == commands_.end()) {
        message_ = "Unknown command: " + std::string(name);
        return false;
    }
    if (!isMovementCommand(name) && name != "setMark" && name != "selectAll") shiftSelecting_ = false;
    it->second(shifted);
    lastCommand_ = std::string(name);
    followCursor_ = true;
    return true;
}

void app::save(std::function<void()> then) {
    buffer& b = current();
    if (b.path().empty()) {
        saveAs(std::move(then));
        return;
    }
    if (const auto error = fileStore::writeAtomic(b.path(), b.text())) {
        console_.error(*error);
        message_ = *error;
        return;  // dirty flag stays set (`tui.md` §7.3)
    }
    b.markClean();
    message_ = "Saved " + displayName(b);
    refreshFileList();
    if (settings_.rebuildOnSave) play();  // `tui.md` §9.4.1
    if (then) then();
}

void app::saveAs(std::function<void()> then) {
    promptThen_ = std::move(then);
    promptBuffer_ = buffers_[current_]->id;
    const buffer& b = current();
    openPrompt(promptKind::saveAs, "Save as: ", b.path().empty() ? "" : displayName(b));
}

void app::closeBuffer(std::size_t index, const std::function<void()>& then) {
    if (index >= buffers_.size()) return;
    // The continuation names the buffer by identity: a save may run after
    // other buffers opened or closed.
    auto close = [this, id = buffers_[index]->id, then] {
        const auto at = indexOf(id);
        if (!at) return;
        buffers_.erase(buffers_.begin() + static_cast<std::ptrdiff_t>(*at));
        if (buffers_.empty()) addBuffer(buffer());
        if (*at < current_) --current_;  // the current buffer stays current
        selectBuffer(std::min(current_, buffers_.size() - 1));
        if (then) then();
    };
    const buffer& b = buffers_[index]->text;
    if (!b.dirty()) {
        close();
        return;
    }
    selectBuffer(index);
    ask("Save Changes?",
        "“" + displayName(b) + "” has unsaved changes. Changes which are not saved will be "
        "permanently lost.",
        [this, close](char choice) {
        if (choice == 's') {
            save(close);
        } else if (choice == 'd') {
            close();
        }
    });
}

void app::quitNext() {
    for (std::size_t i = 0; i < buffers_.size(); ++i) {
        if (!buffers_[i]->text.dirty()) continue;
        selectBuffer(i);
        ask("Save Changes Before Quitting?",
            "“" + displayName(buffers_[i]->text) +
                "” has unsaved changes. If you quit without "
                "saving, the changes will be permanently lost.",
            [this, id = buffers_[i]->id](char choice) {
                if (choice == 's') {
                    save([this] { quitNext(); });
                } else if (choice == 'd') {
                    if (const auto at = indexOf(id)) buffers_[*at]->text.markClean();
                    quitNext();
                }
            });
        return;
    }
    quit_ = true;
    if (exit_) exit_();
}

void app::startSearch(bool forward) {
    searchForward_ = forward;
    searchOrigin_ = current().cursorOffset();
    openPrompt(promptKind::search, forward ? "Search: " : "Search backward: ");
}

bool app::findNext(bool forward, bool fromOrigin) {
    const std::string needle = promptKind_ == promptKind::search ? minibuffer_.text() : lastSearch_;
    buffer& b = current();
    if (needle.empty()) {
        if (fromOrigin) {
            b.clearMark();
            b.setCursor(b.positionOf(searchOrigin_));
        }
        return false;
    }
    const std::string hay = b.text();
    std::size_t pos = std::string::npos;
    if (forward) {
        const std::size_t start = fromOrigin ? searchOrigin_ : b.cursorOffset();
        pos = hay.find(needle, start);
        if (pos == std::string::npos) {
            pos = hay.find(needle);
            if (pos != std::string::npos) message_ = "Wrapped search";
        }
    } else {
        const std::size_t from = fromOrigin ? searchOrigin_ : b.selectionRange().first;
        if (from > 0) pos = hay.rfind(needle, from - 1);
        if (pos == std::string::npos) {
            pos = hay.rfind(needle);
            if (pos != std::string::npos) message_ = "Wrapped search";
        }
    }
    if (pos == std::string::npos) {
        message_ = "Not found: " + needle;
        return false;
    }
    if (forward) {
        b.setCursor(b.positionOf(pos + needle.size()));
        b.setMark(b.positionOf(pos));
    } else {
        b.setCursor(b.positionOf(pos));
        b.setMark(b.positionOf(pos + needle.size()));
    }
    followCursor_ = true;
    return true;
}

// The project's .lisp files, with unsaved buffer contents in place of
// the files on disk.
void app::addSources(runRequest& request) {
    for (const auto& f : projectFiles_) {
        if (f.extension() != ".lisp") continue;
        const fs::path full = resolve(f);
        if (auto file = fileStore::read(full); file.text) request.sources[relative(full)] = *file.text;
    }
    for (const auto& open : buffers_) {
        const buffer& b = open->text;
        if (!b.path().empty() && b.path().extension() == ".lisp") {
            request.sources[relative(b.path())] = b.text();  // unsaved edits win
        }
    }
}

void app::play() {
    runRequest request;
    request.what = runRequest::kind::play;
    request.fade = settings_.fade;
    request.quant = settings_.quant;
    if (!current().path().empty()) request.file = relative(current().path());
    addSources(request);
    if (request.sources.empty()) {
        console_.info("play: the project has no .lisp files");
        return;
    }
    message_ = "Compiling...";
    link_.submit(std::move(request));
}

void app::stop() {
    runRequest request;
    request.what = runRequest::kind::stop;
    request.fade = settings_.fade;
    link_.submit(std::move(request));
}

void app::evalLastForm() {
    buffer& b = current();
    const auto& h = highlights();
    const auto range = formBefore(h.tokens, b.cursorOffset());
    if (!range) {
        message_ = "No complete form before the cursor";
        return;
    }
    runRequest request;
    request.what = runRequest::kind::evalForm;
    const std::string text = b.text();
    request.form = text.substr(range->first, range->second - range->first);
    request.file = b.path().empty() ? fs::path("*scratch*") : relative(b.path());
    const position start = b.positionOf(range->first);
    request.line = start.line + 1;
    request.column = start.column + 1;
    request.fade = settings_.fade;
    request.quant = settings_.quant;
    addSources(request);
    link_.submit(std::move(request));
}

void app::startCommandLine() { openPrompt(promptKind::command, ":"); }

void app::setKeyScheme(keyScheme scheme) {
    settings_.keys = scheme;
    keys_ = keySequencer(scheme);
    persistSettings();
    message_ = "Key scheme: " + std::string(toString(scheme));
}

void app::setTheme(const theme& t) {
    theme_ = std::cref(t);
    settings_.theme = std::string(t.name);
    persistSettings();
}

void app::setFade(double seconds) {
    settings_.fade = std::max(seconds, 0.0);
    persistSettings();
}

void app::collectReplies() {
    for (auto& reply : link_.takeReplies()) {
        if (reply.status) status_ = *reply.status;
        if (reply.entries.empty()) continue;
        bool failed = false;
        for (auto& e : reply.entries) {
            failed = failed || e.kind == consoleKind::error;
            console_.add(e.kind, std::move(e.text), std::move(e.location));
        }
        consoleScroll_ = 0;
        message_ = failed ? "Errors found — see the console" : "";
        if (failed) settings_.console = true;
    }
}

// ---------------------------------------------------------------------
// Prompts and dialogs

void app::openPrompt(promptKind kind, std::string label, std::string initial,
                     std::vector<std::string> candidates) {
    promptKind_ = kind;
    minibuffer_.open(std::move(label), std::move(initial), std::move(candidates));
    menu_.reset();
}

void app::ask(std::string heading, std::string body, std::function<void(char)> answer) {
    ++surfaces_;
    dialog_ = dialog{.heading = std::move(heading), .body = std::move(body), .answer = std::move(answer)};
    menu_.reset();
}

void app::answerDialog(char choice) {
    if (!dialog_) return;
    auto answer = std::move(dialog_->answer);
    dialog_.reset();
    answer(choice);
}

void app::openOverlay(overlayKind kind) {
    ++surfaces_;
    overlay_ = kind;
    overlayFocus_ = 0;
    overlayScroll_ = 0;
    menu_.reset();
}

std::string_view app::overlayName() const noexcept {
    switch (overlay_) {
    case overlayKind::none: return {};
    case overlayKind::preferences: return "preferences";
    case overlayKind::shortcuts: return "shortcuts";
    case overlayKind::about: return "about";
    }
    return {};
}

void app::cancelPrompt() {
    if (promptKind_ == promptKind::search) {
        buffer& b = current();
        b.clearMark();
        b.setCursor(b.positionOf(searchOrigin_));
    }
    minibuffer_.close();
    promptKind_ = promptKind::none;
    promptThen_ = {};
    message_ = "Quit";
}

void app::submitPrompt() {
    const promptKind kind = promptKind_;
    const std::string text = minibuffer_.text();
    minibuffer_.close();
    promptKind_ = promptKind::none;
    auto then = std::exchange(promptThen_, {});
    followCursor_ = true;

    switch (kind) {
    case promptKind::none: break;
    case promptKind::openFile:
        if (!text.empty()) openFile(text);
        break;
    case promptKind::saveAs: {
        const auto at = indexOf(promptBuffer_);
        if (text.empty() || !at) break;
        selectBuffer(*at);
        current().setPath(resolve(text));
        save(std::move(then));
        break;
    }
    case promptKind::switchBuffer:
        for (std::size_t i = 0; i < buffers_.size(); ++i) {
            if (displayName(buffers_[i]->text) == text) {
                selectBuffer(i);
                return;
            }
        }
        if (!text.empty()) openFile(text);
        break;
    case promptKind::search:
        lastSearch_ = text;
        current().clearMark();
        break;
    case promptKind::command: {
        const auto ex = resolveExCommand(text);
        if (ex.command.empty()) {
            message_ = "Not an editor command: " + text;
        } else if (ex.command == "openFile" && !ex.argument.empty()) {
            openFile(std::string(ex.argument));
        } else if (ex.command == "saveAs" && !ex.argument.empty()) {
            current().setPath(resolve(std::string(ex.argument)));
            save();
        } else {
            runCommand(ex.command);
        }
        break;
    }
    case promptKind::fade:
        if (const auto v = parseDuration(text, 1e-3)) {
            setFade(*v);
        } else {
            message_ = "Invalid fade: " + text;
        }
        break;
    }
}

bool app::handlePromptKey(std::string_view key) {
    auto& mb = minibuffer_;
    bool changed = false;
    if (key == "RET") {
        submitPrompt();
        return true;
    }
    if (key == "ESC" || key == "C-g") {
        cancelPrompt();
        return true;
    }
    if (isPrintableKey(key) || key == "SPC") {
        mb.insert(key == "SPC" ? " " : key);
        changed = true;
    } else if (key == "DEL") {
        mb.deleteBackward();
        changed = true;
    } else if (key == "<delete>" || key == "C-d") {
        mb.deleteForward();
        changed = true;
    } else if (key == "<left>" || key == "C-b") {
        mb.moveLeft();
    } else if (key == "<right>" || key == "C-f") {
        mb.moveRight();
    } else if (key == "<home>" || key == "C-a") {
        mb.moveStart();
    } else if (key == "<end>" || key == "C-e") {
        mb.moveEnd();
    } else if (key == "TAB") {
        changed = mb.complete();
    } else if (promptKind_ == promptKind::search) {
        // The scheme's search keys move between matches while searching.
        std::string_view command;
        for (const auto& b : schemeBindings(keys_.scheme(), editMode::insert)) {
            if (b.keys == key) command = b.command;
        }
        if (command == "searchForward" || command == "searchNext") {
            searchForward_ = true;
            findNext(true, false);
        } else if (command == "searchBackward" || command == "searchPrevious") {
            searchForward_ = false;
            findNext(false, false);
        }
        return true;
    }
    if (changed && promptKind_ == promptKind::search) findNext(searchForward_, true);
    return true;
}

bool app::handleDialogKey(std::string_view key) {
    char choice = 0;
    // Enter takes the default response, Save (the suggested action).
    if (key == "RET" || key == "s" || key == "S") choice = 's';
    if (key == "d" || key == "D") choice = 'd';
    if (key == "c" || key == "C" || key == "ESC" || key == "C-g") choice = 'c';
    if (choice != 0) answerDialog(choice);
    return true;  // the dialog keeps the keyboard
}

std::size_t app::wrapIndex(std::size_t i, int dir, std::size_t n) noexcept {
    const auto size = static_cast<std::ptrdiff_t>(n);
    return static_cast<std::size_t>(((static_cast<std::ptrdiff_t>(i) + dir) % size + size) % size);
}

bool app::handleMenuKey(std::string_view key) {
    if (!menu_) return false;  // a press on a row drawn before the menu closed
    auto& entries = *menu_;
    auto step = [&](int dir) {
        const std::size_t n = entries.size();
        for (std::size_t k = 0; k < n; ++k) {
            menuFocus_ = wrapIndex(menuFocus_, dir, n);
            if (entries[menuFocus_].usable()) return;
        }
    };
    if (key == "<down>" || key == "C-n" || key == "TAB" || key == "j") {
        step(1);
    } else if (key == "<up>" || key == "C-p" || key == "S-TAB" || key == "k") {
        step(-1);
    } else if ((key == "RET" || key == "SPC") && menuFocus_ < entries.size()) {
        if (!entries[menuFocus_].sensitive) return true;  // nothing to do; the menu stays open
        auto action = entries[menuFocus_].action;
        menu_.reset();
        if (action) action();
        followCursor_ = true;
    } else {
        menu_.reset();  // Escape and any other key close the popover
    }
    return true;
}

bool app::handleOverlayKey(std::string_view key) {
    if (key == "ESC" || key == "C-g" || key == "q" ||
        (overlay_ == overlayKind::about && key == "RET")) {
        overlay_ = overlayKind::none;
        return true;
    }
    if (overlay_ == overlayKind::preferences) {
        const auto rows = preferenceRows();
        if (key == "<down>" || key == "C-n" || key == "TAB" || key == "j") {
            overlayFocus_ = wrapIndex(overlayFocus_, 1, rows.size());
        } else if (key == "<up>" || key == "C-p" || key == "S-TAB" || key == "k") {
            overlayFocus_ = wrapIndex(overlayFocus_, -1, rows.size());
        } else if (key == "<left>" || key == "h") {
            rows[overlayFocus_].change(-1);
        } else if (key == "<right>" || key == "l" || key == "RET" || key == "SPC") {
            rows[overlayFocus_].change(1);
        }
    } else if (overlay_ == overlayKind::shortcuts) {
        if (key == "<down>" || key == "C-n" || key == "j") ++overlayScroll_;
        if ((key == "<up>" || key == "C-p" || key == "k") && overlayScroll_ > 0) --overlayScroll_;
        if (key == "<next>" || key == "C-v") overlayScroll_ += 10;
        if (key == "<prior>" || key == "M-v") overlayScroll_ -= std::min<std::size_t>(10, overlayScroll_);
    }
    return true;
}

// ---------------------------------------------------------------------
// Input

bool app::handleKey(std::string_view key) {
    collectReplies();
    persistPending();
    if (dialog_) return handleDialogKey(key);
    if (menu_) return handleMenuKey(key);
    if (overlay_ != overlayKind::none) return handleOverlayKey(key);
    if (minibuffer_.active()) return handlePromptKey(key);

    message_.clear();
    const auto r = keys_.feed(key);
    switch (r.kind) {
    case keySequencer::outcome::command: {
        const bool shifted = key.starts_with("S-") || key.find("-S-") != std::string_view::npos;
        runCommand(r.command, shifted);
        return true;
    }
    case keySequencer::outcome::pending:
        message_ = r.keys + "-";
        return true;
    case keySequencer::outcome::selfInsert:
        current().insert(key);
        shiftSelecting_ = false;
        lastCommand_ = "selfInsert";
        followCursor_ = true;
        return true;
    case keySequencer::outcome::unbound:
        if (r.keys == "SPC" && keys_.insertsText()) {
            current().insert(" ");
            followCursor_ = true;
            return true;
        }
        message_ = r.keys + " is undefined";
        return false;
    }
    return false;
}

const ftxui::Box& app::boxOf(region r) const noexcept {
    switch (r) {
    case region::menu: return menuBox_;
    case region::fileList: return fileListBox_;
    case region::tabBar: return tabBarBox_;
    case region::console: return consoleBox_;
    case region::message: return messageBox_;
    case region::dialog: return dialogBox_;
    case region::overlay: return overlayBox_;
    case region::none: break;
    }
    return rootBox_;
}

bool app::regionOpen(region r) const noexcept {
    switch (r) {
    case region::menu: return menu_.has_value();
    case region::dialog: return dialog_.has_value();
    case region::overlay: return overlay_ != overlayKind::none;
    case region::none:
    case region::fileList:
    case region::tabBar:
    case region::console:
    case region::message: break;
    }
    return true;
}

bool app::isPopover(region r) noexcept {
    return r == region::menu || r == region::dialog || r == region::overlay;
}

app::region app::activeLayer() const noexcept {
    if (menu_) return region::menu;
    if (dialog_) return region::dialog;
    return overlay_ != overlayKind::none ? region::overlay : region::none;
}

bool app::reachable(const target& t) const {
    // Targets come from the last render: skip those of a popover or modal
    // that has closed or been replaced since.
    if (!regionOpen(t.within) || (isPopover(t.within) && t.surface != surfaces_)) return false;
    const region layer = activeLayer();
    return layer == region::none || t.within == layer;
}

bool app::hits(const target& t, int x, int y) const {
    return t.box.Contain(x, y) && (t.within == region::none || boxOf(t.within).Contain(x, y));
}

// Hover, press, and wheel use the same rule, so a control lights up only
// if a press would reach it. A control drawn inside another, such as a
// button on a draggable heading, wins over it; otherwise the one drawn
// last, which is on top, wins.
std::optional<std::size_t> app::targetAt(int x, int y, input kind) const {
    auto inside = [](const Box& a, const Box& b) {
        return !(a == b) && a.x_min >= b.x_min && a.x_max <= b.x_max && a.y_min >= b.y_min &&
               a.y_max <= b.y_max;
    };
    std::optional<std::size_t> best;
    for (std::size_t i = targets_.size(); i-- > 0;) {
        const target& t = targets_[i];
        const bool responds =
            kind == input::press ? static_cast<bool>(t.press) : static_cast<bool>(t.options.wheel);
        if (!responds || !reachable(t) || !hits(t, x, y)) continue;
        if (!best || inside(t.box, targets_[*best].box)) best = i;
    }
    return best;
}

std::optional<std::pair<std::size_t, app::controlState>> app::litControl() const {
    const auto under = targetAt(pointer_.first, pointer_.second);
    if (!under) return std::nullopt;
    const target& t = targets_[*under];
    if (t.options.kind == look::none || !t.options.sensitive) return std::nullopt;
    if (!pressed_) return std::pair{*under, controlState::hover};
    // Pressed while the pointer stays on the control it went down on; a
    // divider stays pressed while it is dragged.
    if (t.box == *pressed_ || (dragAction_ && t.options.drag))
        return std::pair{*under, controlState::pressed};
    return std::nullopt;
}

bool app::handleMouse(const Mouse& mouse) {
    collectReplies();
    const int x = mouse.x;
    const int y = mouse.y;
    pointer_ = {x, y};

    if (mouse.button == Mouse::WheelUp || mouse.button == Mouse::WheelDown) {
        const auto under = targetAt(x, y, input::wheel);
        if (!under) return false;
        targets_[*under].options.wheel(mouse.button == Mouse::WheelUp ? -1 : 1);
        return true;
    }

    if (mouse.motion == Mouse::Released) {
        const bool wasDown = std::exchange(pressed_, std::nullopt).has_value();
        if (!dragAction_) return wasDown;  // the pressed control returns to rest
        dragAction_ = {};
        persistPending();
        return true;
    }
    if (mouse.motion == Mouse::Moved) {
        if (dragAction_) {
            dragAction_(mouse);
            return true;
        }
        // Hover shows the tooltip of the control under the pointer, and may
        // move a menu's focus to it.
        const auto under = targetAt(x, y);
        if (!under) {
            hoverTip_.clear();
            return true;
        }
        const control& c = targets_[*under].options;
        hoverTip_ = c.tooltip;
        if (c.enter && c.sensitive) c.enter();
        return true;  // the control under the pointer changes how it is drawn
    }
    if (mouse.motion != Mouse::Pressed) return false;
    // An empty box, which no control has: held down away from every control.
    if (mouse.button == Mouse::Left) pressed_ = Box{.x_min = 1, .x_max = 0, .y_min = 1, .y_max = 0};
    persistPending();
    // A press outside the prompt cancels it, so its action cannot apply to
    // a buffer or state the press changes.
    if (minibuffer_.active() && !messageBox_.Contain(x, y)) cancelPrompt();
    if (mouse.button == Mouse::Right && activeLayer() == region::none && editorBox_.Contain(x, y)) {
        openMenu(contextMenu(), x, y + 1);  // context menu (HIG: duplicates other routes)
        return true;
    }
    if (mouse.button != Mouse::Left) return false;

    if (menu_ && !menuBox_.Contain(x, y)) {
        menu_.reset();  // a press outside the menu closes it
        return true;
    }
    const auto under = targetAt(x, y);
    if (!under) return false;
    const target& candidate = targets_[*under];
    pressed_ = candidate.box;
    if (!candidate.options.sensitive)
        return true;  // a control that cannot be used takes the press and does nothing
    auto drag = candidate.options.drag;
    candidate.press(mouse);  // may change state; `candidate` stays valid until the next render
    dragAction_ = std::move(drag);
    return true;
}

} // namespace tui
