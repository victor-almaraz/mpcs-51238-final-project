# Runs the test suite in a coverage build and writes an SVG badge with the
# line coverage of the project's own sources (src/ and include/).
#
#   cmake -DBUILD_DIR=<build> -DOUTPUT=<file.svg> -DLLVM_PROFDATA=<path>
#         -DLLVM_COV=<path> -DOBJECTS=<exe|exe|...> -P CoverageBadge.cmake
#
# BUILD_DIR is a tree configured with PROJ_COVERAGE=ON. OBJECTS are the
# instrumented executables, separated by '|'. The badge reads "N.N%":
# green from 80%, yellow from 60%, red below. The script fails, without
# writing the badge, when any test fails.

foreach(var BUILD_DIR OUTPUT LLVM_PROFDATA LLVM_COV OBJECTS)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "CoverageBadge.cmake: ${var} is not set")
    endif()
endforeach()

# One raw profile per process: %p (process ID) keeps parallel and child
# processes apart, %m the binary.
set(profiles "${BUILD_DIR}/coverage")
file(REMOVE_RECURSE "${profiles}")
file(MAKE_DIRECTORY "${profiles}")
set(ENV{LLVM_PROFILE_FILE} "${profiles}/%p-%m.profraw")
execute_process(
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${BUILD_DIR}" --output-on-failure
    RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "CoverageBadge.cmake: the test suite failed; ${OUTPUT} is unchanged")
endif()

file(GLOB raw "${profiles}/*.profraw")
if(NOT raw)
    message(FATAL_ERROR "CoverageBadge.cmake: no profiles in ${profiles}; is the tree built with PROJ_COVERAGE=ON?")
endif()
set(merged "${profiles}/merged.profdata")
execute_process(
    COMMAND "${LLVM_PROFDATA}" merge -sparse ${raw} -o "${merged}"
    COMMAND_ERROR_IS_FATAL ANY)

string(REPLACE "|" ";" objects "${OBJECTS}")
list(POP_FRONT objects first)
set(more)
foreach(object IN LISTS objects)
    list(APPEND more -object "${object}")
endforeach()
# Only the project's sources count: not dependencies, tests, or system
# and toolchain headers.
execute_process(
    COMMAND "${LLVM_COV}" export -summary-only "-instr-profile=${merged}" "${first}" ${more}
            "-ignore-filename-regex=(/third_party/|/_deps/|/tests/|^/usr/|^/Library/|^/Applications/)"
    OUTPUT_VARIABLE report
    COMMAND_ERROR_IS_FATAL ANY)
string(JSON percent GET "${report}" data 0 totals lines percent)

# One decimal, truncated, so the badge never rounds up past a threshold.
string(REGEX MATCH "^[0-9]+(\\.[0-9])?" text "${percent}")
string(REGEX MATCH "^[0-9]+" whole "${percent}")
if(whole GREATER_EQUAL 80)
    set(color "#4c1")
elseif(whole GREATER_EQUAL 60)
    set(color "#dfb317")
else()
    set(color "#e05d44")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/Badge.cmake")
proj_write_badge("${OUTPUT}" "coverage" "${text}%" "${color}")
