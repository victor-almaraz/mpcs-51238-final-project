cmake_minimum_required(VERSION 3.25)

project(espresso
    VERSION 0.2.0
    DESCRIPTION "Workstation for acousmatic, aleatory, and automatic music"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(BUILD_SHARED_LIBS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(ESPRESSO_BUILD_SERVER "Build the server app (fetches JUCE)" ON)
option(ESPRESSO_BUILD_CLIENT "Build the client app" ON)
# The GUI editor's web packages and sidecar; Tauri builds the app itself
# (src/apps/editor/README.md).
option(ESPRESSO_BUILD_EDITOR "Prepare the GUI editor: fetch its web packages, place the client as its sidecar" ${ESPRESSO_BUILD_CLIENT})
option(ESPRESSO_BUILD_TESTS "Build the Catch2 test suite" ON)
option(ESPRESSO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(ESPRESSO_COVERAGE "Instrument the project for LLVM source-based coverage (Clang)" OFF)

# Pins floating point for every target, dependencies included: no
# contraction of a multiply and an add into one fused multiply-add, which
# GCC (by default) and Clang (within an expression) make where the
# processor has one (AArch64, or x86 with -mfma). A fused and a separate
# rounding differ in the last bit, so without the pin a render's bits
# would depend on the compiler and the processor (threads.md §1.3,
# §4.2.1). Set before the dependencies are fetched, so their targets
# inherit it.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-ffp-contract=off)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(EspressoHelpers)
include(Dependencies)

if(ESPRESSO_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ESPRESSO_COVERAGE needs Clang: it uses LLVM source-based coverage")
    endif()
    # After the dependencies, so only the project's own targets are
    # instrumented.
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate)
endif()

add_subdirectory(src)

if(ESPRESSO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(Docs)
