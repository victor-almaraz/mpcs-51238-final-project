# Third-party dependencies. Every library comes through FetchContent,
# pinned to a release tag, downloaded to third_party/ (untracked), and
# built as a static CMake target.

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
