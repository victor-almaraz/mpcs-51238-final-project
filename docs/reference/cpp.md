# C++ API

The C++ API is the public headers in `include/espresso/`: one namespace per module, and namespace `espresso` for the project-wide thread type. Each section below lists one namespace's declarations, drawn from the headers' Doxygen comments.

## espresso

`espresso::jthread`, the self-joining thread type of every thread in the project, its stop token, and the stack size of every thread, declared in `include/espresso/protocol/jthread.h`.

```{doxygennamespace} espresso
:members:
:undoc-members:
```

## metadata

Primitive parameter tables, page layout kinds, graph description types, and the metadata version shared by client and server.

```{doxygennamespace} metadata
:members:
:undoc-members:
```

## ir

IR JSON reading and writing, schema and semantic validation, data-flow sorting, and hazard analysis.

```{doxygennamespace} ir
:members:
:undoc-members:
```

## page

A page's layout drawn as the HTML the server serves when a project has no `ui/index.html`, and that the client's `--page` writes.

```{doxygennamespace} page
:members:
:undoc-members:
```

## protocol

OSC encoding and decoding, the client-server message set, IR chunking, and retries.

```{doxygennamespace} protocol
:members:
:undoc-members:
```

## primitives

Sources, sinks, generators, pipes, the pipe table, frames, plans, and the engine.

```{doxygennamespace} primitives
:members:
:undoc-members:
```

## interpreter

The Lisp dialect's lexer and reader, the compiler from a source file to a validated graph, the project file (`project.lisp`) and the entry file a project plays, and the conversion of unit literals to canonical units.

```{doxygennamespace} interpreter
:members:
:undoc-members:
```

## workspace

The editor's core without a screen: files, settings, highlighting, the console's log, and the link to the interpreter and server.

```{doxygennamespace} workspace
:members:
:undoc-members:
```

## tui

The terminal editor: buffers, key schemes, themes, and the screen.

```{doxygennamespace} tui
:members:
:undoc-members:
```
