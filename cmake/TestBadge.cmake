# Runs the test suite and writes an SVG badge with the result.
#
#   cmake -DBUILD_DIR=<build> -DOUTPUT=<file.svg> -P TestBadge.cmake
#
# The badge reads "P of N passed": green when every test passed, red
# otherwise; tests that were skipped or did not run are not counted as
# passed. The script fails when any test fails, after writing the badge.

include("${CMAKE_CURRENT_LIST_DIR}/Badge.cmake")
espresso_badge_require(TestBadge.cmake BUILD_DIR OUTPUT)

# The counts come from a JUnit report, not from the console summary,
# whose wording and colour codes (CLICOLOR_FORCE) vary.
set(report "${BUILD_DIR}/Testing/test-badge.xml")
file(REMOVE "${report}")
execute_process(
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${BUILD_DIR}" --output-on-failure --output-junit "${report}"
    RESULT_VARIABLE status)

if(NOT EXISTS "${report}")
    message(FATAL_ERROR "TestBadge.cmake: ctest wrote no report to ${report}")
endif()
file(READ "${report}" junit)
# <testsuite ... tests="N" failures="F" disabled="D" skipped="S" ...>;
# tests that could not run (a missing executable) count as skipped.
if(NOT junit MATCHES "<testsuite[^>]*[ \t\r\n]tests=\"([0-9]+)\"[ \t\r\n]+failures=\"([0-9]+)\"[ \t\r\n]+disabled=\"[0-9]+\"[ \t\r\n]+skipped=\"([0-9]+)\"")
    message(FATAL_ERROR "TestBadge.cmake: no test counts in ${report}")
endif()
set(total ${CMAKE_MATCH_1})
math(EXPR passed "${total} - ${CMAKE_MATCH_2} - ${CMAKE_MATCH_3}")

if(status EQUAL 0 AND passed EQUAL total)
    set(text "${passed} of ${total} passed")
    set(color "${ESPRESSO_BADGE_GREEN}")
elseif(passed EQUAL total)
    set(text "ctest failed")  # without failing a test; never report a pass
    set(color "${ESPRESSO_BADGE_RED}")
else()
    set(text "${passed} of ${total} passed")
    set(color "${ESPRESSO_BADGE_RED}")
endif()

espresso_write_badge("${OUTPUT}" "tests" "${text}" "${color}")

if(NOT status EQUAL 0)
    message(FATAL_ERROR "${text}; badge written to ${OUTPUT}")
endif()
