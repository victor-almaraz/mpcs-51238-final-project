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
                                                  : options_.settingsFile) {
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
    console_.info("project " + root_.string() + " · keys " + std::string(toString(settings_.keys)));
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
    ask("Save changes to " + displayName(b) + "?", [this, close](char choice) {
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
        ask("Save changes to " + displayName(*buffers_[i]) + " before quitting?",
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
        bool failed = false;
        for (auto& e : reply.entries) {
            failed = failed || e.kind == consoleKind::error;
            console_.add(e.kind, std::move(e.text), std::move(e.location));
        }
        consoleScroll_ = 0;
        message_ = failed ? "Errors; see the console" : "";
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

void app::ask(std::string question, std::function<void(char)> answer) {
    dialog_ = dialog{std::move(question), std::move(answer)};
    menu_.reset();
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
    if (key == "s" || key == "y" || key == "S" || key == "Y") choice = 's';
    if (key == "d" || key == "n" || key == "D" || key == "N") choice = 'd';
    if (key == "c" || key == "C" || key == "ESC" || key == "C-g") choice = 'c';
    if (choice == 0) return true;  // the dialog keeps the keyboard
    auto answer = std::move(dialog_->answer);
    dialog_.reset();
    answer(choice);
    return true;
}

// ---------------------------------------------------------------------
// Input

bool app::handleKey(std::string_view key) {
    collectReplies();
    if (dialog_) return handleDialogKey(key);
    if (menu_) {
        menu_.reset();
        if (key == "ESC" || key == "C-g") return true;
    }
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
        if (!dragAction_) return false;
        dragAction_(mouse);
        return true;
    }
    if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return false;

    if (menu_ && !menuBox_.Contain(x, y)) {
        menu_.reset();
        return true;
    }
    for (auto it = targets_.rbegin(); it != targets_.rend(); ++it) {
        if (dialog_ && it->within != &messageBox_) continue;  // the dialog is modal
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

Element app::clickable(Element e, std::function<void(const Mouse&)> press,
                       std::function<void(int)> wheel, std::function<void(const Mouse&)> drag) {
    targets_.push_back({{}, std::move(press), std::move(drag), std::move(wheel), clipTo_});
    return e | reflect(targets_.back().box);
}

std::string app::keyHint(std::string_view command) const {
    const editMode mode = keys_.scheme() == keyScheme::vim ? editMode::normal : editMode::insert;
    for (const auto& b : schemeBindings(keys_.scheme(), mode)) {
        if (b.command == command) return std::string(b.keys);
    }
    if (keys_.scheme() == keyScheme::vim) {
        for (const auto& e : exCommands()) {
            if (e.command == command) return ":" + std::string(e.name);
        }
    }
    for (const auto& b : commonBindings()) {
        if (b.command == command) return std::string(b.keys);
    }
    return {};
}

app::menuEntry app::item(std::string label, const char* command) {
    return {std::move(label), [this, command] { runCommand(command); }, keyHint(command)};
}

std::vector<app::menuEntry> app::fileMenu() {
    return {item("Open...", "openFile"),          item("Save", "save"),
            item("Save As...", "saveAs"),         item("Switch Buffer...", "switchBuffer"),
            item("Next Buffer", "nextBuffer"),    item("Previous Buffer", "previousBuffer"),
            item("Close Buffer", "closeBuffer"),  item("Quit", "quit")};
}

std::vector<app::menuEntry> app::editMenu() {
    return {item("Undo", "undo"),
            item("Redo", "redo"),
            item("Cut", "cut"),
            item("Copy", "copy"),
            item("Paste", "paste"),
            item("Select All", "selectAll"),
            item("Set Mark", "setMark"),
            item("Kill Line", "killLine"),
            item("Search...", "searchForward"),
            item("Search Backward...", "searchBackward"),
            item("Find Next", "searchNext"),
            item("Find Previous", "searchPrevious")};
}

std::vector<app::menuEntry> app::runMenu() {
    return {item("Play", "play"), item("Stop", "stop"), item("Eval Last Form", "evalLastForm")};
}

std::vector<app::menuEntry> app::viewMenu() {
    std::vector<menuEntry> entries{
        item(std::string(settings_.fileList ? "Hide" : "Show") + " File List", "toggleFileList"),
        item(std::string(settings_.console ? "Hide" : "Show") + " Console", "toggleConsole"),
        {std::string(settings_.rebuildOnSave ? "[x]" : "[ ]") + " Rebuild on Save",
         [this] {
             settings_.rebuildOnSave = !settings_.rebuildOnSave;
             persistSettings();
         },
         {}},
    };
    for (auto scheme : {keyScheme::emacs, keyScheme::cua, keyScheme::vim, keyScheme::nano}) {
        const std::string mark = settings_.keys == scheme ? "(o) " : "( ) ";
        entries.push_back({mark + "Keys: " + std::string(toString(scheme)),
                           [this, scheme] { setKeyScheme(scheme); },
                           {}});
    }
    return entries;
}

void app::openMenu(std::vector<menuEntry> entries, int x, int y) {
    menu_ = std::move(entries);
    menuX_ = x;
    menuY_ = y;
}

Element app::renderMenu() {
    Elements rows;
    for (const auto& entry : *menu_) {
        auto action = entry.action;
        Element hint = text(entry.hint.empty() ? " " : "  " + entry.hint + " ") | dim;
        rows.push_back(clickable(hbox({text(" " + entry.label + " "), filler(), hint}),
                                 [this, action](const Mouse&) {
                                     menu_.reset();
                                     action();
                                     followCursor_ = true;
                                 }));
    }
    Element popup = vbox(std::move(rows)) | border | clear_under | reflect(menuBox_);
    return vbox({
        text("") | size(HEIGHT, EQUAL, std::max(menuY_, 0)),
        hbox({text("") | size(WIDTH, EQUAL, std::max(menuX_, 0)), popup, filler()}),
        filler(),
    });
}

Element app::renderToolbar() {
    auto button = [this](std::string label, std::function<void(const Mouse&)> press) {
        return clickable(text(" " + label + " ") | inverted, std::move(press));
    };
    auto menuButton = [this](std::string label, std::function<std::vector<menuEntry>()> build) {
        return clickable(text(" " + label + " "), [this, build](const Mouse& m) {
            openMenu(build(), m.x, m.y + 1);
        });
    };
    Element fade = clickable(
        text(" " + formatMs(settings_.fade) + " ") | underlined,
        [this](const Mouse&) {
            openPrompt(promptKind::fade, "Fade (ms): ", formatMs(settings_.fade));
        },
        [this](int dir) { setFade(settings_.fade - dir * 0.01); });
    Element quant = clickable(text(" " + std::string(toString(settings_.quant)) + " ▾ ") | underlined,
                              [this](const Mouse& m) {
                                  std::vector<menuEntry> entries;
                                  for (auto q : {quantization::none, quantization::beat,
                                                 quantization::bar}) {
                                      entries.push_back({std::string(toString(q)),
                                                         [this, q] {
                                                             settings_.quant = q;
                                                             persistSettings();
                                                         },
                                                         {}});
                                  }
                                  openMenu(std::move(entries), m.x, m.y + 1);
                              });
    return hbox({
        menuButton("File", [this] { return fileMenu(); }),
        menuButton("Edit", [this] { return editMenu(); }),
        menuButton("Run", [this] { return runMenu(); }),
        menuButton("View", [this] { return viewMenu(); }),
        separatorLight(),
        text(" "),
        button("▶ Play", [this](const Mouse&) { runCommand("play"); }) | color(Color::Green),
        text(" "),
        button("■ Stop", [this](const Mouse&) { runCommand("stop"); }) | color(Color::Red),
        text("  Fade"),
        fade,
        text(" Quant"),
        quant,
        filler(),
    });
}

Element app::renderFileList() {
    clipTo_ = &fileListBox_;
    Elements rows;
    rows.push_back(text(" " + root_.filename().string() + "/") | bold);
    const fs::path currentPath = current().path();
    const std::size_t first = std::min(fileListScroll_, projectFiles_.size());
    for (std::size_t i = first; i < projectFiles_.size(); ++i) {
        const fs::path& f = projectFiles_[i];
        Element row = text("  " + f.string());
        if (resolve(f) == currentPath) row = row | bold | color(Color::Cyan);
        rows.push_back(clickable(row, [this, f](const Mouse&) { openFile(f); },
                                 [this](int dir) {
                                     if (dir < 0 && fileListScroll_ > 0) --fileListScroll_;
                                     if (dir > 0 && fileListScroll_ + 1 < projectFiles_.size()) {
                                         ++fileListScroll_;
                                     }
                                 }));
    }
    clipTo_ = nullptr;
    return vbox(std::move(rows)) | yframe | size(WIDTH, EQUAL, settings_.fileListWidth) |
           reflect(fileListBox_);
}

Element app::renderTabBar() {
    clipTo_ = &tabBarBox_;
    Elements tabs;
    for (std::size_t i = 0; i < buffers_.size(); ++i) {
        const buffer& b = *buffers_[i];
        Element name = text(" " + displayName(b) + (b.dirty() ? " ●" : "") + " ");
        if (i == current_) name = name | bold | inverted;
        tabs.push_back(clickable(name, [this, i](const Mouse&) { selectBuffer(i); }));
        tabs.push_back(clickable(text("×") | dim, [this, i](const Mouse&) { closeBuffer(i); }));
        tabs.push_back(text(" "));
    }
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
    const bool focused = !minibuffer_.active() && !dialog_;
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
         nullptr});
    // yframe clips rows beyond the pane after a resize.
    return view | yframe | reflect(targets_.back().box) | reflect(editorBox_) | flex;
}

Element app::renderConsole() {
    clipTo_ = &consoleBox_;
    const auto& entries = console_.entries();
    const int height = std::max(settings_.consoleHeight, 1);
    const std::size_t visible = static_cast<std::size_t>(height);
    const std::size_t end = entries.size() - std::min(consoleScroll_, entries.size());
    const std::size_t begin = end > visible ? end - visible : 0;
    Elements rows;
    for (std::size_t i = begin; i < end; ++i) {
        const consoleEntry& e = entries[i];
        Element row = text(e.text);
        switch (e.kind) {
        case consoleKind::error: row = row | color(Color::Red); break;
        case consoleKind::server: row = row | color(Color::Blue); break;
        case consoleKind::info: break;
        }
        auto wheel = [this](int dir) {
            if (dir < 0 && consoleScroll_ + 1 < console_.size()) ++consoleScroll_;
            if (dir > 0 && consoleScroll_ > 0) --consoleScroll_;
        };
        if (e.location) {
            const sourceLocation loc = *e.location;
            row = clickable(row | underlined, [this, loc](const Mouse&) { jumpTo(loc); }, wheel);
        } else {
            row = clickable(row, {}, wheel);
        }
        rows.push_back(row);
    }
    clipTo_ = nullptr;
    return vbox(std::move(rows)) | yframe | size(HEIGHT, EQUAL, height) | reflect(consoleBox_);
}

Element app::renderStatusBar() {
    const serverStatus& s = status_;
    std::ostringstream left;
    left << (s.connected ? " ● connected" : " ○ not connected") << " · " << s.bpm << " BPM · beat ";
    if (s.beat) {
        left << *s.beat;
    } else {
        left << "—";
    }
    left << " · missed " << s.missed << " · xrun " << s.xruns << " · drops " << s.drops;

    const buffer& b = *buffers_[current_];
    std::ostringstream right;
    if (keys_.scheme() == keyScheme::vim) {
        std::string mode(toString(keys_.mode()));
        for (auto& ch : mode) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        right << "-- " << mode << " -- ";
    }
    right << toString(keys_.scheme()) << "  Ln " << b.cursor().line + 1 << ", Col "
          << b.cursor().column + 1 << ' ';
    return hbox({text(left.str()), filler(), text(right.str())}) | inverted;
}

Element app::renderMessageLine() {
    clipTo_ = &messageBox_;
    Element line = [this]() -> Element {
    if (dialog_) {
        auto choice = [this](std::string label, char c) {
            return clickable(text(" " + label + " ") | inverted, [this, c](const Mouse&) {
                auto answer = std::move(dialog_->answer);
                dialog_.reset();
                answer(c);
            });
        };
        return hbox({text(dialog_->question + " ") | bold, choice("Save", 's'), text(" "),
                     choice("Discard", 'd'), text(" "), choice("Cancel", 'c'), filler()});
    }
    if (minibuffer_.active()) {
        const std::string& t = minibuffer_.text();
        const std::size_t at = utf8::byteOffset(t, minibuffer_.cursor());
        const std::string after = at < t.size() ? t.substr(at + utf8::sequenceLength(
                                                               static_cast<unsigned char>(t[at])))
                                                : "";
        const std::string under =
            at < t.size() ? t.substr(at, utf8::sequenceLength(static_cast<unsigned char>(t[at])))
                          : " ";
        Elements parts{text(minibuffer_.prompt()) | bold, text(t.substr(0, at)),
                       text(under) | inverted, text(after)};
        const auto matches = minibuffer_.matches();
        if (!matches.empty() && promptKind_ != promptKind::search) {
            parts.push_back(text("  "));
            std::size_t shown = 0;
            for (const auto& m : matches) {
                if (++shown > 12) {
                    parts.push_back(text(" ...") | dim);
                    break;
                }
                parts.push_back(clickable(text(m) | dim, [this, m](const Mouse&) {
                    minibuffer_.setText(m);
                    submitPrompt();
                }));
                parts.push_back(text(" "));
            }
        }
        parts.push_back(filler());
        return hbox(std::move(parts));
    }
    const std::string pending = keys_.pendingKeys();
    return text(!message_.empty() ? message_ : pending.empty() ? "" : pending + "-");
    }();
    clipTo_ = nullptr;
    return line | reflect(messageBox_);
}

Element app::render() {
    collectReplies();
    targets_.clear();

    Elements editorColumn{renderTabBar(), separatorLight(), renderEditorPane()};
    if (settings_.console) {
        auto drag = [this](const Mouse& m) {
            settings_.consoleHeight = std::clamp(consoleBox_.y_max - m.y, 1, 200);
            persistOnRelease_ = true;
        };
        editorColumn.push_back(clickable(separator(), [](const Mouse&) {}, {}, drag));
        editorColumn.push_back(renderConsole());
    }

    Elements middle;
    if (settings_.fileList) {
        middle.push_back(renderFileList());
        auto drag = [this](const Mouse& m) {
            settings_.fileListWidth = std::clamp(m.x - fileListBox_.x_min, 8, 200);
            persistOnRelease_ = true;
        };
        middle.push_back(clickable(separator(), [](const Mouse&) {}, {}, drag));
    }
    middle.push_back(vbox(std::move(editorColumn)) | flex);

    Element main = vbox({
        renderToolbar(),
        separatorLight(),
        hbox(std::move(middle)) | flex,
        renderStatusBar(),
        renderMessageLine(),
    });
    if (menu_) return dbox({main, renderMenu()});
    return main;
}

} // namespace tui
