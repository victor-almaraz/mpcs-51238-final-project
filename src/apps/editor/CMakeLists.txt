# The GUI editor (tauri-editor.md): what CMake prepares for Tauri, which
# builds the app itself (README.md). The web interface's packages and its
# import map (§5.1), and the client, placed where Tauri finds its sidecar
# (§3.1).

include(EditorWeb)

set(web "${CMAKE_CURRENT_SOURCE_DIR}/web")
espresso_editor_web("${web}/vendor" ESPRESSO_IMPORT_MAP)
configure_file("${web}/index.html.in" "${web}/index.html" @ONLY)

# Tauri takes an external binary from src-tauri/binaries/, named with the
# target triple of the Rust host (bundle.externalBin).
find_program(ESPRESSO_RUSTC rustc)
if(ESPRESSO_RUSTC)
    execute_process(COMMAND "${ESPRESSO_RUSTC}" --print host-tuple
        OUTPUT_VARIABLE triple OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE found)
endif()
if(NOT ESPRESSO_RUSTC OR NOT found EQUAL 0)
    message(STATUS "rustc 1.84 or newer not found: the editor's sidecar is not placed")
    return()
endif()
set(sidecar "${CMAKE_CURRENT_SOURCE_DIR}/src-tauri/binaries/espresso-client-${triple}")
add_custom_command(OUTPUT "${sidecar}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:espresso_client>" "${sidecar}"
    DEPENDS espresso_client
    COMMENT "Placing espresso-client as the editor's sidecar"
    VERBATIM)
add_custom_target(editor_sidecar ALL DEPENDS "${sidecar}")
