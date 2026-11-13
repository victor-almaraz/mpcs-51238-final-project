# The docs target builds the API reference with Doxygen. Sphinx and
# Breathe build the user guide and language reference once docs/ has
# Sphinx sources.

find_package(Doxygen)

if(DOXYGEN_FOUND)
    set(DOXYGEN_PROJECT_NAME "${PROJECT_NAME}")
    set(DOXYGEN_PROJECT_NUMBER "${PROJECT_VERSION}")
    set(DOXYGEN_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/docs/doxygen")
    set(DOXYGEN_GENERATE_XML YES)
    set(DOXYGEN_EXTRACT_ALL NO)
    set(DOXYGEN_WARN_IF_UNDOCUMENTED YES)
    set(DOXYGEN_RECURSIVE YES)
    set(DOXYGEN_STRIP_FROM_INC_PATH "${PROJECT_SOURCE_DIR}/include")
    doxygen_add_docs(docs "${PROJECT_SOURCE_DIR}/include"
        COMMENT "Building API reference with Doxygen")
else()
    add_custom_target(docs
        COMMAND ${CMAKE_COMMAND} -E echo "Doxygen not found; docs target skipped"
        VERBATIM)
endif()
