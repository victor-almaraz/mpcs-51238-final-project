// The client's sidecar mode, `espresso-client --serve` (tauri-editor.md
// §4): an editor's requests and their answers as JSON-RPC 2.0 messages,
// one a line, with no terminal.

#pragma once

#include <istream>
#include <ostream>

#include <espresso/workspace/interpreterLink.h>

namespace client {

/// What the sidecar is started with.
struct serveOptions {
    /// How its link reaches the server.
    workspace::linkOptions link;
};

/// Answers the requests read from @p in, writing responses and
/// notifications to @p out, one message a line (tauri-editor.md §4.2),
/// until `shutdown` or the end of @p in. Then waits for the run requests
/// already read, writes what they produce, and gives the exit status, 0.
int serve(std::istream& in, std::ostream& out, serveOptions options);

}  // namespace client
