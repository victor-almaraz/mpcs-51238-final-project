/// @file
/// Key binding schemes as data, and the state machine that resolves key
/// sequences to command names (`tui.md` §8.1–8.5). Has no terminal
/// dependency.
///
/// Keys use Emacs notation: `C-x` (Control), `M-f` (Meta), `S-<left>`
/// (Shift), `C-M-_`, named keys such as `RET`, `TAB`, `DEL`
/// (backspace), `ESC`, `SPC`, `<delete>`, `<up>`, `<home>`, `<prior>`
/// (Page Up), `<next>` (Page Down), `<f5>`, and printable characters as
/// themselves. A sequence is keys separated by spaces: `C-x C-s`.

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <proj/tui/settings.h>

namespace tui {

/// One binding: a key sequence and the command it invokes.
struct keyBinding {
    /// Key sequence, such as "C-x C-s".
    std::string_view keys;
    /// Command it invokes.
    std::string_view command;
};

/// A vim `:` command and the editor command it invokes.
struct exCommand {
    /// The `:` command, such as `w`.
    std::string_view name;
    /// Editor command it invokes.
    std::string_view command;
};

/// Editor modes. Only the vim scheme leaves `insert`.
enum class editMode { insert, normal, visual };

/// Name of @p mode, such as "insert".
std::string_view toString(editMode mode) noexcept;

/// Names of every command an editor must provide.
std::span<const std::string_view> commandNames() noexcept;

/// True for the commands that move the cursor, which keep a Shift
/// selection going.
bool isMovementCommand(std::string_view command) noexcept;

/// Bindings shared by every scheme: arrows, Home, End, Page Up, Page
/// Down, Shift-selection, and Delete (`tui.md` §8.3).
std::span<const keyBinding> commonBindings() noexcept;

/// Bindings of @p scheme in @p mode, excluding commonBindings().
std::span<const keyBinding> schemeBindings(keyScheme scheme, editMode mode) noexcept;

/// The vim scheme's `:` commands.
std::span<const exCommand> exCommands() noexcept;

/// A resolved vim `:` command line.
struct exInvocation {
    std::string_view command;   ///< Editor command, or empty when unknown
    std::string_view argument;  ///< Text after the command name, trimmed
};

/// Resolves the vim `:` command @p line, such as `w` or `e main.lisp`.
exInvocation resolveExCommand(std::string_view line) noexcept;

/// True when @p key is a single printable character (not a named key
/// or a modified key).
bool isPrintableKey(std::string_view key) noexcept;

/// Resolves keys to commands for one scheme, tracking prefix keys and
/// the vim mode.
class keySequencer {
public:
    /// Outcome of one key.
    enum class outcome {
        command,     ///< A binding matched; see result::command
        pending,     ///< The keys so far are a prefix of a binding
        selfInsert,  ///< Unbound printable key in a text-inserting mode
        unbound,     ///< No binding; the sequence is discarded
    };

    /// Outcome of one key, with the command or the keys typed.
    struct result {
        /// What happened.
        outcome kind = outcome::unbound;
        std::string command;  ///< Command name for outcome::command
        std::string keys;     ///< The full sequence typed, such as "C-x C-s"
    };

    /// Creates a sequencer for @p scheme, in its initial mode.
    explicit keySequencer(keyScheme scheme = keyScheme::emacs);

    /// The key scheme.
    keyScheme scheme() const noexcept { return scheme_; }
    /// The current mode.
    editMode mode() const noexcept { return mode_; }
    /// Changes the mode; schemes other than vim stay in insert mode.
    void setMode(editMode mode) noexcept;

    /// True when unbound printable keys insert text in the current mode.
    bool insertsText() const noexcept { return mode_ == editMode::insert; }

    /// Keys typed so far in an incomplete sequence, such as "C-x".
    std::string pendingKeys() const;

    /// Processes one key.
    result feed(std::string_view key);

    /// Discards a pending sequence.
    void reset() noexcept { pending_.clear(); }

private:
    keyScheme scheme_ = keyScheme::emacs;
    editMode mode_ = editMode::insert;
    std::vector<std::string> pending_;
};

} // namespace tui
