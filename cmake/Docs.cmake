# Two documentation targets:
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
#   site/, the editor's fonts, and the manual under docs/.
#
# Sphinx and its extensions are tools, not dependencies: install the
# versions docs/requirements.txt pins, and point SPHINX_EXECUTABLE at their
# sphinx-build when it is not on the PATH.

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
    add_custom_target(docs
        COMMAND ${CMAKE_COMMAND} -E echo "Doxygen not found; docs target skipped"
        VERBATIM)
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
else()
    add_custom_target(manual
        COMMAND ${CMAKE_COMMAND} -E echo "Doxygen or sphinx-build not found; manual target skipped"
        VERBATIM)
endif()

# The web site: site/'s landing page at the root, the GUI editor's fonts
# (fetched with the editor's web packages) beside it, and the manual under
# docs/.
if(TARGET manual-xml)
    set(ESPRESSO_SITE_DIR "${PROJECT_BINARY_DIR}/site")
    set(ESPRESSO_EDITOR_FONTS "${PROJECT_SOURCE_DIR}/src/apps/editor/web/vendor/fonts")
    add_custom_target(site
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${ESPRESSO_SITE_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/site" "${ESPRESSO_SITE_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_BINARY_DIR}/docs/html" "${ESPRESSO_SITE_DIR}/docs"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${ESPRESSO_SITE_DIR}/fonts"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ESPRESSO_EDITOR_FONTS}/NotoSans.ttf"
                "${ESPRESSO_EDITOR_FONTS}/NotoSans-OFL.txt" "${ESPRESSO_EDITOR_FONTS}/JetBrainsMono.ttf"
                "${ESPRESSO_EDITOR_FONTS}/JetBrainsMono-OFL.txt" "${ESPRESSO_SITE_DIR}/fonts"
        COMMENT "Assembling the web site in ${ESPRESSO_SITE_DIR}"
        VERBATIM)
    add_dependencies(site manual)
else()
    add_custom_target(site
        COMMAND ${CMAKE_COMMAND} -E echo "Doxygen or sphinx-build not found; site target skipped"
        VERBATIM)
endif()
