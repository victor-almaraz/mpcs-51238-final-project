# One Catch2 executable per module, build/tests/<module>_tests, and
# build/tests/network_tests for the libcurl transport.

include(Catch)

# Every Catch2 case has a timeout, so a test that waits forever fails
# instead of hanging ctest. The slowest case, the server's thread-count
# renders, takes about 12 s in Debug alone and longer under a parallel
# ctest; the timeout only catches a hang. Sanitizer trees run their
# executables directly, without ctest.
set(ESPRESSO_TEST_TIMEOUT 300)

# espresso_add_tests(<suite> [LINKS <library>]): the cases in
# tests/<suite>/ as build/tests/<suite>_tests, linked with the module
# espresso::<suite>, or with LINKS when the suite is not a module's.
function(espresso_add_tests module)
    cmake_parse_arguments(ARG "" "LINKS" "" ${ARGN})
    if(NOT ARG_LINKS)
        set(ARG_LINKS espresso::${module})
    endif()
    set(target ${module}_tests)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${module}/*.cpp")
    if(NOT sources)
        return()  # the module has no tests yet
    endif()
    add_executable(${target} ${sources})
    target_link_libraries(${target} PRIVATE ${ARG_LINKS} Catch2::Catch2WithMain)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(${target} PRIVATE ESPRESSO_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tests")
    espresso_set_warnings(${target})
    # Catch2 tags become CTest labels: `[network]` marks a test that opens
    # local sockets (io.md §14.4).
    catch_discover_tests(${target} TEST_PREFIX "${module}: " ADD_TAGS_AS_LABELS
        PROPERTIES TIMEOUT ${ESPRESSO_TEST_TIMEOUT})
    set_property(GLOBAL APPEND PROPERTY ESPRESSO_TEST_TARGETS ${target})
endfunction()

foreach(module metadata ir protocol primitives interpreter workspace tui server client)
    espresso_add_tests(${module})
endforeach()
# The server suite also renders pieces written with the standard library,
# compiled from source.
if(TARGET server_tests)
    target_link_libraries(server_tests PRIVATE espresso::interpreter)
endif()
# The libcurl transport's test runs in an executable of its own. On
# macOS, curl_global_init() leaves libdispatch threads that nothing can
# join, and ThreadSanitizer kills the child of any later fork in the same
# process that starts a thread, as the server suite's signal tests do
# (lessons.md, "System libraries leave threads behind"). Apart, both keep
# ThreadSanitizer's default fork check.
espresso_add_tests(network LINKS espresso::server)

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
    add_test(NAME "page: espresso-ui.js maps positions to values and formats them (ui.md 5.2)"
        COMMAND "${ESPRESSO_OSASCRIPT}" -l JavaScript "${CMAKE_CURRENT_SOURCE_DIR}/page/controls_values_test.js"
                "${PROJECT_SOURCE_DIR}/src/apps/server/espresso-ui.js")
    set_tests_properties("page: espresso-ui.js maps positions to values and formats them (ui.md 5.2)" PROPERTIES
        PASS_REGULAR_EXPRESSION "espresso-ui.js values: ok" TIMEOUT ${ESPRESSO_CLI_TEST_TIMEOUT})
endif()

# The page controls in a browser (ui.md §7.2), where Chrome is installed:
# tests/page/controls_test.html drives each control and writes its result
# into the page, which --dump-dom prints. A profile of its own keeps the
# test apart from a Chrome the user has open.
find_program(ESPRESSO_CHROME NAMES google-chrome chromium chromium-browser "Google Chrome"
    PATHS "/Applications/Google Chrome.app/Contents/MacOS" "/Applications/Chromium.app/Contents/MacOS")
if(ESPRESSO_CHROME)
    add_test(NAME "page: espresso-ui.js controls send receive and reset in a browser (ui.md 7.2)"
        COMMAND "${ESPRESSO_CHROME}" --headless=new --disable-gpu --no-first-run
                "--user-data-dir=${PROJECT_BINARY_DIR}/chrome-profile" --allow-file-access-from-files
                --window-size=1280,1600 --dump-dom
                "file://${CMAKE_CURRENT_SOURCE_DIR}/page/controls_test.html")
    set_tests_properties("page: espresso-ui.js controls send receive and reset in a browser (ui.md 7.2)" PROPERTIES
        PASS_REGULAR_EXPRESSION "controls: ok" FAIL_REGULAR_EXPRESSION "controls: FAIL"
        TIMEOUT ${ESPRESSO_CLI_TEST_TIMEOUT} RUN_SERIAL TRUE)
    # The GUI editor's sidecar module against a fake of Tauri's shell plugin
    # (tauri-editor.md §4). The verdict is matched with the <pre>'s tags, so
    # the test's own source, which --dump-dom also prints, cannot match it.
    add_test(NAME "editor: sidecar.js matches responses to requests and passes notifications (tauri-editor.md 4)"
        COMMAND "${ESPRESSO_CHROME}" --headless=new --disable-gpu --no-first-run
                "--user-data-dir=${PROJECT_BINARY_DIR}/chrome-profile" --allow-file-access-from-files
                --virtual-time-budget=5000 --dump-dom
                "file://${CMAKE_CURRENT_SOURCE_DIR}/editor/sidecar_test.html")
    set_tests_properties("editor: sidecar.js matches responses to requests and passes notifications (tauri-editor.md 4)"
        PROPERTIES PASS_REGULAR_EXPRESSION ">sidecar: ok<" FAIL_REGULAR_EXPRESSION ">sidecar: FAIL"
        TIMEOUT ${ESPRESSO_CLI_TEST_TIMEOUT} RUN_SERIAL TRUE)
    # The GUI editor's window against a fake of Tauri (tauri-editor.md §5.2):
    # a page CMake writes into the build tree, with the editor's import map.
    if(ESPRESSO_BUILD_EDITOR)
        get_property(EDITOR_IMPORT_MAP GLOBAL PROPERTY ESPRESSO_EDITOR_IMPORT_MAP)
        set(EDITOR_WEB "file://${PROJECT_SOURCE_DIR}/src/apps/editor/web")
        set(EXAMPLES "${PROJECT_SOURCE_DIR}/examples")
        configure_file(editor/editor_test.html.in "${CMAKE_CURRENT_BINARY_DIR}/editor/editor_test.html" @ONLY)
        add_test(NAME "editor: the window opens, edits, saves, plays, and keys each scheme (tauri-editor.md 5.2, 5.3)"
            COMMAND "${ESPRESSO_CHROME}" --headless=new --disable-gpu --no-first-run
                    "--user-data-dir=${PROJECT_BINARY_DIR}/chrome-profile" --allow-file-access-from-files
                    --window-size=1280,800 --virtual-time-budget=20000 --dump-dom
                    "file://${CMAKE_CURRENT_BINARY_DIR}/editor/editor_test.html")
        set_tests_properties("editor: the window opens, edits, saves, plays, and keys each scheme (tauri-editor.md 5.2, 5.3)"
            PROPERTIES PASS_REGULAR_EXPRESSION ">editor: ok<" FAIL_REGULAR_EXPRESSION ">editor: FAIL"
            TIMEOUT ${ESPRESSO_CLI_TEST_TIMEOUT} RUN_SERIAL TRUE)

        # The window at 375 px (design-guide.md §4.3): each screenshot state in an
        # iframe that wide, since a headless window is at least 500 px.
        set(EDITOR_TEST_PAGE "${CMAKE_CURRENT_BINARY_DIR}/editor/editor_test.html")
        configure_file(editor/narrow_test.html.in "${CMAKE_CURRENT_BINARY_DIR}/editor/narrow_test.html" @ONLY)
        add_test(NAME "editor: the window holds at 375 px in every state (design-guide.md 4.3)"
            COMMAND "${ESPRESSO_CHROME}" --headless=new --disable-gpu --no-first-run
                    "--user-data-dir=${PROJECT_BINARY_DIR}/chrome-profile" --allow-file-access-from-files
                    --window-size=800,800 --virtual-time-budget=60000 --dump-dom
                    "file://${CMAKE_CURRENT_BINARY_DIR}/editor/narrow_test.html")
        set_tests_properties("editor: the window holds at 375 px in every state (design-guide.md 4.3)"
            PROPERTIES PASS_REGULAR_EXPRESSION ">narrow: ok<" FAIL_REGULAR_EXPRESSION ">narrow: FAIL"
            TIMEOUT ${ESPRESSO_CLI_TEST_TIMEOUT} RUN_SERIAL TRUE)

        # The layout's system (design-guide.md §4.1, §4.4): bars, controls, rows, and
        # insets in every screenshot state, measured against the stylesheet's units.
        configure_file(editor/layout_test.html.in "${CMAKE_CURRENT_BINARY_DIR}/editor/layout_test.html" @ONLY)
        add_test(NAME "editor: every bar, control, row, and inset keeps the layout's units (design-guide.md 4.1, 4.4)"
            COMMAND "${ESPRESSO_CHROME}" --headless=new --disable-gpu --no-first-run
                    "--user-data-dir=${PROJECT_BINARY_DIR}/chrome-profile" --allow-file-access-from-files
                    --window-size=1400,900 --virtual-time-budget=60000 --dump-dom
                    "file://${CMAKE_CURRENT_BINARY_DIR}/editor/layout_test.html")
        set_tests_properties("editor: every bar, control, row, and inset keeps the layout's units (design-guide.md 4.1, 4.4)"
            PROPERTIES PASS_REGULAR_EXPRESSION ">layout: ok<" FAIL_REGULAR_EXPRESSION ">layout: FAIL"
            TIMEOUT ${ESPRESSO_CLI_TEST_TIMEOUT} RUN_SERIAL TRUE)

        # The editor's tokenizer against the C++ highlighter (tauri-editor.md
        # §5.4): editor_highlights asks the sidecar to highlight every .lisp
        # file in examples/ and in editor/highlight/, and the page draws each
        # with lisp.js and compares the two, character by character.
        add_executable(editor_highlights editor/highlights.cpp)
        target_link_libraries(editor_highlights PRIVATE espresso::client nlohmann_json::nlohmann_json)
        set_target_properties(editor_highlights PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tests")
        espresso_set_warnings(editor_highlights)
        set_property(GLOBAL APPEND PROPERTY ESPRESSO_TEST_TARGETS editor_highlights)
        set(HIGHLIGHTS "${CMAKE_CURRENT_BINARY_DIR}/editor/highlights.json")
        add_test(NAME "editor: the sidecar highlights every example (tauri-editor.md 5.4)"
            COMMAND editor_highlights "${HIGHLIGHTS}" "${PROJECT_SOURCE_DIR}/examples"
                    "${CMAKE_CURRENT_SOURCE_DIR}/editor/highlight")
        set_tests_properties("editor: the sidecar highlights every example (tauri-editor.md 5.4)"
            PROPERTIES FIXTURES_SETUP editorHighlights TIMEOUT ${ESPRESSO_CLI_TEST_TIMEOUT})
        configure_file(editor/highlight_test.html.in "${CMAKE_CURRENT_BINARY_DIR}/editor/highlight_test.html" @ONLY)
        add_test(NAME "editor: lisp.js highlights every example as the C++ highlighter does (tauri-editor.md 5.4)"
            COMMAND "${ESPRESSO_CHROME}" --headless=new --disable-gpu --no-first-run
                    "--user-data-dir=${PROJECT_BINARY_DIR}/chrome-profile" --allow-file-access-from-files
                    --window-size=1280,800 --virtual-time-budget=10000 --dump-dom
                    "file://${CMAKE_CURRENT_BINARY_DIR}/editor/highlight_test.html")
        set_tests_properties("editor: lisp.js highlights every example as the C++ highlighter does (tauri-editor.md 5.4)"
            PROPERTIES PASS_REGULAR_EXPRESSION ">highlight: ok<" FAIL_REGULAR_EXPRESSION ">highlight: FAIL"
            FIXTURES_REQUIRED editorHighlights TIMEOUT ${ESPRESSO_CLI_TEST_TIMEOUT} RUN_SERIAL TRUE)
    endif()
else()
    message(STATUS "Chrome not found: the browser tests of the page controls and the editor are skipped")
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
    # The sidecar on the executable's own standard input and output: a
    # hello answered, then shutdown (tauri-editor.md 4.2, 4.7).
    add_test(NAME "apps: client --serve answers on standard output"
        COMMAND sh -c "printf '%s\\n%s\\n' '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"hello\"}' '{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\"}' | \"$1\" --serve --port 1" sh $<TARGET_FILE:espresso_client>)
    set_tests_properties("apps: client --serve answers on standard output" PROPERTIES
        PASS_REGULAR_EXPRESSION "\"id\":1,\"jsonrpc\":\"2.0\",\"result\":{\"client\":\"[0-9.]+\",\"metadata\":\"[0-9.]+\",\"protocol\":1,\"server\":\"127.0.0.1:[0-9]+\"}"
        TIMEOUT ${ESPRESSO_CLI_TEST_TIMEOUT})
    set_property(GLOBAL APPEND PROPERTY ESPRESSO_TEST_TARGETS espresso_client)
    espresso_add_cli_test("client refuses an option --serve does not take" espresso_client
        "invalid argument --bogus with --serve" --serve --bogus)
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
    # The largest --threads exceeds the processors of any machine with fewer.
    cmake_host_system_information(RESULT logicalCores QUERY NUMBER_OF_LOGICAL_CORES)
    if(logicalCores LESS 64)
        espresso_add_cli_test("server warns when --threads exceeds the processors" espresso_server
            "--threads 64 exceeds the [0-9]+ processors this process may use"
            --render "${report}" --out "${renders}/oversubscribed.wav" --seconds 0.01 --threads 64)
    endif()
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
