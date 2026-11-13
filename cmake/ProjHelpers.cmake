# Helper functions shared by every target in the project.

# Applies the project warning set to a target.
function(proj_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(PROJ_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
        if(PROJ_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Declares a module library.
#
#   proj_add_module(<name> [SOURCES <files...>] [LINKS <targets...>])
#
# Creates proj_<name> with the alias proj::<name>: a static library, or
# an interface (header-only) library when there are no SOURCES. Public
# headers live in include/proj/<name>/ and are included as
# <proj/<name>/header.h>. LINKS are linked PUBLIC; the dependency table
# in AGENTS.md defines what each module may link.
function(proj_add_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LINKS" ${ARGN})
    set(target proj_${name})
    if(ARG_SOURCES)
        add_library(${target} STATIC ${ARG_SOURCES})
        set(scope PUBLIC)
        proj_set_warnings(${target})
    else()
        add_library(${target} INTERFACE)
        set(scope INTERFACE)
    endif()
    add_library(proj::${name} ALIAS ${target})
    target_include_directories(${target} ${scope} $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
    target_compile_features(${target} ${scope} cxx_std_20)
    if(ARG_LINKS)
        target_link_libraries(${target} ${scope} ${ARG_LINKS})
    endif()
endfunction()
