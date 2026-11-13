/// @file
/// The editor application: owns buffers, routes commands, and runs the
/// FTXUI loop (`tui.md` §3, §4).

#pragma once

#include <cstddef>
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

namespace tui {

/// Startup options.
struct appOptions {
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
    buffer& current() { return *buffers_[current_]; }
    std::size_t bufferCount() const noexcept { return buffers_.size(); }
    const consoleLog& console() const noexcept { return console_; }
    const tui::settings& currentSettings() const noexcept { return settings_; }
    const minibuffer& prompt() const noexcept { return minibuffer_; }
    const std::string& message() const noexcept { return message_; }
    const keySequencer& keys() const noexcept { return keys_; }
    const serverStatus& status() const noexcept { return status_; }
    const theme& currentTheme() const noexcept { return *theme_; }
    bool quitRequested() const noexcept { return quit_; }
    bool dialogOpen() const noexcept { return dialog_.has_value(); }
    bool menuOpen() const noexcept { return menu_.has_value(); }
    /// Name of the open modal overlay: "preferences", "shortcuts",
    /// "about", or empty.
    std::string_view overlayName() const noexcept;
    interpreterLink& link() noexcept { return link_; }
    /// @}

    /// Opens @p path (relative to the project root unless absolute) in a
    /// buffer, or switches to it if already open. Returns false if the
    /// file exists but cannot be read.
    bool openFile(const std::filesystem::path& path);

private:
    // A clickable screen region; filled by ftxui::reflect during layout.
    struct target {
        ftxui::Box box;
        std::function<void(const ftxui::Mouse&)> press;
        std::function<void(const ftxui::Mouse&)> drag;
        std::function<void(int)> wheel;      // -1 up, +1 down
        const ftxui::Box* within = nullptr;  // clip region; hits outside it are ignored
        std::string tooltip;                 // shown in the message line on hover
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

    enum class promptKind { none, openFile, saveAs, switchBuffer, search, command, fade };
    enum class overlayKind { none, preferences, shortcuts, about };

    // A row of the Preferences dialog.
    struct preferenceRow {
        std::string group;
        std::string title;
        std::string value;
        std::function<void(int)> change;  // -1 or +1
    };

    // Commands
    void registerCommands();
    void movement(bool shifted, const std::function<void(buffer&)>& move);
    void save(std::function<void()> then = {});
    void saveAs(std::function<void()> then = {});
    void closeBuffer(std::size_t index, std::function<void()> then = {});
    void quitNext();
    void startSearch(bool forward);
    bool findNext(bool forward, bool fromOrigin);
    void play();
    void stop();
    void evalLastForm();
    void startCommandLine();
    void setKeyScheme(keyScheme scheme);
    void setTheme(const theme& t);
    void setFade(double seconds);
    void persistSettings();
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
    ftxui::Element renderMenu();
    ftxui::Element renderDialog();
    ftxui::Element renderOverlay();
    ftxui::Element renderPreferences();
    ftxui::Element renderShortcuts();
    ftxui::Element renderAbout();
    std::vector<menuEntry> mainMenu();
    std::vector<menuEntry> contextMenu();
    std::vector<preferenceRow> preferenceRows();
    std::string keyHint(std::string_view command) const;
    menuEntry item(std::string label, const char* command);
    int windowWidth() const;
    bool narrow() const;

    // Helpers
    const highlightResult& highlights(const buffer& b);
    std::string displayName(const buffer& b) const;
    std::filesystem::path resolve(const std::filesystem::path& p) const;
    std::filesystem::path relative(const std::filesystem::path& p) const;
    int editorRows() const;
    int editorColumns() const;
    void refreshFileList();

    appOptions options_;
    std::filesystem::path root_;
    std::filesystem::path settingsFile_;
    tui::settings settings_;
    keySequencer keys_;
    const theme* theme_ = &defaultTheme();
    std::vector<std::unique_ptr<buffer>> buffers_;
    std::size_t current_ = 0;
    std::map<const buffer*, std::pair<std::optional<std::uint64_t>, highlightResult>> highlightCache_;
    std::vector<std::filesystem::path> projectFiles_;
    consoleLog console_;
    minibuffer minibuffer_;
    promptKind promptKind_ = promptKind::none;
    std::optional<dialog> dialog_;
    std::optional<std::vector<menuEntry>> menu_;
    int menuX_ = 0;
    int menuY_ = 0;
    bool menuAlignRight_ = false;
    std::size_t menuFocus_ = 0;
    overlayKind overlay_ = overlayKind::none;
    std::size_t overlayFocus_ = 0;
    std::size_t overlayScroll_ = 0;
    std::map<std::string, std::function<void(bool)>, std::less<>> commands_;
    std::string clipboard_;
    std::string lastCommand_;
    std::string message_;
    std::string hoverTip_;
    std::string lastSearch_;
    bool searchForward_ = true;
    std::size_t searchOrigin_ = 0;
    std::size_t consoleScroll_ = 0;  // entries hidden below the bottom
    std::size_t fileListScroll_ = 0;
    bool shiftSelecting_ = false;
    bool followCursor_ = true;
    bool quit_ = false;
    serverStatus status_;
    interpreterLink link_;
    std::function<void()> exit_;

    std::deque<target> targets_;
    std::function<void(const ftxui::Mouse&)> dragAction_;
    bool persistOnRelease_ = false;
    std::function<void()> promptThen_;
    const ftxui::Box* clipTo_ = nullptr;  // clip region for targets created while rendering a pane
    ftxui::Box rootBox_;
    ftxui::Box editorBox_;
    ftxui::Box fileListBox_;
    ftxui::Box tabBarBox_;
    ftxui::Box consoleBox_;
    ftxui::Box menuBox_;
    ftxui::Box messageBox_;
    ftxui::Box dialogBox_;
    ftxui::Box overlayBox_;
};

} // namespace tui
