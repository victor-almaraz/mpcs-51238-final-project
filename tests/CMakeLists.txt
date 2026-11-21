# One Catch2 executable per module: build/tests/<module>_tests.

include(Catch)

function(espresso_add_tests module)
    set(target ${module}_tests)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${module}/*.cpp")
    if(NOT sources)
        return()  # the module has no tests yet
    endif()
    add_executable(${target} ${sources})
    target_link_libraries(${target} PRIVATE espresso::${module} Catch2::Catch2WithMain)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(${target} PRIVATE ESPRESSO_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tests")
    espresso_set_warnings(${target})
    # Catch2 tags become CTest labels: `[network]` marks a test that opens
    # local sockets (io.md §14.4).
    catch_discover_tests(${target} TEST_PREFIX "${module}: " ADD_TAGS_AS_LABELS)
    set_property(GLOBAL APPEND PROPERTY ESPRESSO_TEST_TARGETS ${target})
endfunction()

foreach(module metadata ir protocol primitives interpreter tui server)
    espresso_add_tests(${module})
endforeach()
# The server suite also renders pieces written with the standard library,
# compiled from source.
if(TARGET server_tests)
    target_link_libraries(server_tests PRIVATE espresso::interpreter)
endif()

# Command-line checks of the executables: each refused value must name the
# option, whatever the exit code. They also add the executables to
# test-badge's dependencies.
#
# The timeout allows for a new executable's first launch: macOS scans a
# freshly linked binary before it runs, which took 3 to 6 s here with the
# suite running in parallel, and every CLI test after a relink waits for
# it. Later launches take milliseconds; the timeout only catches a hang.
set(ESPRESSO_CLI_TEST_TIMEOUT 60)
function(espresso_add_cli_test name target expected)
    add_test(NAME "apps: ${name}" COMMAND ${target} ${ARGN})
    set_tests_properties("apps: ${name}" PROPERTIES PASS_REGULAR_EXPRESSION "${expected}"
        TIMEOUT ${ESPRESSO_CLI_TEST_TIMEOUT})
    set_property(GLOBAL APPEND PROPERTY ESPRESSO_TEST_TARGETS ${target})
endfunction()

set(cliProject "${CMAKE_CURRENT_SOURCE_DIR}/cli/project")
# The page helper's script, where a JavaScript engine is at hand: macOS's
# JavaScriptCore through osascript. Elsewhere it is checked in a browser
# (io.md §15.1).
find_program(ESPRESSO_OSASCRIPT osascript)
if(ESPRESSO_OSASCRIPT)
    add_test(NAME "page: espresso.js sends receives and reconnects (io.md 12.9)"
        COMMAND "${ESPRESSO_OSASCRIPT}" -l JavaScript "${CMAKE_CURRENT_SOURCE_DIR}/page/espresso_test.js"
                "${PROJECT_SOURCE_DIR}/src/apps/server/espresso.js")
    set_tests_properties("page: espresso.js sends receives and reconnects (io.md 12.9)" PROPERTIES
        PASS_REGULAR_EXPRESSION "espresso.js: ok" TIMEOUT ${ESPRESSO_CLI_TEST_TIMEOUT})
endif()

if(ESPRESSO_BUILD_CLIENT)
    espresso_add_cli_test("client refuses a seed that is not a number" espresso_client "invalid --seed 1x"
        --compile "${cliProject}/main.lisp" --seed 1x)
    espresso_add_cli_test("client compiles a project file with its imports" espresso_client "\"format\": \"espresso-ir\""
        --compile "${cliProject}/main.lisp")
    espresso_add_cli_test("client reports warnings with their file" espresso_client "main.lisp:14:5: warning: a gain of 100"
        --compile "${cliProject}/main.lisp" --entry loud)
    espresso_add_cli_test("client reports errors with their file" espresso_client "main.lisp:[0-9]+:1: no piece named nosuch"
        --compile "${cliProject}/main.lisp" --entry nosuch)
    espresso_add_cli_test("client takes the piece and page port from project.lisp" espresso_client
        "\"page\": {[^}]*\"port\": 8000"
        --compile "${CMAKE_CURRENT_SOURCE_DIR}/cli/paged/main.lisp")
    espresso_add_cli_test("client reports an invalid project.lisp" espresso_client
        "bad/project.lisp:1:21: :page-port takes a port from 1024 to 65535"
        --compile "${CMAKE_CURRENT_SOURCE_DIR}/cli/bad/main.lisp")
    espresso_add_cli_test("client reports a file it cannot open" espresso_client "cannot open missing.lisp"
        --compile missing.lisp)
    espresso_add_cli_test("client refuses an unknown option" espresso_client "unknown option --bogus" --bogus)
    espresso_add_cli_test("client prints its version" espresso_client "espresso-client [0-9]+[.][0-9]+[.][0-9]+" --version)
    espresso_add_cli_test("client prints its usage" espresso_client "usage: espresso-client" --help)
endif()
if(ESPRESSO_BUILD_SERVER)
    # --render with a missing IR file: a refused option stops before the
    # file is read.
    foreach(option "--sample-rate;nan" "--sample-rate;inf" "--seconds;nan" "--block-size;256.5"
                   "--outputs;2.5" "--port;nan" "--buffer-memory;0" "--threads;0" "--threads;65" "--threads;2.5" "--worker-spin;-1")
        list(GET option 0 flag)
        list(GET option 1 value)
        espresso_add_cli_test("server refuses ${flag} ${value}" espresso_server "invalid argument ${flag} ${value}"
            --render missing.ir.json --out missing.wav ${flag} ${value})
    endforeach()
    set(renders "${CMAKE_CURRENT_BINARY_DIR}/renders")
    set(report "${PROJECT_SOURCE_DIR}/examples/report/report.ir.json")
    espresso_add_cli_test("server reports an IR file it cannot read" espresso_server "cannot read missing.ir.json"
        --render missing.ir.json --out "${renders}/missing.wav")
    espresso_add_cli_test("server reports IR that is not JSON" espresso_server "invalid IR"
        --render "${cliProject}/main.lisp" --out "${renders}/invalid.wav")
    espresso_add_cli_test("server refuses a render longer than a WAV file holds" espresso_server "does not fit in a WAV file"
        --render "${report}" --out "${renders}/long.wav"
        --outputs 64 --sample-rate 384000 --seconds 1000)
    espresso_add_cli_test("server reports a WAV file it cannot write" espresso_server "cannot write .*/no-such-dir/out.wav"
        --render "${report}" --out "${renders}/no-such-dir/out.wav" --seconds 0.01)
    espresso_add_cli_test("server refuses a render with no outputs" espresso_server "needs at least one output"
        --render "${report}" --out "${renders}/silent.wav" --outputs 0)
    espresso_add_cli_test("server prints its version" espresso_server "espresso-server [0-9]+[.][0-9]+[.][0-9]+" --version)
    espresso_add_cli_test("server prints its usage" espresso_server "usage: espresso-server" --help)
    # An offline render through the executable.
    add_test(NAME "apps: server renders report offline"
        COMMAND ${CMAKE_COMMAND} -DSERVER=$<TARGET_FILE:espresso_server> -DIR=${report} -DOUT=${renders}/report.wav
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/render.cmake)
    set_tests_properties("apps: server renders report offline" PROPERTIES TIMEOUT ${ESPRESSO_CLI_TEST_TIMEOUT})
endif()

# test-badge runs the suite and rewrites docs/badges/tests.svg.
get_property(testTargets GLOBAL PROPERTY ESPRESSO_TEST_TARGETS)
add_custom_target(test-badge
    COMMAND ${CMAKE_COMMAND}
        -DBUILD_DIR=${PROJECT_BINARY_DIR}
        -DOUTPUT=${PROJECT_SOURCE_DIR}/docs/badges/tests.svg
        -P ${PROJECT_SOURCE_DIR}/cmake/TestBadge.cmake
    DEPENDS ${testTargets}
    COMMENT "Running the test suite and writing docs/badges/tests.svg"
    VERBATIM)

# coverage-badge runs the suite in a ESPRESSO_COVERAGE tree and rewrites
# docs/badges/coverage.svg. The LLVM tools come from the compiler's own
# directory first, so their profile format matches it.
if(ESPRESSO_COVERAGE)
    get_filename_component(compilerDir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(ESPRESSO_LLVM_PROFDATA NAMES llvm-profdata HINTS "${compilerDir}" REQUIRED)
    find_program(ESPRESSO_LLVM_COV NAMES llvm-cov HINTS "${compilerDir}" REQUIRED)
    list(TRANSFORM testTargets REPLACE "(.+)" "-object=$<TARGET_FILE:\\1>" OUTPUT_VARIABLE objects)
    add_custom_target(coverage-badge
        COMMAND ${CMAKE_COMMAND}
            -DBUILD_DIR=${PROJECT_BINARY_DIR}
            -DOUTPUT=${PROJECT_SOURCE_DIR}/docs/badges/coverage.svg
            -DLLVM_PROFDATA=${ESPRESSO_LLVM_PROFDATA}
            -DLLVM_COV=${ESPRESSO_LLVM_COV}
            "-DOBJECTS=$<JOIN:${objects},|>"
            -P ${PROJECT_SOURCE_DIR}/cmake/CoverageBadge.cmake
        DEPENDS ${testTargets}
        COMMENT "Running the test suite with coverage and writing docs/badges/coverage.svg"
        VERBATIM)
endif()
