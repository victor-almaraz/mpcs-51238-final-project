# Third-party dependencies. Every library comes through FetchContent,
# pinned to a release tag (or, for projects that publish none, a commit),
# downloaded to third_party/ (untracked), and built as a static CMake
# target.

# Set before include(FetchContent), which otherwise caches its default.
set(FETCHCONTENT_BASE_DIR "${PROJECT_SOURCE_DIR}/third_party" CACHE PATH
    "Download and build directory for FetchContent dependencies")

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- Pins ---------------------------------------------------------------
set(PROJ_NLOHMANN_JSON_TAG    v3.12.0)
set(PROJ_JSON_VALIDATOR_TAG   2.4.1)
set(PROJ_FTXUI_TAG            v7.0.3)
set(PROJ_CATCH2_TAG           v3.16.1)
set(PROJ_JUCE_TAG             9.0.5)
# Blend2D and stb publish no release tags; these are master commits.
set(PROJ_BLEND2D_COMMIT       58ca9460b4138af6e793184682e9324e0a8053b2)
set(PROJ_STB_COMMIT           9600522de53973336fa033a289858a4354cbb04c)

# --- nlohmann/json (ir) -------------------------------------------------
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        ${PROJ_NLOHMANN_JSON_TAG}
    GIT_SHALLOW    TRUE
    SYSTEM)

# --- json-schema-validator (ir) -----------------------------------------
set(JSON_VALIDATOR_BUILD_TESTS OFF CACHE INTERNAL "")
set(JSON_VALIDATOR_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(JSON_VALIDATOR_INSTALL OFF CACHE INTERNAL "")
set(JSON_VALIDATOR_SHARED_LIBS OFF CACHE INTERNAL "")
FetchContent_Declare(json_schema_validator
    GIT_REPOSITORY https://github.com/pboettch/json-schema-validator.git
    GIT_TAG        ${PROJ_JSON_VALIDATOR_TAG}
    GIT_SHALLOW    TRUE
    SYSTEM)

FetchContent_MakeAvailable(nlohmann_json json_schema_validator)

# --- Blend2D (primitives: frame rasterization) ----------------------------
# Static, without the JIT: every host runs the same portable pipelines, so
# a frame's pixels do not depend on the CPU (offline renders are
# deterministic).
set(BLEND2D_STATIC ON CACHE INTERNAL "")
set(BLEND2D_NO_JIT ON CACHE INTERNAL "")
set(BLEND2D_NO_INSTALL ON CACHE INTERNAL "")
set(BLEND2D_TEST OFF CACHE INTERNAL "")
set(BLEND2D_DEMOS OFF CACHE INTERNAL "")
FetchContent_Declare(blend2d
    GIT_REPOSITORY https://github.com/blend2d/blend2d.git
    GIT_TAG        ${PROJ_BLEND2D_COMMIT}
    SYSTEM)
FetchContent_MakeAvailable(blend2d)

# --- stb_image, stb_image_write (server: image files, PNG frames) -------
# Header-only; the server core compiles the implementations once.
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        ${PROJ_STB_COMMIT}
    SYSTEM)
FetchContent_MakeAvailable(stb)
add_library(proj_stb INTERFACE)
target_include_directories(proj_stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")

# --- FTXUI (tui) --------------------------------------------------------
if(PROJ_BUILD_CLIENT OR PROJ_BUILD_TESTS)
    set(FTXUI_BUILD_EXAMPLES OFF CACHE INTERNAL "")
    set(FTXUI_BUILD_DOCS OFF CACHE INTERNAL "")
    set(FTXUI_BUILD_TESTS OFF CACHE INTERNAL "")
    set(FTXUI_ENABLE_INSTALL OFF CACHE INTERNAL "")
    FetchContent_Declare(ftxui
        GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
        GIT_TAG        ${PROJ_FTXUI_TAG}
        GIT_SHALLOW    TRUE
        SYSTEM)
    FetchContent_MakeAvailable(ftxui)
endif()

# --- Catch2 (tests) -----------------------------------------------------
if(PROJ_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        ${PROJ_CATCH2_TAG}
        GIT_SHALLOW    TRUE
        SYSTEM)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()

# --- JUCE (server app only) ---------------------------------------------
if(PROJ_BUILD_SERVER)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        ${PROJ_JUCE_TAG}
        GIT_SHALLOW    TRUE
        SYSTEM)
    FetchContent_MakeAvailable(JUCE)
endif()
