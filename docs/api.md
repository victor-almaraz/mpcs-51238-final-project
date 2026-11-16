# API reference {#mainpage}

The API reference documents the public C++ headers in `include/proj/`, one namespace per module. The HTML reference is generated into `docs/api/` by the `docs` build target; `docs/api/index.html` is its entry page.

| Namespace | Module | Contents |
|---|---|---|
| `metadata` | `metadata` | Primitive parameter tables, graph description types, and the metadata version shared by client and server |
| `ir` | `ir` | IR JSON reading and writing, schema and semantic validation, data-flow sorting, and hazard analysis |
| `protocol` | `protocol` | OSC encoding and decoding, the client-server message set, IR chunking, and retries |
| `primitives` | `primitives` | Sources, sinks, generators, pipes, the pipe table, frames, plans, and the engine |
| `interpreter` | `interpreter` | The lexer, reader, and compiler of the Lisp dialect |
| `tui` | `tui` | The terminal editor: buffers, key schemes, settings, the console, and the link to the interpreter and server |

The language is described in `docs/language.md`, its standard library in `docs/library.md`, and the IR format in `docs/ir.md`.

The reference is regenerated with:

```bash
cmake --build build --target docs
```
