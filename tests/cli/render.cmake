# Renders an IR file offline with espresso-server and checks what it writes:
# a WAV file with samples after its header.
#
#   cmake -DSERVER=<espresso-server> -DIR=<file.ir.json> -DOUT=<file.wav> -P render.cmake

get_filename_component(outDir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${outDir}")
file(REMOVE "${OUT}")
execute_process(
    COMMAND "${SERVER}" --render "${IR}" --out "${OUT}" --seconds 0.25
    RESULT_VARIABLE status
    ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "espresso-server --render exited with ${status}: ${errors}")
endif()
file(SIZE "${OUT}" bytes)
# 0.25 s at 48 kHz, 2 channels of 4 bytes, after a 58-byte header.
if(bytes LESS 96000)
    message(FATAL_ERROR "${OUT} has ${bytes} bytes; expected the samples of 0.25 s")
endif()
message(STATUS "rendered ${OUT}: ${bytes} bytes")
