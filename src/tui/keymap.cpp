#include <algorithm>
#include <array>

#include <proj/tui/keymap.h>
#include <proj/tui/utf8.h>

namespace tui {

namespace {

// Movement commands: they keep a Shift selection going.
constexpr std::string_view movementCommands[] = {
    "forwardChar", "backwardChar",  "nextLine", "previousLine", "forwardWord", "backwardWord", "lineStart",
    "lineEnd",     "firstNonBlank", "pageDown", "pageUp",       "bufferStart", "bufferEnd",
};

constexpr std::string_view nonMovementCommands[] = {
    // Files and buffers
    "openFile",
    "save",
    "saveAs",
    "closeBuffer",
    "switchBuffer",
    "nextBuffer",
    "previousBuffer",
    "quit",
    "saveAndQuit",
    "quitWithoutSaving",
    "cancel",
    // Editing
    "undo",
    "redo",
    "setMark",
    "selectAll",
    "cut",
    "copy",
    "paste",
    "killLine",
    "deleteLine",
    "yankLine",
    "deleteBackward",
    "deleteForward",
    "newline",
    "indent",
    "joinLines",
    // Search
    "searchForward",
    "searchBackward",
    "searchNext",
    "searchPrevious",
    // Run controls
    "evalLastForm",
    "play",
    "stop",
    // Panes and dialogs
    "toggleFileList",
    "toggleConsole",
    "clearConsole",
    "mainMenu",
    "contextMenu",
    "showPreferences",
    "showShortcuts",
    "showAbout",
    // Vim modes
    "insertMode",
    "appendMode",
    "appendLineEnd",
    "insertLineStart",
    "openLineBelow",
    "openLineAbove",
    "normalMode",
    "visualMode",
    "commandLine",
};

// Every command: the movement commands, then the rest.
constexpr auto commands = [] {
    std::array<std::string_view, std::size(movementCommands) + std::size(nonMovementCommands)> all{};
    const auto rest = std::copy(std::begin(movementCommands), std::end(movementCommands), all.begin());
    std::copy(std::begin(nonMovementCommands), std::end(nonMovementCommands), rest);
    return all;
}();

constexpr keyBinding common[] = {
    {"<left>", "backwardChar"},    {"<right>", "forwardChar"},
    {"<up>", "previousLine"},      {"<down>", "nextLine"},
    {"<home>", "lineStart"},       {"<end>", "lineEnd"},
    {"<prior>", "pageUp"},         {"<next>", "pageDown"},
    {"C-<home>", "bufferStart"},   {"C-<end>", "bufferEnd"},
    {"S-<left>", "backwardChar"},  {"S-<right>", "forwardChar"},
    {"S-<up>", "previousLine"},    {"S-<down>", "nextLine"},
    {"S-<home>", "lineStart"},     {"S-<end>", "lineEnd"},
    {"S-<prior>", "pageUp"},       {"S-<next>", "pageDown"},
    {"C-S-<home>", "bufferStart"}, {"C-S-<end>", "bufferEnd"},
    {"C-<left>", "backwardWord"},  {"C-<right>", "forwardWord"},
    {"C-S-<left>", "backwardWord"}, {"C-S-<right>", "forwardWord"},
    {"<delete>", "deleteForward"},
    {"<f5>", "play"},              {"<f6>", "stop"},
    {"<f7>", "toggleFileList"},    {"<f8>", "evalLastForm"},
    {"<f9>", "toggleConsole"},
    {"<f1>", "showShortcuts"},     {"<f10>", "mainMenu"},
    {"S-<f10>", "contextMenu"},
    {"M-<right>", "nextBuffer"},   {"M-<left>", "previousBuffer"},
    {"<f12>", "saveAs"},
};

constexpr keyBinding emacs[] = {
    {"DEL", "deleteBackward"},
    {"RET", "newline"},
    {"TAB", "indent"},
    {"C-x C-f", "openFile"},
    {"C-x C-s", "save"},
    {"C-x C-w", "saveAs"},
    {"C-x k", "closeBuffer"},
    {"C-x b", "switchBuffer"},
    {"C-x C-c", "quit"},
    {"C-x <right>", "nextBuffer"},
    {"C-x <left>", "previousBuffer"},
    {"C-x h", "selectAll"},
    {"C-g", "cancel"},
    {"C-/", "undo"},
    {"C-_", "undo"},
    {"C-x u", "undo"},
    {"C-M-_", "redo"},
    {"C-?", "redo"},
    {"C-s", "searchForward"},
    {"C-r", "searchBackward"},
    {"C-SPC", "setMark"},
    {"C-@", "setMark"},
    {"C-w", "cut"},
    {"M-w", "copy"},
    {"C-y", "paste"},
    {"C-k", "killLine"},
    {"C-d", "deleteForward"},
    {"C-f", "forwardChar"},
    {"C-b", "backwardChar"},
    {"C-n", "nextLine"},
    {"C-p", "previousLine"},
    {"M-f", "forwardWord"},
    {"M-b", "backwardWord"},
    {"C-a", "lineStart"},
    {"C-e", "lineEnd"},
    {"M-m", "firstNonBlank"},
    {"C-v", "pageDown"},
    {"M-v", "pageUp"},
    {"M-<", "bufferStart"},
    {"M->", "bufferEnd"},
    {"C-x C-e", "evalLastForm"},
    {"C-c C-c", "play"},
    {"C-c C-s", "stop"},
    {"C-c f", "toggleFileList"},
    {"C-c o", "toggleConsole"},
    {"C-c ,", "showPreferences"},
};

constexpr keyBinding cua[] = {
    {"DEL", "deleteBackward"},
    {"RET", "newline"},
    {"TAB", "indent"},
    {"C-s", "save"},
    {"C-o", "openFile"},
    {"C-w", "closeBuffer"},
    {"C-p", "switchBuffer"},
    {"C-q", "quit"},
    {"ESC", "cancel"},
    {"C-z", "undo"},
    {"C-y", "redo"},
    {"C-f", "searchForward"},
    {"C-r", "searchBackward"},
    {"<f3>", "searchNext"},
    {"S-<f3>", "searchPrevious"},
    {"C-x", "cut"},
    {"C-c", "copy"},
    {"C-v", "paste"},
    {"C-a", "selectAll"},
    {"C-<next>", "nextBuffer"},
    {"C-<prior>", "previousBuffer"},
    {"C-e", "evalLastForm"},
};

constexpr keyBinding nano[] = {
    {"DEL", "deleteBackward"}, {"RET", "newline"},        {"TAB", "indent"},        {"C-o", "save"},
    {"M-o", "saveAs"},         {"C-r", "openFile"},       {"M-k", "closeBuffer"},   {"M-b", "switchBuffer"},
    {"C-x", "quit"},           {"C-c", "cancel"},         {"M-.", "nextBuffer"},    {"M-,", "previousBuffer"},
    {"M-u", "undo"},           {"M-e", "redo"},           {"C-w", "searchForward"}, {"C-q", "searchBackward"},
    {"M-w", "searchNext"},     {"M-q", "searchPrevious"}, {"M-a", "setMark"},       {"C-k", "cut"},
    {"M-6", "copy"},           {"C-u", "paste"},          {"C-d", "deleteForward"}, {"C-f", "forwardChar"},
    {"C-b", "backwardChar"},   {"C-n", "nextLine"},       {"C-p", "previousLine"},  {"C-SPC", "forwardWord"},
    {"M-SPC", "backwardWord"}, {"C-a", "lineStart"},      {"C-e", "lineEnd"},       {"C-v", "pageDown"},
    {"C-y", "pageUp"},         {"M-\\", "bufferStart"},   {"M-/", "bufferEnd"},     {"M-r", "evalLastForm"},
};

constexpr keyBinding vimNormal[] = {
    {"h", "backwardChar"},     {"l", "forwardChar"},      {"j", "nextLine"},
    {"k", "previousLine"},     {"DEL", "backwardChar"},   {"SPC", "forwardChar"},
    {"RET", "nextLine"},
    {"w", "forwardWord"},      {"b", "backwardWord"},
    {"0", "lineStart"},        {"$", "lineEnd"},          {"^", "firstNonBlank"},
    {"g g", "bufferStart"},    {"G", "bufferEnd"},
    {"C-f", "pageDown"},       {"C-b", "pageUp"},
    {"x", "deleteForward"},    {"X", "deleteBackward"},   {"J", "joinLines"},
    {"d d", "deleteLine"},     {"D", "killLine"},
    {"y y", "yankLine"},       {"p", "paste"},
    {"u", "undo"},             {"C-r", "redo"},
    {"i", "insertMode"},       {"a", "appendMode"},       {"A", "appendLineEnd"},
    {"I", "insertLineStart"},  {"o", "openLineBelow"},    {"O", "openLineAbove"},
    {"v", "visualMode"},       {":", "commandLine"},
    {"/", "searchForward"},    {"?", "searchBackward"},
    {"n", "searchNext"},       {"N", "searchPrevious"},
    {"ESC", "cancel"},         {"g t", "nextBuffer"},     {"g T", "previousBuffer"},
    {"C-g", "cancel"},         {"C-x C-e", "evalLastForm"},
};

constexpr keyBinding vimInsert[] = {
    {"DEL", "deleteBackward"},
    {"RET", "newline"},
    {"TAB", "indent"},
    {"ESC", "normalMode"},
};

constexpr keyBinding vimVisual[] = {
    {"h", "backwardChar"},     {"l", "forwardChar"},      {"j", "nextLine"},
    {"k", "previousLine"},     {"w", "forwardWord"},      {"b", "backwardWord"},
    {"0", "lineStart"},        {"$", "lineEnd"},          {"^", "firstNonBlank"},
    {"g g", "bufferStart"},    {"G", "bufferEnd"},
    {"C-f", "pageDown"},       {"C-b", "pageUp"},
    {"d", "cut"},              {"x", "cut"},              {"y", "copy"},
    {"p", "paste"},            {"ESC", "normalMode"},     {"v", "normalMode"},
    {":", "commandLine"},      {"J", "joinLines"},        {"C-x C-e", "evalLastForm"},
};

constexpr exCommand ex[] = {
    {"w", "save"},              {"write", "save"},          {"saveas", "saveAs"},
    {"q", "quit"},              {"quit", "quit"},           {"q!", "quitWithoutSaving"},
    {"wq", "saveAndQuit"},      {"x", "saveAndQuit"},
    {"e", "openFile"},          {"edit", "openFile"},
    {"bd", "closeBuffer"},      {"b", "switchBuffer"},      {"buffer", "switchBuffer"},
    {"bn", "nextBuffer"},       {"bp", "previousBuffer"},
    {"u", "undo"},              {"redo", "redo"},
    {"play", "play"},           {"stop", "stop"},           {"eval", "evalLastForm"},
    {"filelist", "toggleFileList"}, {"console", "toggleConsole"},
    {"set", "showPreferences"},     {"help", "showShortcuts"},
};


std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

} // namespace

std::string_view toString(editMode mode) noexcept {
    switch (mode) {
    case editMode::insert: return "insert";
    case editMode::normal: return "normal";
    case editMode::visual: return "visual";
    }
    return "insert";
}

std::span<const std::string_view> commandNames() noexcept { return commands; }

std::span<const keyBinding> commonBindings() noexcept { return common; }

std::span<const keyBinding> schemeBindings(keyScheme scheme, editMode mode) noexcept {
    switch (scheme) {
    case keyScheme::emacs: return emacs;
    case keyScheme::cua: return cua;
    case keyScheme::nano: return nano;
    case keyScheme::vim:
        switch (mode) {
        case editMode::insert: return vimInsert;
        case editMode::normal: return vimNormal;
        case editMode::visual: return vimVisual;
        }
    }
    return emacs;
}

bool isMovementCommand(std::string_view command) noexcept {
    return std::find(std::begin(movementCommands), std::end(movementCommands), command) !=
           std::end(movementCommands);
}

std::span<const exCommand> exCommands() noexcept { return ex; }

exInvocation resolveExCommand(std::string_view line) noexcept {
    line = trim(line);
    const std::size_t space = line.find(' ');
    const std::string_view name = line.substr(0, space);
    const std::string_view argument =
        space == std::string_view::npos ? std::string_view{} : trim(line.substr(space));
    for (const auto& c : ex) {
        if (c.name == name) return {c.command, argument};
    }
    return {{}, argument};
}

bool isPrintableKey(std::string_view key) noexcept {
    if (key.empty()) return false;
    const auto lead = static_cast<unsigned char>(key[0]);
    if (utf8::sequenceLength(lead) != key.size()) return false;
    return lead >= 0x20u && lead != 0x7Fu;
}

keySequencer::keySequencer(keyScheme scheme)
    : scheme_(scheme), mode_(scheme == keyScheme::vim ? editMode::normal : editMode::insert) {}

void keySequencer::setMode(editMode mode) noexcept {
    mode_ = scheme_ == keyScheme::vim ? mode : editMode::insert;
    pending_.clear();
}

std::string keySequencer::pendingKeys() const {
    std::string out;
    for (const auto& k : pending_) {
        if (!out.empty()) out += ' ';
        out += k;
    }
    return out;
}

keySequencer::result keySequencer::feed(std::string_view key) {
    pending_.emplace_back(key);
    const std::string typed = pendingKeys();
    const std::string prefix = typed + ' ';

    bool isPrefix = false;
    for (const auto table : {schemeBindings(scheme_, mode_), std::span<const keyBinding>(common)}) {
        for (const auto& b : table) {
            if (b.keys == typed) {
                pending_.clear();
                return {outcome::command, std::string(b.command), typed};
            }
            if (b.keys.starts_with(prefix)) isPrefix = true;
        }
    }
    if (isPrefix) return {outcome::pending, {}, typed};

    pending_.clear();
    if (typed == key && insertsText() && isPrintableKey(key)) {
        return {outcome::selfInsert, {}, typed};
    }
    return {outcome::unbound, {}, typed};
}

} // namespace tui
