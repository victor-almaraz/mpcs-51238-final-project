# Sphinx configuration of the espresso manual: the user guide, the language
# reference, the standard library, the IR format, and the C++ API through
# Breathe. CMake's `manual` target runs Sphinx with `-D version=` and
# `-D release=` from the project's version and with the Doxygen XML
# directory in `breathe_projects.espresso`.

project = "espresso"
author = "The espresso authors"
copyright = "the espresso authors, AGPLv3"
version = release = "0.0.0"

extensions = ["myst_parser", "breathe"]
source_suffix = {".md": "markdown"}
root_doc = "index"

# docs/api/ is Doxygen's committed HTML, and api.md its main page in
# Doxygen's Markdown dialect; badges/ holds images for README.md.
exclude_patterns = ["api", "api.md", "badges", "requirements.txt"]

# Headings up to level 3 get anchors, so `[Pages](language.md#pages)` links
# resolve to a section.
myst_heading_anchors = 3

breathe_projects = {"espresso": ""}
breathe_default_project = "espresso"
breathe_domain_by_extension = {"h": "cpp"}

html_theme = "alabaster"
html_title = "espresso manual"
html_show_sourcelink = False

# ESPRESSO_NONBLOCKING expands to `[[clang::nonblocking]]` where the compiler
# supports it (include/espresso/primitives/realtime.h); Sphinx's C++ parser
# reads it as an attribute.
cpp_id_attributes = ["ESPRESSO_NONBLOCKING"]


def setup(app):
    # The dialect's literals (`#t`, `440hz`, `:keyword`) lex as Scheme's;
    # Pygments' Common Lisp lexer refuses `#t`.
    from pygments.lexers.lisp import SchemeLexer

    app.add_lexer("lisp", SchemeLexer)
