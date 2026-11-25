# The GUI editor (tauri-editor.md): what CMake prepares for Tauri, which
# builds the app itself (README.md). The web interface's packages and its
# import map (§5.1), and the client and the server, placed where Tauri finds
# its external binaries (§3.1, §7.1).

include(EditorWeb)

set(web "${CMAKE_CURRENT_SOURCE_DIR}/web")
espresso_editor_web("${web}/vendor" ESPRESSO_IMPORT_MAP)
espresso_editor_fonts("${web}/vendor/fonts")
configure_file("${web}/index.html.in" "${web}/index.html" @ONLY)
# The tests load the editor's modules from the build tree's pages, so
# their import map names the packages by absolute URL.
string(REPLACE "./vendor/" "file://${web}/vendor/" absoluteMap "${ESPRESSO_IMPORT_MAP}")
set_property(GLOBAL PROPERTY ESPRESSO_EDITOR_IMPORT_MAP "${absoluteMap}")

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
# The client is the sidecar; the server goes beside it, for the bundle's
# `server` argument (tauri-editor.md §7.1). Both are listed in
# bundle.externalBin, so `cargo tauri dev` and `cargo tauri build` need both.
set(placed "")
foreach(app client server)
    if(NOT TARGET espresso_${app})
        message(STATUS "espresso-${app} is not built: the editor cannot run until it is placed")
        continue()
    endif()
    set(binary "${CMAKE_CURRENT_SOURCE_DIR}/src-tauri/binaries/espresso-${app}-${triple}")
    add_custom_command(OUTPUT "${binary}"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:espresso_${app}>" "${binary}"
        DEPENDS espresso_${app}
        COMMENT "Placing espresso-${app} beside the editor"
        VERBATIM)
    list(APPEND placed "${binary}")
endforeach()
add_custom_target(editor_sidecar ALL DEPENDS ${placed})
