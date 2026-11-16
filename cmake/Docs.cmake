# The docs target builds the API reference with Doxygen: HTML into
# docs/api/, which is committed, with docs/api.md as its main page; XML,
# for Sphinx and Breathe once docs/ has Sphinx sources, into the build
# tree. The pages carry no timestamp, so regenerating unchanged headers
# changes no file.

find_package(Doxygen)

if(DOXYGEN_FOUND)
    set(DOXYGEN_PROJECT_NAME "${PROJECT_NAME}")
    set(DOXYGEN_PROJECT_NUMBER "${PROJECT_VERSION}")
    set(DOXYGEN_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/docs/doxygen")
    set(DOXYGEN_HTML_OUTPUT "${PROJECT_SOURCE_DIR}/docs/api")
    set(DOXYGEN_GENERATE_XML YES)
    set(DOXYGEN_EXTRACT_ALL NO)
    # A comment's first sentence is its summary, also in /// comments of
    # more than one line.
    set(DOXYGEN_JAVADOC_AUTOBRIEF YES)
    set(DOXYGEN_WARN_IF_UNDOCUMENTED YES)
    set(DOXYGEN_RECURSIVE YES)
    set(DOXYGEN_TIMESTAMP NO)
    set(DOXYGEN_GENERATE_TREEVIEW YES)
    set(DOXYGEN_FULL_PATH_NAMES YES)
    set(DOXYGEN_STRIP_FROM_PATH "${PROJECT_SOURCE_DIR}")
    set(DOXYGEN_STRIP_FROM_INC_PATH "${PROJECT_SOURCE_DIR}/include")
    set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "${PROJECT_SOURCE_DIR}/docs/api.md")
    doxygen_add_docs(docs "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/docs/api.md"
        COMMENT "Building the API reference into docs/api with Doxygen")
else()
    add_custom_target(docs
        COMMAND ${CMAKE_COMMAND} -E echo "Doxygen not found; docs target skipped"
        VERBATIM)
endif()
