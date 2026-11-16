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
# The server suite also renders pieces written with the standard library,
# compiled from source.
if(TARGET server_tests)
    target_link_libraries(server_tests PRIVATE proj::interpreter)
endif()

# Command-line checks of the executables: each refused value must name the
# option, whatever the exit code. They also add the executables to
# test-badge's dependencies.
#
# The timeout allows for a new executable's first launch: macOS scans a
# freshly linked binary before it runs, which took 3 to 6 s here with the
# suite running in parallel, and every CLI test after a relink waits for
# it. Later launches take milliseconds; the timeout only catches a hang.
set(PROJ_CLI_TEST_TIMEOUT 60)
function(proj_add_cli_test name target expected)
    add_test(NAME "apps: ${name}" COMMAND ${target} ${ARGN})
    set_tests_properties("apps: ${name}" PROPERTIES PASS_REGULAR_EXPRESSION "${expected}"
        TIMEOUT ${PROJ_CLI_TEST_TIMEOUT})
    set_property(GLOBAL APPEND PROPERTY PROJ_TEST_TARGETS ${target})
endfunction()

set(cliProject "${CMAKE_CURRENT_SOURCE_DIR}/cli/project")
if(PROJ_BUILD_CLIENT)
    proj_add_cli_test("client refuses a seed that is not a number" proj_client "invalid --seed 1x"
        --compile "${PROJECT_SOURCE_DIR}/examples/delay.lisp" --seed 1x)
    proj_add_cli_test("client compiles a project file with its imports" proj_client "\"format\": \"proj-ir\""
        --compile "${cliProject}/main.lisp")
    proj_add_cli_test("client reports warnings with their file" proj_client "main.lisp:14:5: warning: a gain of 100"
        --compile "${cliProject}/main.lisp" --entry loud)
    proj_add_cli_test("client reports errors with their file" proj_client "main.lisp:[0-9]+:1: no piece named nosuch"
        --compile "${cliProject}/main.lisp" --entry nosuch)
    proj_add_cli_test("client reports a file it cannot open" proj_client "cannot open missing.lisp"
        --compile missing.lisp)
    proj_add_cli_test("client refuses an unknown option" proj_client "unknown option --bogus" --bogus)
    proj_add_cli_test("client prints its version" proj_client "proj-client [0-9]+[.][0-9]+[.][0-9]+" --version)
    proj_add_cli_test("client prints its usage" proj_client "usage: proj-client" --help)
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
    set(renders "${CMAKE_CURRENT_BINARY_DIR}/renders")
    proj_add_cli_test("server reports an IR file it cannot read" proj_server "cannot read missing.ir.json"
        --render missing.ir.json --out "${renders}/missing.wav")
    proj_add_cli_test("server reports IR that is not JSON" proj_server "invalid IR"
        --render "${PROJECT_SOURCE_DIR}/examples/delay.lisp" --out "${renders}/invalid.wav")
    proj_add_cli_test("server refuses a render longer than a WAV file holds" proj_server "does not fit in a WAV file"
        --render "${PROJECT_SOURCE_DIR}/examples/delay.ir.json" --out "${renders}/long.wav"
        --outputs 64 --sample-rate 384000 --seconds 1000)
    proj_add_cli_test("server reports a WAV file it cannot write" proj_server "cannot write .*/no-such-dir/out.wav"
        --render "${PROJECT_SOURCE_DIR}/examples/delay.ir.json" --out "${renders}/no-such-dir/out.wav" --seconds 0.01)
    proj_add_cli_test("server prints its version" proj_server "proj-server [0-9]+[.][0-9]+[.][0-9]+" --version)
    proj_add_cli_test("server prints its usage" proj_server "usage: proj-server" --help)
    # Offline renders through the executable: audio, and audio with frames.
    foreach(example delay orbit)
        add_test(NAME "apps: server renders ${example} offline"
            COMMAND ${CMAKE_COMMAND} -DSERVER=$<TARGET_FILE:proj_server>
                -DIR=${PROJECT_SOURCE_DIR}/examples/${example}.ir.json -DOUT=${renders}/${example}.wav
                $<$<STREQUAL:${example},orbit>:-DFRAMES_DIR=${renders}/${example}-frames>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/render.cmake)
        set_tests_properties("apps: server renders ${example} offline" PROPERTIES TIMEOUT ${PROJ_CLI_TEST_TIMEOUT})
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
