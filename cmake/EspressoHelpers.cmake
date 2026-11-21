# Helper functions shared by every target in the project.

include(CheckCXXCompilerFlag)

# Clang 19 and later warn when a designated initializer leaves out a
# member without a default member initializer. Options structs rely on
# that: a control names only the options it uses, and the others are
# value-initialized. clang-tidy's readability-redundant-member-init
# forbids spelling those defaults out, so the warning is turned off where
# the compiler has it.
check_cxx_compiler_flag(-Wmissing-designated-field-initializers ESPRESSO_HAS_DESIGNATED_FIELD_WARNING)

# Pins a target's floating point: no contraction of a multiply and an add
# into one fused multiply-add, which GCC (by default) and Clang (within an
# expression) make where the processor has one (AArch64, or x86 with
# -mfma). A fused and a separate rounding differ in the last bit, so
# without the pin a render's bits would depend on the compiler and the
# processor (threads.md §1.3, §4.2.1).
function(espresso_pin_floating_point target)
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE -ffp-contract=off)
    endif()
endfunction()

# Applies the project warning set to a target, and pins its floating point.
function(espresso_set_warnings target)
    espresso_pin_floating_point(${target})
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(ESPRESSO_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
        if(ESPRESSO_HAS_DESIGNATED_FIELD_WARNING)
            target_compile_options(${target} PRIVATE -Wno-missing-designated-field-initializers)
        endif()
        if(ESPRESSO_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Declares a module library.
#
#   espresso_add_module(<name> [SOURCES <files...>] [LINKS <targets...>])
#
# Creates espresso_<name> with the alias espresso::<name>: a static library, or
# an interface (header-only) library when there are no SOURCES. Public
# headers live in include/espresso/<name>/ and are included as
# <espresso/<name>/header.h>. LINKS are linked PUBLIC; the dependency table
# in AGENTS.md defines what each module may link.
function(espresso_add_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LINKS" ${ARGN})
    set(target espresso_${name})
    if(ARG_SOURCES)
        add_library(${target} STATIC ${ARG_SOURCES})
        set(scope PUBLIC)
        espresso_set_warnings(${target})
    else()
        add_library(${target} INTERFACE)
        set(scope INTERFACE)
    endif()
    add_library(espresso::${name} ALIAS ${target})
    target_include_directories(${target} ${scope} $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
    target_compile_features(${target} ${scope} cxx_std_20)
    if(ARG_LINKS)
        target_link_libraries(${target} ${scope} ${ARG_LINKS})
    endif()
endfunction()
