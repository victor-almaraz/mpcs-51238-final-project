# Module libraries. Each module links only what AGENTS.md permits.

proj_add_module(metadata
    SOURCES
        metadata/graph.cpp
        metadata/primitiveSpec.cpp)

# The IR schema is embedded in the ir library as a string.
set(PROJ_IR_SCHEMA_FILE "${PROJECT_SOURCE_DIR}/schema/ir.schema.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJ_IR_SCHEMA_FILE}")
file(READ "${PROJ_IR_SCHEMA_FILE}" PROJ_IR_SCHEMA)
configure_file(ir/schema.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/ir/schema.cpp" @ONLY)

proj_add_module(ir
    SOURCES
        ir/json.cpp
        ir/validate.cpp
        ir/order.cpp
        ir/hazards.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/ir/schema.cpp"
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
    SOURCES
        primitives/event.cpp
        primitives/canvas.cpp
        primitives/frame.cpp
        primitives/images.cpp
        primitives/pipe.cpp
        primitives/primitive.cpp
        primitives/io.cpp
        primitives/plan.cpp
        primitives/engine.cpp
        primitives/transition.cpp
        primitives/oscillators.cpp
        primitives/filters.cpp
        primitives/modulation.cpp
        primitives/delays.cpp
        primitives/math.cpp
        primitives/events.cpp
        primitives/devices.cpp
    LINKS proj::metadata)
target_link_libraries(proj_primitives PUBLIC Threads::Threads PRIVATE blend2d::blend2d)

proj_add_module(interpreter
    SOURCES
        interpreter/lexer.cpp
        interpreter/reader.cpp
        interpreter/evaluator.cpp
        interpreter/builtins.cpp
        interpreter/graph.cpp
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
            tui/theme.cpp
            tui/editorView.cpp
            tui/interpreterLink.cpp
            tui/app.cpp
            tui/appView.cpp
        LINKS proj::interpreter ftxui::component ftxui::dom ftxui::screen)
endif()

add_subdirectory(apps)
