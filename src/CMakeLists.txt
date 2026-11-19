# Module libraries. Each module links only what AGENTS.md permits.

espresso_add_module(metadata
    SOURCES
        metadata/graph.cpp
        metadata/primitiveSpec.cpp)

# The IR schema is embedded in the ir library as a string.
set(ESPRESSO_IR_SCHEMA_FILE "${PROJECT_SOURCE_DIR}/schema/ir.schema.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ESPRESSO_IR_SCHEMA_FILE}")
file(READ "${ESPRESSO_IR_SCHEMA_FILE}" ESPRESSO_IR_SCHEMA)
configure_file(ir/schema.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/ir/schema.cpp" @ONLY)

espresso_add_module(ir
    SOURCES
        ir/json.cpp
        ir/validate.cpp
        ir/order.cpp
        ir/hazards.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/ir/schema.cpp"
    LINKS espresso::metadata nlohmann_json::nlohmann_json nlohmann_json_schema_validator)

espresso_add_module(protocol
    SOURCES
        protocol/osc.cpp
        protocol/crc32.cpp
        protocol/udp.cpp
        protocol/irTransfer.cpp
        protocol/serverSession.cpp
        protocol/client.cpp)
find_package(Threads REQUIRED)
target_link_libraries(espresso_protocol PUBLIC Threads::Threads)

espresso_add_module(primitives
    SOURCES
        primitives/event.cpp
        primitives/canvas.cpp
        primitives/frame.cpp
        primitives/images.cpp
        primitives/pipe.cpp
        primitives/primitive.cpp
        primitives/io.cpp
        primitives/plan.cpp
        primitives/buffer.cpp
        primitives/engine.cpp
        primitives/transition.cpp
        primitives/oscillators.cpp
        primitives/filters.cpp
        primitives/modulation.cpp
        primitives/delays.cpp
        primitives/sampling.cpp
        primitives/spectral.cpp
        primitives/math.cpp
        primitives/events.cpp
        primitives/devices.cpp
    LINKS espresso::metadata)
target_link_libraries(espresso_primitives PUBLIC Threads::Threads PRIVATE blend2d::blend2d espresso_pffft)

# The standard library, lib/std/*.lisp, is embedded in the interpreter as
# string literals; editing a library file re-embeds it at the next build.
file(GLOB ESPRESSO_LIBRARY_FILES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/lib/std/*.lisp")
set(ESPRESSO_LIBRARY_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/interpreter/standardLibrary.cpp")
add_custom_command(
    OUTPUT "${ESPRESSO_LIBRARY_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" "-DROOT=${PROJECT_SOURCE_DIR}/lib" "-DFILES=${ESPRESSO_LIBRARY_FILES}"
            "-DOUT=${ESPRESSO_LIBRARY_SOURCE}" -P "${PROJECT_SOURCE_DIR}/cmake/EmbedLibrary.cmake"
    DEPENDS ${ESPRESSO_LIBRARY_FILES} "${PROJECT_SOURCE_DIR}/cmake/EmbedLibrary.cmake"
    COMMENT "Embedding the standard library"
    VERBATIM)

espresso_add_module(interpreter
    SOURCES
        interpreter/lexer.cpp
        interpreter/reader.cpp
        interpreter/evaluator.cpp
        interpreter/builtins.cpp
        interpreter/graph.cpp
        interpreter/modules.cpp
        interpreter/objects.cpp
        "${ESPRESSO_LIBRARY_SOURCE}"
    LINKS espresso::ir espresso::metadata espresso::protocol)
# The generated library source includes standardLibrary.h from the build tree.
target_include_directories(espresso_interpreter PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/interpreter")

if(ESPRESSO_BUILD_CLIENT OR ESPRESSO_BUILD_TESTS)
    espresso_add_module(tui
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
        LINKS espresso::interpreter ftxui::component ftxui::dom ftxui::screen)
endif()

add_subdirectory(apps)
