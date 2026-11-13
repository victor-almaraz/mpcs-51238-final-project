#include <proj/tui/app.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <system_error>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/terminal.hpp>

#include <proj/tui/editorView.h>
#include <proj/tui/fileStore.h>
#include <proj/tui/keyNames.h>
#include <proj/tui/utf8.h>

namespace fs = std::filesystem;
using namespace ftxui;

namespace tui {

namespace {

constexpr std::size_t indentWidth = 2;

std::string formatMs(double seconds) {
    std::ostringstream out;
    out << std::round(seconds * 1000.0 * 1000.0) / 1000.0 << "ms";
    return out.str();
}

// Parses "50", "50ms", "0.1s" as seconds; plain numbers are milliseconds.
std::optional<double> parseFade(std::string_view text) {
    std::string s(text);
    double scale = 1e-3;
    if (s.ends_with("ms")) {
        s.resize(s.size() - 2);
    } else if (s.ends_with("s")) {
        s.resize(s.size() - 1);
        scale = 1.0;
    }
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (s.empty() || end != s.c_str() + s.size() || !(v >= 0.0) || !std::isfinite(v)) {
        return std::nullopt;
    }
    return v * scale;
}

bool isMovement(std::string_view command) {
    static constexpr std::string_view moves[] = {
        "forwardChar", "backwardChar", "nextLine", "previousLine", "forwardWord", "backwardWord",
        "lineStart", "lineEnd", "firstNonBlank", "pageDown", "pageUp", "bufferStart", "bufferEnd"};
    return std::find(std::begin(moves), std::end(moves), command) != std::end(moves);
}

// The FTXUI component that hosts the app.
class host : public ComponentBase {
public:
    explicit host(app& a) : app_(a) {}

    Element OnRender() override { return app_.render(); }

    bool OnEvent(Event event) override {
        if (event == Event::Custom) {
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
    : options_(std::move(options)),
      root_(fs::absolute(options_.projectRoot).lexically_normal()),
      settingsFile_(options_.settingsFile.empty() ? settingsPathFromEnvironment()
                                                  : options_.settingsFile),
      link_(options_.link) {
    auto loaded = loadSettings(settingsFile_);
    settings_ = loaded.values;
    for (auto& e : loaded.errors) console_.error(std::move(e));
    keys_ = keySequencer(settings_.keys);
    registerCommands();
    refreshFileList();

    for (const auto& f : options_.files) openFile(f);
    if (buffers_.empty() && fs::exists(root_ / "main.lisp")) openFile("main.lisp");
    if (buffers_.empty()) buffers_.push_back(std::make_unique<buffer>());
    current_ = 0;
}

app::~app() { link_.setWake({}); }

int app::run() {
    auto screen = ScreenInteractive::Fullscreen();
    screen.ForceHandleCtrlC(false);
    screen.ForceHandleCtrlZ(false);
    exit_ = screen.ExitLoopClosure();
    link_.setWake([&screen] { screen.PostEvent(Event::Custom); });
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

const highlightResult& app::highlights(const buffer& b) {
    auto& entry = highlightCache_[&b];
    if (entry.first != b.version()) {
        entry.second = highlight(b.text());  // whole-buffer re-lex (`tui.md` §6.3)
        entry.first = b.version();
    }
    return entry.second;
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

void app::persistSettings() {
    if (!options_.persistSettings) return;
    const std::string error = saveSettings(settingsFile_, settings_);
    if (!error.empty()) console_.error("settings: " + error);
}

bool app::openFile(const fs::path& path) {
    const fs::path full = resolve(path);
    for (std::size_t i = 0; i < buffers_.size(); ++i) {
        if (buffers_[i]->path() == full) {
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
    if (buffers_.size() == 1 && buffers_[0]->path().empty() && !buffers_[0]->dirty() &&
        buffers_[0]->byteSize() == 0) {
        highlightCache_.erase(buffers_[0].get());
        buffers_.clear();
    }
    buffers_.push_back(std::make_unique<buffer>(text, full));
    selectBuffer(buffers_.size() - 1);
    return true;
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
    b.setCursor({line, column});
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
    auto move = [this](void (buffer::*fn)()) {
        return [this, fn](bool shifted) { movement(shifted, [fn](buffer& b) { (b.*fn)(); }); };
    };

    // Files and buffers
    c["openFile"] = [this](bool) {
        std::vector<std::string> names;
        for (const auto& f : projectFiles_) names.push_back(f.string());
        openPrompt(promptKind::openFile, "Open file: ", {}, std::move(names));
    };
    c["save"] = [this](bool) { save(); };
    c["saveAs"] = [this](bool) { saveAs(); };
    c["closeBuffer"] = [this](bool) { closeBuffer(current_); };
    c["switchBuffer"] = [this](bool) {
        std::vector<std::string> names;
        for (const auto& b : buffers_) names.push_back(displayName(*b));
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
        b.setMark({0, 0});
        shiftSelecting_ = true;
    };
    c["cut"] = [this](bool) {
        buffer& b = current();
        if (b.hasSelection()) {
            clipboard_ = b.deleteSelection();
        } else {
            // Without a selection, cut the whole line.
            const std::size_t line = b.cursor().line;
            const std::size_t first = b.offsetOf({line, 0});
            const bool last = line + 1 >= b.lineCount();
            const std::size_t end = last ? b.offsetOf({line, b.lineLength(line)})
                                         : b.offsetOf({line + 1, 0});
            clipboard_ = b.line(line) + "\n";
            b.replace(first, end - first, {});
        }
        if (keys_.mode() == editMode::visual) keys_.setMode(editMode::normal);
    };
    c["copy"] = [this](bool) {
        buffer& b = current();
        clipboard_ = b.hasSelection() ? b.selectedText() : b.line(b.cursor().line) + "\n";
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
                b.setCursor({line + 1, 0});
                b.insert(clipboard_);
                b.setCursor({line + 1, 0});
            } else {
                b.moveLineEnd();
                b.insert("\n" + clipboard_.substr(0, clipboard_.size() - 1));
                b.setCursor({line + 1, 0});
            }
            return;
        }
        b.insert(clipboard_);
    };
    c["killLine"] = [this](bool) {
        std::string killed = current().killLine();
        clipboard_ = lastCommand_ == "killLine" ? clipboard_ + killed : killed;
    };
    c["deleteLine"] = [this](bool) {
        buffer& b = current();
        const std::size_t line = b.cursor().line;
        clipboard_ = b.line(line) + "\n";
        std::size_t first = b.offsetOf({line, 0});
        std::size_t end = 0;
        if (line + 1 < b.lineCount()) {
            end = b.offsetOf({line + 1, 0});
        } else {
            end = b.byteSize();
            if (line > 0) first -= 1;  // remove the preceding line break instead
        }
        b.replace(first, end - first, {});
        b.setCursor({std::min(line, b.lineCount() - 1), 0});
    };
    c["yankLine"] = [this](bool) {
        clipboard_ = current().line(current().cursor().line) + "\n";
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
    c["forwardChar"] = move(&buffer::moveRight);
    c["backwardChar"] = move(&buffer::moveLeft);
    c["nextLine"] = [this](bool s) { movement(s, [](buffer& b) { b.moveDown(); }); };
    c["previousLine"] = [this](bool s) { movement(s, [](buffer& b) { b.moveUp(); }); };
    c["forwardWord"] = move(&buffer::moveWordForward);
    c["backwardWord"] = move(&buffer::moveWordBackward);
    c["lineStart"] = move(&buffer::moveLineStart);
    c["lineEnd"] = move(&buffer::moveLineEnd);
    c["firstNonBlank"] = [this](bool s) {
        movement(s, [](buffer& b) {
            const std::string line = b.line(b.cursor().line);
            const std::size_t i = line.find_first_not_of(" \t");
            b.setCursor({b.cursor().line, i == std::string::npos ? 0 : utf8::length(line.substr(0, i))});
        });
    };
    c["pageDown"] = [this](bool s) {
        const auto rows = static_cast<std::size_t>(std::max(editorRows() - 2, 1));
        movement(s, [rows](buffer& b) {
            b.moveDown(rows);
            b.setTopLine(b.topLine() + rows);
        });
    };
    c["pageUp"] = [this](bool s) {
        const auto rows = static_cast<std::size_t>(std::max(editorRows() - 2, 1));
        movement(s, [rows](buffer& b) {
            b.moveUp(rows);
            b.setTopLine(b.topLine() - std::min(rows, b.topLine()));
        });
    };
    c["bufferStart"] = move(&buffer::moveBufferStart);
    c["bufferEnd"] = move(&buffer::moveBufferEnd);

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
        commands_["firstNonBlank"](false);
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
    if (!isMovement(name) && name != "setMark" && name != "selectAll") shiftSelecting_ = false;
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
    const buffer& b = current();
    openPrompt(promptKind::saveAs, "Save as: ", b.path().empty() ? "" : displayName(b));
}

void app::closeBuffer(std::size_t index, std::function<void()> then) {
    if (index >= buffers_.size()) return;
    auto close = [this, index, then] {
        highlightCache_.erase(buffers_[index].get());
        buffers_.erase(buffers_.begin() + static_cast<std::ptrdiff_t>(index));
        if (buffers_.empty()) buffers_.push_back(std::make_unique<buffer>());
        if (current_ >= buffers_.size()) current_ = buffers_.size() - 1;
        selectBuffer(current_);
        if (then) then();
    };
    buffer& b = *buffers_[index];
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
        if (!buffers_[i]->dirty()) continue;
        selectBuffer(i);
        ask("Save Changes Before Quitting?",
            "“" + displayName(*buffers_[i]) + "” has unsaved changes. If you quit without "
            "saving, the changes will be permanently lost.",
            [this, i](char choice) {
                if (choice == 's') {
                    save([this] { quitNext(); });
                } else if (choice == 'd') {
                    buffers_[i]->markClean();
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

void app::play() {
    runRequest request;
    request.what = runRequest::kind::play;
    request.fade = settings_.fade;
    request.quant = settings_.quant;
    for (const auto& f : projectFiles_) {
        if (f.extension() != ".lisp") continue;
        const fs::path full = resolve(f);
        if (auto file = fileStore::read(full); file.text) request.sources[relative(full)] = *file.text;
    }
    for (const auto& b : buffers_) {
        if (!b->path().empty() && b->path().extension() == ".lisp") {
            request.sources[relative(b->path())] = b->text();  // unsaved edits win
        }
    }
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
    const auto& h = highlights(b);
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
    link_.submit(std::move(request));
}

void app::startCommandLine() { openPrompt(promptKind::command, ":"); }

void app::setKeyScheme(keyScheme scheme) {
    settings_.keys = scheme;
    keys_ = keySequencer(scheme);
    persistSettings();
    message_ = "Key scheme: " + std::string(toString(scheme));
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
        if (failed && !settings_.console) settings_.console = true;
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
    dialog_ = dialog{std::move(heading), std::move(body), std::move(answer)};
    menu_.reset();
}

void app::answerDialog(char choice) {
    if (!dialog_) return;
    auto answer = std::move(dialog_->answer);
    dialog_.reset();
    answer(choice);
}

void app::openOverlay(overlayKind kind) {
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
    case promptKind::saveAs:
        if (text.empty()) break;
        current().setPath(resolve(text));
        save(std::move(then));
        break;
    case promptKind::switchBuffer:
        for (std::size_t i = 0; i < buffers_.size(); ++i) {
            if (displayName(*buffers_[i]) == text) {
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
        if (const auto v = parseFade(text)) {
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

bool app::handleMenuKey(std::string_view key) {
    auto& entries = *menu_;
    auto step = [&](int dir) {
        const std::size_t n = entries.size();
        for (std::size_t k = 0; k < n; ++k) {
            menuFocus_ = (menuFocus_ + n + static_cast<std::size_t>(dir + static_cast<int>(n))) % n;
            if (!entries[menuFocus_].label.empty()) return;
        }
    };
    if (key == "<down>" || key == "C-n" || key == "TAB" || key == "j") {
        step(1);
    } else if (key == "<up>" || key == "C-p" || key == "S-TAB" || key == "k") {
        step(-1);
    } else if (key == "RET" || key == "SPC") {
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
            overlayFocus_ = (overlayFocus_ + 1) % rows.size();
        } else if (key == "<up>" || key == "C-p" || key == "S-TAB" || key == "k") {
            overlayFocus_ = (overlayFocus_ + rows.size() - 1) % rows.size();
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

bool app::handleMouse(const Mouse& mouse) {
    collectReplies();
    const int x = mouse.x;
    const int y = mouse.y;
    auto hit = [&](const target& t) {
        return t.box.Contain(x, y) && (t.within == nullptr || t.within->Contain(x, y));
    };

    if (mouse.button == Mouse::WheelUp || mouse.button == Mouse::WheelDown) {
        const int dir = mouse.button == Mouse::WheelUp ? -1 : 1;
        for (auto it = targets_.rbegin(); it != targets_.rend(); ++it) {
            if (it->wheel && hit(*it)) {
                it->wheel(dir);
                return true;
            }
        }
        return false;
    }

    if (mouse.motion == Mouse::Released) {
        if (!dragAction_) return false;
        dragAction_ = {};
        if (std::exchange(persistOnRelease_, false)) persistSettings();
        return true;
    }
    if (mouse.motion == Mouse::Moved) {
        if (dragAction_) {
            dragAction_(mouse);
            return true;
        }
        // Hover shows the tooltip of the control under the pointer.
        std::string tip;
        for (auto it = targets_.rbegin(); it != targets_.rend(); ++it) {
            if (hit(*it)) {
                tip = it->tooltip;
                break;
            }
        }
        if (tip == hoverTip_) return false;
        hoverTip_ = std::move(tip);
        return true;
    }
    if (mouse.motion != Mouse::Pressed) return false;
    if (mouse.button == Mouse::Right && !menu_ && !dialog_ && overlay_ == overlayKind::none &&
        editorBox_.Contain(x, y)) {
        openMenu(contextMenu(), x, y + 1);  // context menu (HIG: duplicates other routes)
        return true;
    }
    if (mouse.button != Mouse::Left) return false;

    if (menu_ && !menuBox_.Contain(x, y)) {
        menu_.reset();
        return true;
    }
    // Dialogs and overlays are modal: only their own controls respond.
    const ftxui::Box* modal = dialog_ ? &dialogBox_
                              : overlay_ != overlayKind::none ? &overlayBox_
                                                              : nullptr;
    for (auto it = targets_.rbegin(); it != targets_.rend(); ++it) {
        if (menu_ && it->within != &menuBox_) continue;
        if (!menu_ && modal && it->within != modal) continue;
        if (it->press && hit(*it)) {
            auto drag = it->drag;
            it->press(mouse);  // may change state; `it` stays valid until the next render
            dragAction_ = std::move(drag);
            return true;
        }
    }
    return false;
}

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

Element suggestedStyle(Element e) { return e | bgcolor(Color::Blue) | color(Color::White) | bold; }
Element destructiveStyle(Element e) { return e | bgcolor(Color::Red) | color(Color::White) | bold; }
Element fieldStyle(Element e) { return e | bgcolor(Color::GrayDark); }
Element selectedStyle(Element e) { return e | bgcolor(Color::GrayDark) | bold; }

// Raised surfaces: every popover, dialog, overlay, and the open prompt
// line has its own background, border included, so it stands apart
// from the window behind it. Controls on a surface use the styles
// below, which keep their contrast against it.
Element surface(Element e) { return e | color(Color::White) | bgcolor(Color::GrayDark); }
Element surfaceSelected(Element e) { return suggestedStyle(std::move(e)); }
Element surfaceButton(Element e) { return e | bgcolor(Color::GrayLight) | color(Color::Black); }
Element surfaceSecondary(Element e) { return e | color(Color::GrayLight); }

// Clears a one-cell margin around a bordered box, so that its border
// never joins the lines of the content behind it.
Element margin(Element box) {
    return vbox({text(""), hbox({text(" "), std::move(box), text(" ")}), text("")}) | clear_under;
}

// Popovers hang below their trigger, so their margin covers the sides
// and bottom only; the row above stays visible.
Element popoverMargin(Element box) {
    return vbox({hbox({text(" "), std::move(box), text(" ")}), text("")}) | clear_under;
}

// A modal box centered over the window.
Element centered(Element box) {
    return vbox({filler(), hbox({filler(), margin(std::move(box)), filler()}), filler()});
}

std::string spacedMs(double seconds) {
    std::ostringstream out;
    out << std::round(seconds * 1000.0 * 1000.0) / 1000.0 << " ms";  // unit after a space
    return out.str();
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

bool app::narrow() const noexcept {
    const int width = rootBox_.x_max - rootBox_.x_min + 1;
    return (width > 1 ? width : Terminal::Size().dimx) < narrowColumns;
}

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
    clipTo_ = &menuBox_;
    std::size_t width = 0;
    for (const auto& e : *menu_) {
        width = std::max(width, utf8::length(e.label) + utf8::length(e.hint) + 6);
    }
    Elements rows;
    for (std::size_t i = 0; i < menu_->size(); ++i) {
        const menuEntry& entry = (*menu_)[i];
        if (entry.label.empty()) {
            rows.push_back(separatorLight());
            continue;
        }
        const std::string check = entry.checked ? (*entry.checked ? "✓ " : "  ") : "  ";
        const bool focused = i == menuFocus_;
        Element hint = text(entry.hint.empty() ? "" : "  " + entry.hint);
        Element row = hbox({text(check), text(entry.label), filler(),
                            focused ? hint : surfaceSecondary(hint), text(" ")});
        if (focused) row = surfaceSelected(row);  // accent highlight across the whole row
        rows.push_back(clickable(row, [this, i](const Mouse&) {
            menuFocus_ = i;
            handleMenuKey("RET");
        }));
    }
    clipTo_ = nullptr;
    const int w = static_cast<int>(width) + 2;  // plus the border
    Element popup = surface(vbox(std::move(rows)) | size(WIDTH, EQUAL, w - 2) | borderRounded) |
                    clear_under | reflect(menuBox_);
    // The margin starts one cell left of the popover.
    const int x = std::max((menuAlignRight_ ? menuX_ - w + 1 : menuX_) - 1, 0);
    return vbox({
        text("") | size(HEIGHT, EQUAL, std::max(menuY_, 0)),
        hbox({text("") | size(WIDTH, EQUAL, x), popoverMargin(popup), filler()}),
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
    const bool compact = narrow();
    const std::string play = " ▶ Play ";
    const std::string stopLabel = " ■ Stop ";
    Elements start{
        clickable(suggestedStyle(text(play)), [this](const Mouse&) { runCommand("play"); }, {}, {},
                  "Compile and play the project · " + keyHint("play")),
        text(" "),
        clickable(text(stopLabel), [this](const Mouse&) { runCommand("stop"); }, {}, {},
                  "Stop after the fade · " + keyHint("stop")),
    };
    const int startWidth = cells({play, " ", stopLabel});

    const std::string fadeLabel = compact ? "" : "Fade ";
    const std::string fadeValue = " " + spacedMs(settings_.fade) + " ";
    const std::string quantLabel = compact ? "" : "Quantize ";
    const std::string quantValue = " " + titleCase(toString(settings_.quant)) + " ▾ ";
    // ≡ rather than ☰: U+2630 is double width in Unicode 16 and would
    // misalign the row in terminals that follow it.
    const std::string menuButton = " ≡ ";
    const std::string closeButton = " × ";
    Elements end{
        text(fadeLabel) | dim,
        clickable(
            fieldStyle(text(fadeValue)),
            [this](const Mouse&) {
                openPrompt(promptKind::fade, "Fade (ms): ", formatMs(settings_.fade));
            },
            [this](int dir) { setFade(settings_.fade - dir * 0.01); }, {},
            "Swap crossfade · click to type, scroll to adjust"),
        text("  "),
        text(quantLabel) | dim,
        clickable(
            fieldStyle(text(quantValue)),
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
    const int rootWidth = rootBox_.x_max - rootBox_.x_min + 1;
    const int width = rootWidth > 1 ? rootWidth : Terminal::Size().dimx;
    Element titleRow = text("");
    if (2 * std::max(startWidth, endWidth) + titleWidth + 4 <= width) {
        titleRow = hbox({filler(), text(title) | bold, text(subtitle) | dim, filler()});
    }
    return dbox({titleRow, hbox({hbox(std::move(start)), filler(), hbox(std::move(end))})});
}

Element app::renderSidebar() {
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
        if (resolve(f) == currentPath) row = selectedStyle(row);
        rows.push_back(clickable(row, [this, f](const Mouse&) { openFile(f); }, wheel));
    }
    clipTo_ = nullptr;
    Element list = rows.empty()
                       ? vbox({text(""), text("No Files") | bold | hcenter,
                               paragraphAlignCenter("Open a file to start") | dim, filler()})
                       : vbox(std::move(rows));
    // The heading row lines up with the tab bar, and its rule with the
    // rule under the tabs.
    return vbox({hbox({text(" " + root_.filename().string()) | bold, filler()}), separator(),
                 list | yframe | flex}) |
           size(WIDTH, EQUAL, settings_.fileListWidth) | reflect(fileListBox_);
}

Element app::renderTabBar() {
    clipTo_ = &tabBarBox_;
    Elements tabs;
    for (std::size_t i = 0; i < buffers_.size(); ++i) {
        const buffer& b = *buffers_[i];
        Element name = clickable(text(" " + displayName(b) + (b.dirty() ? " •" : "") + " "),
                                 [this, i](const Mouse&) { selectBuffer(i); });
        Element close = clickable(text("× "), [this, i](const Mouse&) { closeBuffer(i); }, {}, {},
                                  "Close tab · " + keyHint("closeBuffer"));
        Element tab = hbox({name, close});
        tabs.push_back(i == current_ ? selectedStyle(tab) : tab | dim);
        tabs.push_back(text(" "));
    }
    tabs.push_back(clickable(text(" + "), [this](const Mouse&) { runCommand("openFile"); }, {}, {},
                             "Open a file in a new tab · " + keyHint("openFile")));
    clipTo_ = nullptr;
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
    Element view = renderEditor(b, highlights(b), rows, columns, {focused, true});
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
    // Pane heading; dragging it resizes the console.
    auto drag = [this](const Mouse& m) {
        settings_.consoleHeight = std::clamp(consoleBox_.y_max - m.y, 1, 200);
        persistOnRelease_ = true;
    };
    Element clear = clickable(text(" Clear "), [this](const Mouse&) { runCommand("clearConsole"); },
                              {}, {}, "Remove all console messages");
    Element heading = hbox({text("─ "), text("Console") | bold, text(" "),
                            separatorCharacter("─") | flex, clear, text("─")});
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
        case consoleKind::error: row = row | color(Color::Red); break;
        case consoleKind::server: row = row | color(Color::Blue); break;
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
    Element body = rows.empty() ? vbox({filler(), text("No Messages") | dim | hcenter, filler()})
                                : vbox(std::move(rows));
    return vbox({heading, body | yframe | size(HEIGHT, EQUAL, height) | reflect(consoleBox_)});
}

Element app::renderStatusBar() {
    const serverStatus& s = status_;
    // The dot's shape and the text both carry the state, not color alone.
    const std::string server = s.connected ? s.server : "Not connected";
    Element state = hbox({s.connected ? text(" ● ") | color(Color::Green) : text(" ○ ") | dim,
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
    const int rootWidth = rootBox_.x_max - rootBox_.x_min + 1;
    const int width = rootWidth > 1 ? rootWidth : Terminal::Size().dimx;
    const bool fits = cells({"   ", server, detail.str(), right.str()}) + 2 <= width;
    return hbox({state, fits ? text(detail.str()) | dim : text(""), filler(), text(right.str()) | dim});
}

Element app::renderMessageLine() {
    clipTo_ = &messageBox_;
    Element line = [this]() -> Element {
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
                        parts.push_back(surfaceSecondary(text(" …")));
                        break;
                    }
                    parts.push_back(clickable(surfaceSecondary(text(m)), [this, m](const Mouse&) {
                        minibuffer_.setText(m);
                        submitPrompt();
                    }));
                    parts.push_back(text("  "));
                }
            }
            parts.push_back(filler());
            return surface(hbox(std::move(parts)));  // an open prompt is a surface too
        }
        const std::string pending = keys_.pendingKeys();
        if (!message_.empty()) return text(" " + message_);
        if (!pending.empty()) return text(" " + pending + "-");
        return text(" " + hoverTip_) | dim;
    }();
    clipTo_ = nullptr;
    return line | reflect(messageBox_);
}

// An alert dialog: heading, body, and responses with Cancel first, the
// destructive response in red, and the default response highlighted.
Element app::renderDialog() {
    clipTo_ = &dialogBox_;
    auto respond = [this](char c) { return [this, c](const Mouse&) { answerDialog(c); }; };
    Element cancel = clickable(surfaceButton(text("  Cancel  ")), respond('c'), {}, {},
                               "Keep editing · Esc");
    Element discard = clickable(destructiveStyle(text("  Discard  ")), respond('d'), {}, {},
                                "Close without saving · D");
    Element saveButton = clickable(suggestedStyle(text("  Save  ")), respond('s'), {}, {},
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
    box = surface(box | borderRounded) | clear_under | reflect(dialogBox_);
    return centered(box);
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
        {"Keyboard", "Key Bindings", titleCase(toString(settings_.keys)) + " ▾",
         [this, cycle](int dir) {
             setKeyScheme(cycle(settings_.keys,
                                {keyScheme::emacs, keyScheme::cua, keyScheme::vim, keyScheme::nano},
                                dir));
         }},
        {"Playback", "Fade", "−  " + spacedMs(settings_.fade) + "  +",
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
    const auto rows = preferenceRows();
    overlayFocus_ = std::min(overlayFocus_, rows.size() - 1);
    Elements content;
    std::string group;
    Elements groupRows;
    auto flush = [&] {
        if (groupRows.empty()) return;
        content.push_back(text(" " + group) | bold);
        content.push_back(vbox(std::move(groupRows)) | borderRounded);
        groupRows.clear();
    };
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const preferenceRow& r = rows[i];
        if (r.group != group) {
            flush();
            group = r.group;
        }
        if (!groupRows.empty()) groupRows.push_back(separatorLight());
        auto change = r.change;
        Element value = text(r.value);
        const bool focused = i == overlayFocus_;
        if (!focused && r.value.find("On") != std::string::npos) value = value | color(Color::BlueLight);
        if (!focused && r.value.find("Off") != std::string::npos) value = surfaceSecondary(value);
        Element row = hbox({text(" " + r.title), filler(), value, text(" ")});
        if (focused) row = surfaceSelected(row);
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
            rows.push_back(hbox({text("   " + l.left), filler(), surfaceButton(text(" " + l.right + " ")),
                                 text(" ")}));
        }
    }
    auto wheel = [this](int dir) {
        if (dir > 0) ++overlayScroll_;
        if (dir < 0 && overlayScroll_ > 0) --overlayScroll_;
    };
    const std::string scheme = titleCase(toString(keys_.scheme())) + " key bindings";
    return clickable(vbox({surfaceSecondary(text(" " + scheme)), text(""), vbox(std::move(rows))}), {},
                     wheel);
}

Element app::renderAbout() {
    Element close = clickable(suggestedStyle(text("  Close  ")),
                              [this](const Mouse&) { overlay_ = overlayKind::none; });
    return vbox({
        text(""),
        text("proj") | bold | hcenter,
        surfaceSecondary(text(options_.link.clientVersion)) | hcenter,
        text(""),
        paragraphAlignCenter("A workstation for acousmatic, aleatory, and automatic music"),
        text(""),
        surfaceSecondary(paragraphAlignCenter("GNU Affero General Public License, version 3")),
        text(""),
        hbox({filler(), close, filler()}),
    });
}

Element app::renderOverlay() {
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
                        separatorLight(), content}) |
                  size(WIDTH, EQUAL, width);
    box = surface(box | borderRounded) | clear_under | reflect(overlayBox_);
    return centered(box);
}

Element app::render() {
    collectReplies();
    targets_.clear();

    Elements mainColumn{renderTabBar(), separator(), renderEditorPane()};
    if (settings_.console) mainColumn.push_back(renderConsole());

    Elements body;
    if (settings_.fileList && !narrow()) {
        body.push_back(renderSidebar());
        auto drag = [this](const Mouse& m) {
            settings_.fileListWidth = std::clamp(m.x - fileListBox_.x_min, 8, 200);
            persistOnRelease_ = true;
        };
        body.push_back(clickable(separator(), [](const Mouse&) {}, {}, drag,
                                 "Drag to resize the file list"));
    }
    body.push_back(vbox(std::move(mainColumn)) | flex);

    Elements layers{vbox({
                         renderHeaderBar(),
                         separator(),
                         hbox(std::move(body)) | flex,
                         separator(),
                         renderStatusBar(),
                         renderMessageLine(),
                     }) |
                     reflect(rootBox_)};
    // A modal dims the window behind it.
    if (dialog_ || overlay_ != overlayKind::none) layers[0] = layers[0] | dim;
    if (overlay_ != overlayKind::none) layers.push_back(renderOverlay());
    if (dialog_) layers.push_back(renderDialog());
    if (menu_) layers.push_back(renderMenu());
    return dbox(std::move(layers));
}

} // namespace tui
