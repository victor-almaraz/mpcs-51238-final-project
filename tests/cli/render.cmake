# Renders an IR file offline with proj-server and checks what it writes:
# a WAV file with samples after its header and, with FRAMES_DIR, the
# first PNG frame.
#
#   cmake -DSERVER=<proj-server> -DIR=<file.ir.json> -DOUT=<file.wav>
#         [-DFRAMES_DIR=<dir>] -P render.cmake

get_filename_component(outDir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${outDir}")
file(REMOVE "${OUT}")
set(frameArgs "")
if(FRAMES_DIR)
    file(REMOVE_RECURSE "${FRAMES_DIR}")
    set(frameArgs --frames "${FRAMES_DIR}" --fps 10 --width 64 --height 36)
endif()
execute_process(
    COMMAND "${SERVER}" --render "${IR}" --out "${OUT}" --seconds 0.25 ${frameArgs}
    RESULT_VARIABLE status
    ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "proj-server --render exited with ${status}: ${errors}")
endif()
file(SIZE "${OUT}" bytes)
# 0.25 s at 48 kHz, 2 channels of 4 bytes, after a 58-byte header.
if(bytes LESS 96000)
    message(FATAL_ERROR "${OUT} has ${bytes} bytes; expected the samples of 0.25 s")
endif()
if(FRAMES_DIR AND NOT EXISTS "${FRAMES_DIR}/frame-00000.png")
    message(FATAL_ERROR "no frame-00000.png in ${FRAMES_DIR}")
endif()
message(STATUS "rendered ${OUT}: ${bytes} bytes")
