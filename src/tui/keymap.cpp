#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include <proj/tui/keymap.h>
#include <proj/tui/settings.h>
#include <proj/tui/utf8.h>

namespace tui {

namespace {

// Movement commands: they keep a Shift selection going.
constexpr auto movementCommands = std::to_array<std::string_view>({
    "forwardChar",
    "backwardChar",
    "nextLine",
    "previousLine",
    "forwardWord",
    "backwardWord",
    "lineStart",
    "lineEnd",
    "firstNonBlank",
    "pageDown",
    "pageUp",
    "bufferStart",
    "bufferEnd",
});

constexpr auto nonMovementCommands = std::to_array<std::string_view>({
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
});

// Every command: the movement commands, then the rest.
constexpr auto commands = [] {
    std::array<std::string_view, std::size(movementCommands) + std::size(nonMovementCommands)> all{};
    const auto rest = std::ranges::copy(movementCommands, all.begin()).out;
    std::ranges::copy(nonMovementCommands, rest);
    return all;
}();

constexpr auto common = std::to_array<keyBinding>({
    {.keys = "<left>", .command = "backwardChar"},
    {.keys = "<right>", .command = "forwardChar"},
    {.keys = "<up>", .command = "previousLine"},
    {.keys = "<down>", .command = "nextLine"},
    {.keys = "<home>", .command = "lineStart"},
    {.keys = "<end>", .command = "lineEnd"},
    {.keys = "<prior>", .command = "pageUp"},
    {.keys = "<next>", .command = "pageDown"},
    {.keys = "C-<home>", .command = "bufferStart"},
    {.keys = "C-<end>", .command = "bufferEnd"},
    {.keys = "S-<left>", .command = "backwardChar"},
    {.keys = "S-<right>", .command = "forwardChar"},
    {.keys = "S-<up>", .command = "previousLine"},
    {.keys = "S-<down>", .command = "nextLine"},
    {.keys = "S-<home>", .command = "lineStart"},
    {.keys = "S-<end>", .command = "lineEnd"},
    {.keys = "S-<prior>", .command = "pageUp"},
    {.keys = "S-<next>", .command = "pageDown"},
    {.keys = "C-S-<home>", .command = "bufferStart"},
    {.keys = "C-S-<end>", .command = "bufferEnd"},
    {.keys = "C-<left>", .command = "backwardWord"},
    {.keys = "C-<right>", .command = "forwardWord"},
    {.keys = "C-S-<left>", .command = "backwardWord"},
    {.keys = "C-S-<right>", .command = "forwardWord"},
    {.keys = "<delete>", .command = "deleteForward"},
    {.keys = "<f5>", .command = "play"},
    {.keys = "<f6>", .command = "stop"},
    {.keys = "<f7>", .command = "toggleFileList"},
    {.keys = "<f8>", .command = "evalLastForm"},
    {.keys = "<f9>", .command = "toggleConsole"},
    {.keys = "<f1>", .command = "showShortcuts"},
    {.keys = "<f10>", .command = "mainMenu"},
    {.keys = "S-<f10>", .command = "contextMenu"},
    {.keys = "M-<right>", .command = "nextBuffer"},
    {.keys = "M-<left>", .command = "previousBuffer"},
    {.keys = "<f12>", .command = "saveAs"},
});

constexpr auto emacs = std::to_array<keyBinding>({
    {.keys = "DEL", .command = "deleteBackward"},
    {.keys = "RET", .command = "newline"},
    {.keys = "TAB", .command = "indent"},
    {.keys = "C-x C-f", .command = "openFile"},
    {.keys = "C-x C-s", .command = "save"},
    {.keys = "C-x C-w", .command = "saveAs"},
    {.keys = "C-x k", .command = "closeBuffer"},
    {.keys = "C-x b", .command = "switchBuffer"},
    {.keys = "C-x C-c", .command = "quit"},
    {.keys = "C-x <right>", .command = "nextBuffer"},
    {.keys = "C-x <left>", .command = "previousBuffer"},
    {.keys = "C-x h", .command = "selectAll"},
    {.keys = "C-g", .command = "cancel"},
    {.keys = "C-/", .command = "undo"},
    {.keys = "C-_", .command = "undo"},
    {.keys = "C-x u", .command = "undo"},
    {.keys = "C-M-_", .command = "redo"},
    {.keys = "C-?", .command = "redo"},
    {.keys = "C-s", .command = "searchForward"},
    {.keys = "C-r", .command = "searchBackward"},
    {.keys = "C-SPC", .command = "setMark"},
    {.keys = "C-@", .command = "setMark"},
    {.keys = "C-w", .command = "cut"},
    {.keys = "M-w", .command = "copy"},
    {.keys = "C-y", .command = "paste"},
    {.keys = "C-k", .command = "killLine"},
    {.keys = "C-d", .command = "deleteForward"},
    {.keys = "C-f", .command = "forwardChar"},
    {.keys = "C-b", .command = "backwardChar"},
    {.keys = "C-n", .command = "nextLine"},
    {.keys = "C-p", .command = "previousLine"},
    {.keys = "M-f", .command = "forwardWord"},
    {.keys = "M-b", .command = "backwardWord"},
    {.keys = "C-a", .command = "lineStart"},
    {.keys = "C-e", .command = "lineEnd"},
    {.keys = "M-m", .command = "firstNonBlank"},
    {.keys = "C-v", .command = "pageDown"},
    {.keys = "M-v", .command = "pageUp"},
    {.keys = "M-<", .command = "bufferStart"},
    {.keys = "M->", .command = "bufferEnd"},
    {.keys = "C-x C-e", .command = "evalLastForm"},
    {.keys = "C-c C-c", .command = "play"},
    {.keys = "C-c C-s", .command = "stop"},
    {.keys = "C-c f", .command = "toggleFileList"},
    {.keys = "C-c o", .command = "toggleConsole"},
    {.keys = "C-c ,", .command = "showPreferences"},
});

constexpr auto cua = std::to_array<keyBinding>({
    {.keys = "DEL", .command = "deleteBackward"},
    {.keys = "RET", .command = "newline"},
    {.keys = "TAB", .command = "indent"},
    {.keys = "C-s", .command = "save"},
    {.keys = "C-o", .command = "openFile"},
    {.keys = "C-w", .command = "closeBuffer"},
    {.keys = "C-p", .command = "switchBuffer"},
    {.keys = "C-q", .command = "quit"},
    {.keys = "ESC", .command = "cancel"},
    {.keys = "C-z", .command = "undo"},
    {.keys = "C-y", .command = "redo"},
    {.keys = "C-f", .command = "searchForward"},
    {.keys = "C-r", .command = "searchBackward"},
    {.keys = "<f3>", .command = "searchNext"},
    {.keys = "S-<f3>", .command = "searchPrevious"},
    {.keys = "C-x", .command = "cut"},
    {.keys = "C-c", .command = "copy"},
    {.keys = "C-v", .command = "paste"},
    {.keys = "C-a", .command = "selectAll"},
    {.keys = "C-<next>", .command = "nextBuffer"},
    {.keys = "C-<prior>", .command = "previousBuffer"},
    {.keys = "C-e", .command = "evalLastForm"},
});

constexpr auto nano = std::to_array<keyBinding>({
    {.keys = "DEL", .command = "deleteBackward"}, {.keys = "RET", .command = "newline"},
    {.keys = "TAB", .command = "indent"},         {.keys = "C-o", .command = "save"},
    {.keys = "M-o", .command = "saveAs"},         {.keys = "C-r", .command = "openFile"},
    {.keys = "M-k", .command = "closeBuffer"},    {.keys = "M-b", .command = "switchBuffer"},
    {.keys = "C-x", .command = "quit"},           {.keys = "C-c", .command = "cancel"},
    {.keys = "M-.", .command = "nextBuffer"},     {.keys = "M-,", .command = "previousBuffer"},
    {.keys = "M-u", .command = "undo"},           {.keys = "M-e", .command = "redo"},
    {.keys = "C-w", .command = "searchForward"},  {.keys = "C-q", .command = "searchBackward"},
    {.keys = "M-w", .command = "searchNext"},     {.keys = "M-q", .command = "searchPrevious"},
    {.keys = "M-a", .command = "setMark"},        {.keys = "C-k", .command = "cut"},
    {.keys = "M-6", .command = "copy"},           {.keys = "C-u", .command = "paste"},
    {.keys = "C-d", .command = "deleteForward"},  {.keys = "C-f", .command = "forwardChar"},
    {.keys = "C-b", .command = "backwardChar"},   {.keys = "C-n", .command = "nextLine"},
    {.keys = "C-p", .command = "previousLine"},   {.keys = "C-SPC", .command = "forwardWord"},
    {.keys = "M-SPC", .command = "backwardWord"}, {.keys = "C-a", .command = "lineStart"},
    {.keys = "C-e", .command = "lineEnd"},        {.keys = "C-v", .command = "pageDown"},
    {.keys = "C-y", .command = "pageUp"},         {.keys = "M-\\", .command = "bufferStart"},
    {.keys = "M-/", .command = "bufferEnd"},      {.keys = "M-r", .command = "evalLastForm"},
});

constexpr auto vimNormal = std::to_array<keyBinding>({
    {.keys = "h", .command = "backwardChar"},    {.keys = "l", .command = "forwardChar"},
    {.keys = "j", .command = "nextLine"},        {.keys = "k", .command = "previousLine"},
    {.keys = "DEL", .command = "backwardChar"},  {.keys = "SPC", .command = "forwardChar"},
    {.keys = "RET", .command = "nextLine"},      {.keys = "w", .command = "forwardWord"},
    {.keys = "b", .command = "backwardWord"},    {.keys = "0", .command = "lineStart"},
    {.keys = "$", .command = "lineEnd"},         {.keys = "^", .command = "firstNonBlank"},
    {.keys = "g g", .command = "bufferStart"},   {.keys = "G", .command = "bufferEnd"},
    {.keys = "C-f", .command = "pageDown"},      {.keys = "C-b", .command = "pageUp"},
    {.keys = "x", .command = "deleteForward"},   {.keys = "X", .command = "deleteBackward"},
    {.keys = "J", .command = "joinLines"},       {.keys = "d d", .command = "deleteLine"},
    {.keys = "D", .command = "killLine"},        {.keys = "y y", .command = "yankLine"},
    {.keys = "p", .command = "paste"},           {.keys = "u", .command = "undo"},
    {.keys = "C-r", .command = "redo"},          {.keys = "i", .command = "insertMode"},
    {.keys = "a", .command = "appendMode"},      {.keys = "A", .command = "appendLineEnd"},
    {.keys = "I", .command = "insertLineStart"}, {.keys = "o", .command = "openLineBelow"},
    {.keys = "O", .command = "openLineAbove"},   {.keys = "v", .command = "visualMode"},
    {.keys = ":", .command = "commandLine"},     {.keys = "/", .command = "searchForward"},
    {.keys = "?", .command = "searchBackward"},  {.keys = "n", .command = "searchNext"},
    {.keys = "N", .command = "searchPrevious"},  {.keys = "ESC", .command = "cancel"},
    {.keys = "g t", .command = "nextBuffer"},    {.keys = "g T", .command = "previousBuffer"},
    {.keys = "C-g", .command = "cancel"},        {.keys = "C-x C-e", .command = "evalLastForm"},
});

constexpr auto vimInsert = std::to_array<keyBinding>({
    {.keys = "DEL", .command = "deleteBackward"},
    {.keys = "RET", .command = "newline"},
    {.keys = "TAB", .command = "indent"},
    {.keys = "ESC", .command = "normalMode"},
});

constexpr auto vimVisual = std::to_array<keyBinding>({
    {.keys = "h", .command = "backwardChar"},  {.keys = "l", .command = "forwardChar"},
    {.keys = "j", .command = "nextLine"},      {.keys = "k", .command = "previousLine"},
    {.keys = "w", .command = "forwardWord"},   {.keys = "b", .command = "backwardWord"},
    {.keys = "0", .command = "lineStart"},     {.keys = "$", .command = "lineEnd"},
    {.keys = "^", .command = "firstNonBlank"}, {.keys = "g g", .command = "bufferStart"},
    {.keys = "G", .command = "bufferEnd"},     {.keys = "C-f", .command = "pageDown"},
    {.keys = "C-b", .command = "pageUp"},      {.keys = "d", .command = "cut"},
    {.keys = "x", .command = "cut"},           {.keys = "y", .command = "copy"},
    {.keys = "p", .command = "paste"},         {.keys = "ESC", .command = "normalMode"},
    {.keys = "v", .command = "normalMode"},    {.keys = ":", .command = "commandLine"},
    {.keys = "J", .command = "joinLines"},     {.keys = "C-x C-e", .command = "evalLastForm"},
});

constexpr auto ex = std::to_array<exCommand>({
    {.name = "w", .command = "save"},
    {.name = "write", .command = "save"},
    {.name = "saveas", .command = "saveAs"},
    {.name = "q", .command = "quit"},
    {.name = "quit", .command = "quit"},
    {.name = "q!", .command = "quitWithoutSaving"},
    {.name = "wq", .command = "saveAndQuit"},
    {.name = "x", .command = "saveAndQuit"},
    {.name = "e", .command = "openFile"},
    {.name = "edit", .command = "openFile"},
    {.name = "bd", .command = "closeBuffer"},
    {.name = "b", .command = "switchBuffer"},
    {.name = "buffer", .command = "switchBuffer"},
    {.name = "bn", .command = "nextBuffer"},
    {.name = "bp", .command = "previousBuffer"},
    {.name = "u", .command = "undo"},
    {.name = "redo", .command = "redo"},
    {.name = "play", .command = "play"},
    {.name = "stop", .command = "stop"},
    {.name = "eval", .command = "evalLastForm"},
    {.name = "filelist", .command = "toggleFileList"},
    {.name = "console", .command = "toggleConsole"},
    {.name = "set", .command = "showPreferences"},
    {.name = "help", .command = "showShortcuts"},
});

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
    return std::ranges::find(movementCommands, command) != std::end(movementCommands);
}

std::span<const exCommand> exCommands() noexcept { return ex; }

exInvocation resolveExCommand(std::string_view line) noexcept {
    line = trim(line);
    const std::size_t space = line.find(' ');
    const std::string_view name = line.substr(0, space);
    const std::string_view argument =
        space == std::string_view::npos ? std::string_view{} : trim(line.substr(space));
    for (const auto& c : ex) {
        if (c.name == name) return {.command = c.command, .argument = argument};
    }
    return {.command = {}, .argument = argument};
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
                return {.kind = outcome::command, .command = std::string(b.command), .keys = typed};
            }
            if (b.keys.starts_with(prefix)) isPrefix = true;
        }
    }
    if (isPrefix) return {.kind = outcome::pending, .command = {}, .keys = typed};

    pending_.clear();
    if (typed == key && insertsText() && isPrintableKey(key)) {
        return {.kind = outcome::selfInsert, .command = {}, .keys = typed};
    }
    return {.kind = outcome::unbound, .command = {}, .keys = typed};
}

} // namespace tui
