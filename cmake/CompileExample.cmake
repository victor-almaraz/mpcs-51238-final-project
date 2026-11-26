# Writes the IR of a piece to a file, as `espresso-client --compile PIECE
# --root ROOT > OUT` does, for tests that give the server an IR file.
# Usage: cmake -DCLIENT=... -DPIECE=... -DROOT=... -DOUT=... -P CompileExample.cmake
foreach(variable CLIENT PIECE ROOT OUT)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "CompileExample.cmake needs -D${variable}=...")
    endif()
endforeach()
get_filename_component(directory "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${directory}")
execute_process(COMMAND "${CLIENT}" --compile "${PIECE}" --root "${ROOT}"
    OUTPUT_FILE "${OUT}" ERROR_VARIABLE errors RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    file(REMOVE "${OUT}")
    message(FATAL_ERROR "${PIECE} did not compile: ${errors}")
endif()
message(STATUS "compiled ${PIECE} to ${OUT}")
