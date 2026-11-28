# API reference {#mainpage}

The API reference documents the public C++ headers in `include/espresso/`. Each module's declarations are in a namespace named after the module. `espresso::jthread`, the self-joining thread type of every thread in the project, is in namespace `espresso`, declared in `include/espresso/protocol/jthread.h`. The HTML reference is generated into `docs/api/` by the `docs` build target; `docs/api/index.html` is its entry page.

| Namespace | Contents |
|---|---|
| `metadata` | Primitive parameter tables, page layout kinds, graph description types, and the metadata version shared by client and server |
| `ir` | IR JSON reading and writing, schema and semantic validation, data-flow sorting, and hazard analysis |
| `page` | A page's layout drawn as the HTML the server serves when a project has no `ui/index.html`, and that the client's `--page` writes |
| `protocol` | OSC encoding and decoding, the client-server message set, IR chunking, and retries |
| `primitives` | Sources, sinks, generators, pipes, the pipe table, frames, plans, and the engine |
| `interpreter` | The Lisp dialect's lexer and reader, the compiler from a source file to a validated graph, the project file (`project.lisp`) and the entry file a project plays, and the conversion of unit literals to canonical units |
| `workspace` | The editor's core without a screen: files, settings, highlighting, the console's log, and the link to the interpreter and server |
| `tui` | The terminal editor: buffers, key schemes, themes, and the screen |

The language is described in `docs/language.md`, its standard library in `docs/library.md`, and the IR format in `docs/ir.md`.

The reference is regenerated with:

```bash
cmake --build build --target docs
```
