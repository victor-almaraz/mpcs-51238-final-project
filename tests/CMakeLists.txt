# One Catch2 executable per module: build/tests/<module>_tests.

include(Catch)

function(proj_add_tests module)
    set(target ${module}_tests)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${module}/*.cpp")
    if(NOT sources)
        return()  # the module has no tests yet
    endif()
    add_executable(${target} ${sources})
    target_link_libraries(${target} PRIVATE proj::${module} Catch2::Catch2WithMain)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(${target} PRIVATE PROJ_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tests")
    proj_set_warnings(${target})
    catch_discover_tests(${target} TEST_PREFIX "${module}: ")
    set_property(GLOBAL APPEND PROPERTY PROJ_TEST_TARGETS ${target})
endfunction()

foreach(module metadata ir protocol primitives interpreter tui server)
    proj_add_tests(${module})
endforeach()

# Command-line checks of the executables: each refused value must name the
# option, whatever the exit code. They also add the executables to
# test-badge's dependencies.
function(proj_add_cli_test name target expected)
    add_test(NAME "apps: ${name}" COMMAND ${target} ${ARGN})
    set_tests_properties("apps: ${name}" PROPERTIES PASS_REGULAR_EXPRESSION "${expected}" TIMEOUT 10)
    set_property(GLOBAL APPEND PROPERTY PROJ_TEST_TARGETS ${target})
endfunction()

if(PROJ_BUILD_CLIENT)
    proj_add_cli_test("client refuses a seed that is not a number" proj_client "invalid --seed 1x"
        --compile "${PROJECT_SOURCE_DIR}/examples/delay.lisp" --seed 1x)
endif()
if(PROJ_BUILD_SERVER)
    # --render with a missing IR file: a refused option stops before the
    # file is read.
    foreach(option "--sample-rate;nan" "--sample-rate;inf" "--seconds;nan" "--block-size;256.5"
                   "--outputs;2.5" "--port;nan")
        list(GET option 0 flag)
        list(GET option 1 value)
        proj_add_cli_test("server refuses ${flag} ${value}" proj_server "invalid argument ${flag} ${value}"
            --render missing.ir.json --out missing.wav ${flag} ${value})
    endforeach()
endif()

# test-badge runs the suite and rewrites docs/badges/tests.svg.
get_property(testTargets GLOBAL PROPERTY PROJ_TEST_TARGETS)
add_custom_target(test-badge
    COMMAND ${CMAKE_COMMAND}
        -DBUILD_DIR=${PROJECT_BINARY_DIR}
        -DOUTPUT=${PROJECT_SOURCE_DIR}/docs/badges/tests.svg
        -P ${PROJECT_SOURCE_DIR}/cmake/TestBadge.cmake
    DEPENDS ${testTargets}
    COMMENT "Running the test suite and writing docs/badges/tests.svg"
    VERBATIM)

# coverage-badge runs the suite in a PROJ_COVERAGE tree and rewrites
# docs/badges/coverage.svg. The LLVM tools come from the compiler's own
# directory first, so their profile format matches it.
if(PROJ_COVERAGE)
    get_filename_component(compilerDir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(PROJ_LLVM_PROFDATA NAMES llvm-profdata HINTS "${compilerDir}" REQUIRED)
    find_program(PROJ_LLVM_COV NAMES llvm-cov HINTS "${compilerDir}" REQUIRED)
    list(TRANSFORM testTargets REPLACE "(.+)" "-object=$<TARGET_FILE:\\1>" OUTPUT_VARIABLE objects)
    add_custom_target(coverage-badge
        COMMAND ${CMAKE_COMMAND}
            -DBUILD_DIR=${PROJECT_BINARY_DIR}
            -DOUTPUT=${PROJECT_SOURCE_DIR}/docs/badges/coverage.svg
            -DLLVM_PROFDATA=${PROJ_LLVM_PROFDATA}
            -DLLVM_COV=${PROJ_LLVM_COV}
            "-DOBJECTS=$<JOIN:${objects},|>"
            -P ${PROJECT_SOURCE_DIR}/cmake/CoverageBadge.cmake
        DEPENDS ${testTargets}
        COMMENT "Running the test suite with coverage and writing docs/badges/coverage.svg"
        VERBATIM)
endif()
