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

execute_process(
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${BUILD_DIR}" --output-on-failure
    OUTPUT_VARIABLE log
    ECHO_OUTPUT_VARIABLE
    RESULT_VARIABLE status)

# ctest summarizes as "100% tests passed out of N" or
# "P% tests passed, F tests failed out of N".
if(log MATCHES "tests passed out of ([0-9]+)")
    set(failed 0)
    set(total ${CMAKE_MATCH_1})
elseif(log MATCHES "tests passed, ([0-9]+) tests? failed out of ([0-9]+)")
    set(failed ${CMAKE_MATCH_1})
    set(total ${CMAKE_MATCH_2})
else()
    message(FATAL_ERROR "TestBadge.cmake: no test summary in the ctest output")
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
