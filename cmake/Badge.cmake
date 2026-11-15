# Writes a flat SVG badge: a grey label and a coloured value.
#
#   include(Badge)
#   proj_write_badge(<file.svg> <label> <text> <color>)
#
# Widths approximate 11 px Verdana: 7 px per character plus 10 px padding.

function(proj_write_badge output label text color)
    string(LENGTH "${label}" labelChars)
    string(LENGTH "${text}" textChars)
    math(EXPR labelWidth "${labelChars} * 7 + 10")
    math(EXPR textWidth "${textChars} * 7 + 10")
    math(EXPR width "${labelWidth} + ${textWidth}")
    math(EXPR labelX "${labelWidth} * 5")
    math(EXPR textX "(${labelWidth} + ${textWidth} / 2) * 10")
    math(EXPR labelLength "(${labelWidth} - 10) * 10")
    math(EXPR textLength "(${textWidth} - 10) * 10")

    file(WRITE "${output}" "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"${width}\" height=\"20\" role=\"img\" aria-label=\"${label}: ${text}\">
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
endfunction()
