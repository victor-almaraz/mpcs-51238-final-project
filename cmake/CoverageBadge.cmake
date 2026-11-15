# Runs the test suite in a coverage build and writes an SVG badge with the
# line coverage of the project's own sources (src/ and include/).
#
#   cmake -DBUILD_DIR=<build> -DOUTPUT=<file.svg> -DLLVM_PROFDATA=<path>
#         -DLLVM_COV=<path> -DOBJECTS=<-object=exe|-object=exe|...>
#         -P CoverageBadge.cmake
#
# BUILD_DIR is a tree configured with PROJ_COVERAGE=ON. OBJECTS are
# llvm-cov's -object arguments for the instrumented executables,
# separated by '|'. The badge reads "N.N%":
# green from 80%, yellow from 60%, red below. The script fails, without
# writing the badge, when any test fails.

include("${CMAKE_CURRENT_LIST_DIR}/Badge.cmake")
proj_badge_require(CoverageBadge.cmake BUILD_DIR OUTPUT LLVM_PROFDATA LLVM_COV OBJECTS)

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
# Only the project's sources count: not dependencies, tests, or system
# and toolchain headers.
execute_process(
    COMMAND "${LLVM_COV}" export -summary-only "-instr-profile=${merged}" ${objects}
            "-ignore-filename-regex=(/third_party/|/_deps/|/tests/|^/usr/|^/Library/|^/Applications/)"
    OUTPUT_VARIABLE report
    COMMAND_ERROR_IS_FATAL ANY)
string(JSON percent GET "${report}" data 0 totals lines percent)

# One decimal, truncated, so the badge never rounds up past a threshold.
string(REGEX MATCH "^[0-9]+(\\.[0-9])?" text "${percent}")
if(percent GREATER_EQUAL 80)
    set(color "${PROJ_BADGE_GREEN}")
elseif(percent GREATER_EQUAL 60)
    set(color "${PROJ_BADGE_YELLOW}")
else()
    set(color "${PROJ_BADGE_RED}")
endif()

proj_write_badge("${OUTPUT}" "coverage" "${text}%" "${color}")
