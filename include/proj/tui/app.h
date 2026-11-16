/// @file
/// The editor application: owns buffers, routes commands, and runs the
/// FTXUI loop (`tui.md` §3, §4).

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <proj/tui/buffer.h>
#include <proj/tui/consoleLog.h>
#include <proj/tui/highlighter.h>
#include <proj/tui/interpreterLink.h>
#include <proj/tui/keymap.h>
#include <proj/tui/minibuffer.h>
#include <proj/tui/settings.h>
#include <proj/tui/theme.h>

/// The terminal editor: buffers, key schemes, settings, the console, and
/// the link to the interpreter and server (the `tui` module).
namespace tui {

/// Startup options.
struct appOptions {
    /// Project root directory.
    std::filesystem::path projectRoot = ".";
    std::vector<std::filesystem::path> files;  ///< Files to open, relative to the root
    std::filesystem::path settingsFile;        ///< Empty: settingsPathFromEnvironment()
    bool persistSettings = true;               ///< Write pane changes to the settings file
    linkOptions link;                          ///< Interpreter and server connection
};

/// The editor. Every method runs on the UI thread. The class can be
/// driven without a terminal: feed keys with handleKey(), mouse events
/// with handleMouse(), and draw with render().
class app {
public:
    /// Loads settings, opens the files in @p options, and starts the
    /// interpreter thread.
    explicit app(appOptions options);
    ~app();

    app(const app&) = delete;
    app& operator=(const app&) = delete;

    /// Runs the full-screen FTXUI loop until quit. Returns the exit code.
    int run();

    /// @name Input
    /// @{

    /// Handles one key in keymap.h notation. Returns false if ignored.
    bool handleKey(std::string_view key);
    /// Handles one mouse event in screen coordinates.
    bool handleMouse(const ftxui::Mouse& mouse);
    /// Runs the command named @p name (`tui.md` §8.1). Returns false for
    /// an unknown name.
    bool runCommand(std::string_view name, bool shifted = false);
    /// Collects interpreter replies into the console.
    void collectReplies();
    /// @}

    /// Builds the screen. Clickable regions are recorded for
    /// handleMouse() once FTXUI lays out the element.
    ftxui::Element render();

    /// @name State, for the views and tests
    /// @{

    /// The buffer being edited.
    buffer& current() { return buffers_[current_]->text; }
    /// The buffer being edited.
    const buffer& current() const { return buffers_[current_]->text; }
    /// Number of open buffers.
    std::size_t bufferCount() const noexcept { return buffers_.size(); }
    /// Console contents.
    const consoleLog& console() const noexcept { return console_; }
    /// Settings in effect.
    const tui::settings& currentSettings() const noexcept { return settings_; }
    /// The bottom-line prompt.
    const minibuffer& prompt() const noexcept { return minibuffer_; }
    /// The message on the bottom line.
    const std::string& message() const noexcept { return message_; }
    /// Key scheme and mode.
    const keySequencer& keys() const noexcept { return keys_; }
    /// Server state shown in the status bar.
    const serverStatus& status() const noexcept { return status_; }
    /// The theme in effect.
    const theme& currentTheme() const noexcept { return theme_.get(); }
    /// True after Quit completed.
    bool quitRequested() const noexcept { return quit_; }
    /// True while an alert dialog is open.
    bool dialogOpen() const noexcept { return dialog_.has_value(); }
    /// True while a popover menu is open.
    bool menuOpen() const noexcept { return menu_.has_value(); }
    /// Name of the open modal overlay: "preferences", "shortcuts",
    /// "about", or empty.
    std::string_view overlayName() const noexcept;
    /// The interpreter queue.
    interpreterLink& link() noexcept { return link_; }
    /// @}

    /// Opens @p path (relative to the project root unless absolute) in a
    /// buffer, or switches to it if already open. Returns false if the
    /// file exists but cannot be read.
    bool openFile(const std::filesystem::path& path);

private:
    // A pane whose box clips the clickable targets drawn in it.
    enum class region : std::uint8_t { none, menu, fileList, tabBar, console, message, dialog, overlay };

    // A clickable screen region; filled by ftxui::reflect during layout.
    struct target {
        ftxui::Box box;
        std::function<void(const ftxui::Mouse&)> press;
        std::function<void(const ftxui::Mouse&)> drag;
        std::function<void(int)> wheel;      // -1 up, +1 down
        region within = region::none;        // clip region; hits outside it are ignored
        std::string tooltip;                 // shown in the message line on hover
        std::uint64_t surface =
            0;  // surfaces_ when drawn; a popover or modal target is stale once it changes
    };

    // An alert dialog: heading, body, and Cancel / Discard / Save
    // responses (`tui.md` §7.4).
    struct dialog {
        std::string heading;
        std::string body;
        std::function<void(char)> answer;  // 's', 'd', or 'c'
    };

    // A popover menu row. An entry without a label is a section separator.
    struct menuEntry {
        std::string label;
        std::function<void()> action;
        std::string hint;              // key binding in the current scheme
        std::optional<bool> checked;   // toggle state, for check items
    };

    // An open buffer, its stable identity, and its highlighting cache.
    struct openBuffer {
        buffer text;
        std::uint64_t id = 0;
        std::optional<std::uint64_t> highlightedVersion;  // version `highlights` was computed from
        highlightResult highlights;
    };

    enum class promptKind : std::uint8_t { none, openFile, saveAs, switchBuffer, search, command, fade };
    enum class overlayKind : std::uint8_t { none, preferences, shortcuts, about };

    // A row of the Preferences dialog.
    struct preferenceRow {
        enum class control : std::uint8_t { choice, stepper, toggle };
        std::string group;
        std::string title;
        std::string value;
        std::function<void(int)> change;  // -1 or +1
        control kind = control::choice;
        bool on = false;  // a toggle's state
    };

    // (i + dir) wrapped into [0, n).
    static std::size_t wrapIndex(std::size_t i, int dir, std::size_t n) noexcept;

    // Commands
    void registerCommands();
    void movement(bool shifted, const std::function<void(buffer&)>& move);
    void save(std::function<void()> then = {});
    void saveAs(std::function<void()> then = {});
    void closeBuffer(std::size_t index, const std::function<void()>& then = {});
    void quitNext();
    void startSearch(bool forward);
    bool findNext(bool forward, bool fromOrigin);
    void play();
    void stop();
    void evalLastForm();
    void addSources(runRequest& request);
    void startCommandLine();
    void setKeyScheme(keyScheme scheme);
    void setTheme(const theme& t);
    void setFade(double seconds);
    void persistSettings();
    void persistPending();  // writes settings if a drag or scroll changed them
    void selectBuffer(std::size_t index);

    // Input helpers
    bool handlePromptKey(std::string_view key);
    bool handleDialogKey(std::string_view key);
    bool handleMenuKey(std::string_view key);
    bool handleOverlayKey(std::string_view key);
    void submitPrompt();
    void cancelPrompt();
    void openPrompt(promptKind kind, std::string label, std::string initial = {},
                    std::vector<std::string> candidates = {});
    void ask(std::string heading, std::string body, std::function<void(char)> answer);
    void answerDialog(char choice);
    void openMenu(std::vector<menuEntry> entries, int x, int y, bool alignRight = false);
    void openOverlay(overlayKind kind);
    void jumpTo(const sourceLocation& location);

    // Rendering
    ftxui::Element clickable(ftxui::Element e, std::function<void(const ftxui::Mouse&)> press,
                             std::function<void(int)> wheel = {},
                             std::function<void(const ftxui::Mouse&)> drag = {},
                             std::string tooltip = {});
    ftxui::Element renderHeaderBar();
    ftxui::Element renderSidebar();
    ftxui::Element renderTabBar();
    ftxui::Element renderEditorPane();
    ftxui::Element renderConsole();
    ftxui::Element renderStatusBar();
    ftxui::Element renderMessageLine();
    ftxui::Element renderMenu(const std::vector<menuEntry>& entries);
    ftxui::Element renderDialog(const dialog& d);
    ftxui::Element renderOverlay();
    ftxui::Element renderPreferences();
    ftxui::Element renderShortcuts();
    ftxui::Element renderAbout();
    std::vector<menuEntry> mainMenu();
    std::vector<menuEntry> contextMenu();
    std::vector<preferenceRow> preferenceRows();
    std::string keyHint(std::string_view command) const;
    menuEntry item(std::string_view command);  // labeled by commandLabel()
    int windowWidth() const;
    bool narrow() const;

    // Helpers
    const highlightResult& highlights();  // of the current buffer
    void addBuffer(buffer b);
    std::optional<std::size_t> indexOf(std::uint64_t bufferId) const;
    std::string displayName(const buffer& b) const;
    std::filesystem::path resolve(const std::filesystem::path& p) const;
    std::filesystem::path relative(const std::filesystem::path& p) const;
    int editorRows() const;
    int editorColumns() const;
    void refreshFileList();

    const ftxui::Box& boxOf(region r) const noexcept;
    bool regionOpen(region r) const noexcept;  // false for a popover or modal that is closed

    // Members are grouped by alignment to avoid padding: 8-byte members,
    // then the 16-byte-aligned ones (std::function and its holders), then
    // boxes and the small scalars.
    std::string clientVersion_;
    std::filesystem::path root_;
    std::optional<std::filesystem::path> settingsFile_;  // absent: no home directory, nothing persists
    tui::settings settings_;
    keySequencer keys_;
    std::reference_wrapper<const theme> theme_{defaultTheme()};
    std::vector<std::unique_ptr<openBuffer>> buffers_;
    std::size_t current_ = 0;
    std::uint64_t nextBufferId_ = 0;
    std::vector<std::filesystem::path> projectFiles_;
    consoleLog console_;
    minibuffer minibuffer_;
    std::optional<std::vector<menuEntry>> menu_;
    std::size_t menuFocus_ = 0;
    std::size_t overlayFocus_ = 0;
    std::size_t overlayScroll_ = 0;
    std::map<std::string, std::function<void(bool)>, std::less<>> commands_;
    std::string clipboard_;
    std::string lastCommand_;
    std::string message_;
    std::string hoverTip_;
    std::string lastSearch_;
    std::size_t searchOrigin_ = 0;
    std::size_t consoleScroll_ = 0;  // entries hidden below the bottom
    std::size_t fileListScroll_ = 0;
    serverStatus status_;
    std::deque<target> targets_;
    std::uint64_t promptBuffer_ = 0;  // the buffer a Save As prompt names
    std::uint64_t surfaces_ = 0;      // counts popovers and modals opened, to tell their targets apart

    std::optional<dialog> dialog_;
    std::function<void(const ftxui::Mouse&)> dragAction_;
    std::function<void()> promptThen_;
    std::function<void()> exit_;
    // Declared after the members above, so its worker thread joins before
    // they are destroyed.
    interpreterLink link_;

    ftxui::Box rootBox_;
    ftxui::Box editorBox_;
    ftxui::Box fileListBox_;
    ftxui::Box tabBarBox_;
    ftxui::Box consoleBox_;
    ftxui::Box menuBox_;
    ftxui::Box messageBox_;
    ftxui::Box dialogBox_;
    ftxui::Box overlayBox_;
    int menuX_ = 0;
    int menuY_ = 0;
    promptKind promptKind_ = promptKind::none;
    overlayKind overlay_ = overlayKind::none;
    region clipTo_ = region::none;  // clip region for targets created while rendering a pane
    bool persist_;
    bool menuAlignRight_ = false;
    bool searchForward_ = true;
    bool shiftSelecting_ = false;
    bool followCursor_ = true;
    bool quit_ = false;
    bool persistPending_ = false;  // settings changed by a drag or scroll, written on the next other input
};

} // namespace tui
