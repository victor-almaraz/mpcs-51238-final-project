# The GUI editor

The GUI editor is a desktop application built with Tauri: a web interface in the system's webview, over the client run as its sidecar. The sidecar reads and writes the project's files, compiles it, and talks to the server.

## The sidecar

```bash
build/bin/espresso-client --serve [--port N]
```

Runs the client without the terminal editor, for a graphical editor that starts it as a child process. The client reads JSON-RPC 2.0 requests from standard input and writes responses and notifications to standard output, one message per line, and reaches the server on `127.0.0.1:N` as the terminal editor does. The methods are `hello`, `project.open`, `project.files`, `file.read`, `file.stat`, `file.write`, `file.create` (an empty file, and the folders above it), `file.rename`, `run.play`, `run.eval`, `run.stop`, `server.connect`, `page.render`, `highlight`, `language`, and `shutdown`; the notifications are `console` and `status`. Paths are relative to the project root that `project.open` names. After `shutdown`, or at the end of standard input, the client finishes the run requests already read and exits with status 0.
