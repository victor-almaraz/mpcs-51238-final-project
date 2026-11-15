# Runs the test suite and writes an SVG badge with the result.
#
#   cmake -DBUILD_DIR=<build> -DOUTPUT=<file.svg> -P TestBadge.cmake
#
# The badge reads "N passed" (green) or "F of N failed" (red). The script
# fails when any test fails, after writing the badge.

foreach(var BUILD_DIR OUTPUT)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "TestBadge.cmake: ${var} is not set")
    endif()
endforeach()

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
set(failed ${CMAKE_MATCH_2})
if(NOT status EQUAL 0)
    math(EXPR failed "${failed} + ${CMAKE_MATCH_3}")  # ctest failed: skipped tests did not run
    if(failed EQUAL 0)
        set(failed 1)  # ctest failed without failing a test; never report a pass
    endif()
endif()

if(failed EQUAL 0)
    set(text "${total} passed")
    set(color "#4c1")
else()
    set(text "${failed} of ${total} failed")
    set(color "#e05d44")
endif()

# Widths approximate 11 px Verdana: 7 px per character plus 10 px padding.
set(label "tests")
string(LENGTH "${label}" labelChars)
string(LENGTH "${text}" textChars)
math(EXPR labelWidth "${labelChars} * 7 + 10")
math(EXPR textWidth "${textChars} * 7 + 10")
math(EXPR width "${labelWidth} + ${textWidth}")
math(EXPR labelX "${labelWidth} * 5")
math(EXPR textX "(${labelWidth} + ${textWidth} / 2) * 10")
math(EXPR labelLength "(${labelWidth} - 10) * 10")
math(EXPR textLength "(${textWidth} - 10) * 10")

file(WRITE "${OUTPUT}" "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"${width}\" height=\"20\" role=\"img\" aria-label=\"${label}: ${text}\">
<title>${label}: ${text}</title>
<linearGradient id=\"s\" x2=\"0\" y2=\"100%\"><stop offset=\"0\" stop-color=\"#bbb\" stop-opacity=\".1\"/><stop offset=\"1\" stop-opacity=\".1\"/></linearGradient>
<clipPath id=\"r\"><rect width=\"${width}\" height=\"20\" rx=\"3\" fill=\"#fff\"/></clipPath>
<g clip-path=\"url(#r)\"><rect width=\"${labelWidth}\" height=\"20\" fill=\"#555\"/><rect x=\"${labelWidth}\" width=\"${textWidth}\" height=\"20\" fill=\"${color}\"/><rect width=\"${width}\" height=\"20\" fill=\"url(#s)\"/></g>
<g fill=\"#fff\" text-anchor=\"middle\" font-family=\"Verdana,Geneva,DejaVu Sans,sans-serif\" font-size=\"110\">
<text x=\"${labelX}\" y=\"150\" fill=\"#010101\" fill-opacity=\".3\" transform=\"scale(.1)\" textLength=\"${labelLength}\">${label}</text><text x=\"${labelX}\" y=\"140\" transform=\"scale(.1)\" textLength=\"${labelLength}\">${label}</text>
<text x=\"${textX}\" y=\"150\" fill=\"#010101\" fill-opacity=\".3\" transform=\"scale(.1)\" textLength=\"${textLength}\">${text}</text><text x=\"${textX}\" y=\"140\" transform=\"scale(.1)\" textLength=\"${textLength}\">${text}</text>
</g>
</svg>
")

if(NOT status EQUAL 0)
    message(FATAL_ERROR "${text}; badge written to ${OUTPUT}")
endif()
