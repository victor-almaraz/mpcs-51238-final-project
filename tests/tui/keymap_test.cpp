#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>

#include <espresso/tui/keymap.h>
#include <espresso/workspace/settings.h>

using namespace tui;
using outcome = keySequencer::outcome;

namespace {

std::set<std::string_view> boundCommands(workspace::keyScheme scheme) {
    std::set<std::string_view> out;
    for (auto mode : {editMode::insert, editMode::normal, editMode::visual}) {
        for (const auto& b : schemeBindings(scheme, mode)) out.insert(b.command);
    }
    for (const auto& b : commonBindings()) out.insert(b.command);
    if (scheme == workspace::keyScheme::vim) {
        for (const auto& c : exCommands()) out.insert(c.command);
    }
    return out;
}

} // namespace

TEST_CASE("every scheme binds the core command set (tui.md 8.3)", "[tui][keymap]") {
    const auto core = std::to_array<std::string_view>(
        {"openFile",       "save",         "saveAs", "closeBuffer", "switchBuffer",   "nextBuffer",
         "previousBuffer", "quit",         "cancel", "undo",        "redo",           "searchForward",
         "searchBackward", "cut",          "copy",   "paste",       "deleteBackward", "deleteForward",
         "newline",        "evalLastForm", "play",   "stop",        "toggleFileList", "toggleConsole"});
    for (auto scheme : {workspace::keyScheme::emacs, workspace::keyScheme::cua, workspace::keyScheme::vim,
                        workspace::keyScheme::nano}) {
        const auto bound = boundCommands(scheme);
        for (auto c : core) {
            INFO(toString(scheme) << " binds " << c);
            CHECK(bound.count(c) == 1);
        }
    }
}

TEST_CASE("bindings name only known commands", "[tui][keymap]") {
    const auto names = commandNames();
    for (auto scheme : {workspace::keyScheme::emacs, workspace::keyScheme::cua, workspace::keyScheme::vim,
                        workspace::keyScheme::nano}) {
        for (auto c : boundCommands(scheme)) {
            INFO(c);
            CHECK(std::ranges::find(names, c) != names.end());
        }
    }
}

TEST_CASE("the table in tui.md 8.2 holds", "[tui][keymap]") {
    auto resolve = [](workspace::keyScheme scheme, std::initializer_list<std::string_view> keys) {
        keySequencer s(scheme);
        keySequencer::result r;
        for (auto k : keys) r = s.feed(k);
        return r.kind == outcome::command ? r.command : std::string("<none>");
    };
    CHECK(resolve(workspace::keyScheme::emacs, {"C-x", "C-s"}) == "save");
    CHECK(resolve(workspace::keyScheme::emacs, {"C-x", "C-f"}) == "openFile");
    CHECK(resolve(workspace::keyScheme::emacs, {"C-/"}) == "undo");
    CHECK(resolve(workspace::keyScheme::emacs, {"C-x", "C-c"}) == "quit");
    CHECK(resolve(workspace::keyScheme::cua, {"C-s"}) == "save");
    CHECK(resolve(workspace::keyScheme::cua, {"C-o"}) == "openFile");
    CHECK(resolve(workspace::keyScheme::cua, {"C-z"}) == "undo");
    CHECK(resolve(workspace::keyScheme::cua, {"C-q"}) == "quit");
    CHECK(resolve(workspace::keyScheme::nano, {"C-o"}) == "save");
    CHECK(resolve(workspace::keyScheme::nano, {"C-r"}) == "openFile");
    CHECK(resolve(workspace::keyScheme::nano, {"M-u"}) == "undo");
    CHECK(resolve(workspace::keyScheme::nano, {"C-x"}) == "quit");
    CHECK(resolve(workspace::keyScheme::vim, {"u"}) == "undo");
    CHECK(resolveExCommand("w").command == "save");
    CHECK(resolveExCommand("e").command == "openFile");
    CHECK(resolveExCommand("q").command == "quit");
}

TEST_CASE("key sequencer tracks prefixes and self-insert", "[tui][keymap]") {
    keySequencer s(workspace::keyScheme::emacs);
    CHECK(s.feed("C-x").kind == outcome::pending);
    CHECK(s.pendingKeys() == "C-x");
    const auto unbound = s.feed("z");
    CHECK(unbound.kind == outcome::unbound);
    CHECK(unbound.keys == "C-x z");
    CHECK(s.pendingKeys().empty());
    CHECK(s.feed("a").kind == outcome::selfInsert);
    CHECK(s.feed("é").kind == outcome::selfInsert);
    CHECK(s.feed("<up>").command == "previousLine");
    CHECK(s.feed("<f13>").kind == outcome::unbound);
}

TEST_CASE("vim modes change bindings (tui.md 8.4)", "[tui][keymap]") {
    keySequencer s(workspace::keyScheme::vim);
    CHECK(s.mode() == editMode::normal);
    CHECK_FALSE(s.insertsText());
    CHECK(s.feed("q").kind == outcome::unbound);  // printable keys do not insert in normal mode
    CHECK(s.feed("d").kind == outcome::pending);
    CHECK(s.feed("d").command == "deleteLine");
    CHECK(s.feed("i").command == "insertMode");
    s.setMode(editMode::insert);
    CHECK(s.feed("i").kind == outcome::selfInsert);
    CHECK(s.feed("ESC").command == "normalMode");
    s.setMode(editMode::visual);
    CHECK(s.feed("d").command == "cut");

    keySequencer emacs(workspace::keyScheme::emacs);
    emacs.setMode(editMode::normal);
    CHECK(emacs.mode() == editMode::insert);  // only vim is modal
}

TEST_CASE("ex commands take arguments", "[tui][keymap]") {
    const auto e = resolveExCommand("  e  voices.lisp ");
    CHECK(e.command == "openFile");
    CHECK(e.argument == "voices.lisp");
    CHECK(resolveExCommand("wq").command == "saveAndQuit");
    CHECK(resolveExCommand("nope").command.empty());
}
