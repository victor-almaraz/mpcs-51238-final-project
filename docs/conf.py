# Sphinx configuration of the espresso manual: the user guide, the
# tutorials, the language reference, the standard library, the primitive
# reference, the IR format, the C++ API through Breathe, and the resources. CMake's `manual` target runs Sphinx with `-D version=` and
# `-D release=` from the project's version and with the Doxygen XML
# directory in `breathe_projects.espresso`.

project = "espresso"
author = "The espresso authors"
copyright = "the espresso authors, AGPLv3"
version = release = "0.0.0"

extensions = ["myst_parser", "breathe"]
source_suffix = {".md": "markdown"}
root_doc = "index"

# api.md is Doxygen's main page, in Doxygen's Markdown dialect; badges/
# holds images for README.md.
exclude_patterns = ["api.md", "badges", "requirements.txt"]

# Headings up to level 3 get anchors, so `[Pages](language.md#pages)` links
# resolve to a section.
myst_heading_anchors = 3

breathe_projects = {"espresso": ""}
breathe_default_project = "espresso"
breathe_domain_by_extension = {"h": "cpp"}

html_theme = "alabaster"
html_title = "espresso manual"
html_show_sourcelink = False


def setup(app):
    # The dialect's literals (`#t`, `440hz`, `:keyword`) lex as Scheme's;
    # Pygments' Common Lisp lexer refuses `#t`.
    from pygments.lexers.lisp import SchemeLexer

    app.add_lexer("lisp", SchemeLexer)
