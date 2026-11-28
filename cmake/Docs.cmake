# Three documentation targets:
#
# - `docs` builds the API reference with Doxygen: HTML into
#   build/docs/api/, which is not committed, with docs/api.md as its main
#   page.
# - `manual` builds the manual with Sphinx, MyST, and Breathe: the Markdown
#   of docs/ and the C++ API from Doxygen XML, as HTML into
#   build/docs/html/, which is not committed. The XML comes from a Doxygen
#   run of its own that expands ESPRESSO_NONBLOCKING to nothing, as Sphinx's
#   C++ parser does not accept an attribute before `override`. The build
#   fails on any warning.
#
# - `site` assembles the web site in build/site/: the landing page of
#   site/ and the manual under docs/.
#
# Sphinx and its extensions are tools, not dependencies: install the
# versions docs/requirements.txt pins, and point SPHINX_EXECUTABLE at their
# sphinx-build when it is not on the PATH.

# A target that only reports why it does nothing.
function(espresso_skipped_target name reason)
    add_custom_target(${name} COMMAND ${CMAKE_COMMAND} -E echo "${reason}; ${name} target skipped" VERBATIM)
endfunction()

find_package(Doxygen)
find_program(SPHINX_EXECUTABLE NAMES sphinx-build DOC "sphinx-build with myst-parser and breathe installed")

if(DOXYGEN_FOUND)
    set(DOXYGEN_PROJECT_NAME "${PROJECT_NAME}")
    set(DOXYGEN_PROJECT_NUMBER "${PROJECT_VERSION}")
    set(DOXYGEN_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/docs/doxygen")
    set(DOXYGEN_HTML_OUTPUT "${PROJECT_BINARY_DIR}/docs/api")
    set(DOXYGEN_EXTRACT_ALL NO)
    # A comment's first sentence is its summary, also in /// comments of
    # more than one line.
    set(DOXYGEN_JAVADOC_AUTOBRIEF YES)
    set(DOXYGEN_WARN_IF_UNDOCUMENTED YES)
    set(DOXYGEN_RECURSIVE YES)
    set(DOXYGEN_TIMESTAMP NO)
    set(DOXYGEN_GENERATE_TREEVIEW YES)
    # The source pages show each header as written, documentation included.
    set(DOXYGEN_STRIP_CODE_COMMENTS NO)
    set(DOXYGEN_FULL_PATH_NAMES YES)
    set(DOXYGEN_STRIP_FROM_PATH "${PROJECT_SOURCE_DIR}")
    set(DOXYGEN_STRIP_FROM_INC_PATH "${PROJECT_SOURCE_DIR}/include")
    set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "${PROJECT_SOURCE_DIR}/docs/api.md")
    doxygen_add_docs(docs "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/docs/api.md"
        COMMENT "Building the API reference into ${PROJECT_BINARY_DIR}/docs/api with Doxygen")
else()
    espresso_skipped_target(docs "Doxygen not found")
endif()

if(DOXYGEN_FOUND AND SPHINX_EXECUTABLE)
    set(DOXYGEN_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/docs/doxygen-xml")
    set(DOXYGEN_GENERATE_HTML NO)
    set(DOXYGEN_GENERATE_XML YES)
    set(DOXYGEN_WARN_IF_UNDOCUMENTED NO)
    set(DOXYGEN_QUIET YES)
    set(DOXYGEN_MACRO_EXPANSION YES)
    set(DOXYGEN_EXPAND_ONLY_PREDEF YES)
    set(DOXYGEN_PREDEFINED "ESPRESSO_NONBLOCKING=")
    set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "")
    doxygen_add_docs(manual-xml "${PROJECT_SOURCE_DIR}/include"
        COMMENT "Building the C++ API's XML for the manual with Doxygen")

    add_custom_target(manual
        COMMAND "${SPHINX_EXECUTABLE}" -b html -W --keep-going -q
                -D "version=${PROJECT_VERSION}" -D "release=${PROJECT_VERSION}"
                -D "breathe_projects.espresso=${PROJECT_BINARY_DIR}/docs/doxygen-xml/xml"
                -d "${PROJECT_BINARY_DIR}/docs/doctrees"
                "${PROJECT_SOURCE_DIR}/docs" "${PROJECT_BINARY_DIR}/docs/html"
        COMMENT "Building the manual into ${PROJECT_BINARY_DIR}/docs/html with Sphinx"
        VERBATIM)
    add_dependencies(manual manual-xml)

    # The web site: site/'s landing page at the root, the manual under docs/.
    set(site "${PROJECT_BINARY_DIR}/site")
    add_custom_target(site
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${site}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/site" "${site}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_BINARY_DIR}/docs/html" "${site}/docs"
        COMMENT "Assembling the web site in ${site}"
        VERBATIM)
    add_dependencies(site manual)
else()
    espresso_skipped_target(manual "Doxygen or sphinx-build not found")
    espresso_skipped_target(site "Doxygen or sphinx-build not found")
endif()
