# Module libraries. Each module links only what AGENTS.md permits.

proj_add_module(metadata
    SOURCES metadata/module.cpp)

proj_add_module(ir
    SOURCES ir/module.cpp
    LINKS proj::metadata nlohmann_json::nlohmann_json nlohmann_json_schema_validator)

proj_add_module(protocol
    SOURCES
        protocol/osc.cpp
        protocol/crc32.cpp
        protocol/udp.cpp
        protocol/irTransfer.cpp
        protocol/serverSession.cpp
        protocol/client.cpp)
find_package(Threads REQUIRED)
target_link_libraries(proj_protocol PUBLIC Threads::Threads)

proj_add_module(primitives
    SOURCES primitives/module.cpp
    LINKS proj::metadata)

proj_add_module(interpreter
    SOURCES
        interpreter/lexer.cpp
        interpreter/reader.cpp
    LINKS proj::ir proj::metadata proj::protocol)

if(PROJ_BUILD_CLIENT OR PROJ_BUILD_TESTS)
    proj_add_module(tui
        SOURCES
            tui/utf8.cpp
            tui/buffer.cpp
            tui/fileStore.cpp
            tui/highlighter.cpp
            tui/settings.cpp
            tui/keymap.cpp
            tui/keyNames.cpp
            tui/consoleLog.cpp
            tui/minibuffer.cpp
            tui/editorView.cpp
            tui/interpreterLink.cpp
            tui/app.cpp
        LINKS proj::interpreter ftxui::component ftxui::dom ftxui::screen)
endif()

add_subdirectory(apps)
