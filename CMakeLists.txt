cmake_minimum_required(VERSION 3.25)

project(proj
    VERSION 0.1.0
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

option(PROJ_BUILD_SERVER "Build the server app (fetches JUCE)" ON)
option(PROJ_BUILD_CLIENT "Build the client app" ON)
option(PROJ_BUILD_TESTS "Build the Catch2 test suite" ON)
option(PROJ_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjHelpers)
include(Dependencies)

add_subdirectory(src)

if(PROJ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(Docs)
