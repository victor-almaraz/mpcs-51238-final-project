# AGENTS.md

Instructions for agents working in this repository.

## Project

espresso is a Linux-first workstation for acousmatic, aleatory, and automatic music, and the Lisp dialect its pieces are written in. A project is a collection of files that compiles to an intermediate representation (IR). A native C++ engine plays the IR.

The core is the `primitives` module. Every primitive is a node in the IR graph:

| Primitive | Role | Examples |
|---|---|---|
| Source | Produces data | MIDI, OSC, web data, serial devices, scripts that generate triggers or synthetic data |
| Sink | Receives data | Audio, MIDI, OSC, web data, serial devices |
| Generator | Takes parameters and produces a signal | Oscillators, filters, envelopes, mappers |
| Pipe | Connects primitives | `aPipe`, `cPipe`, `ePipe` |

The engine treats every source and every sink the same way. Any source can drive any sink.

A companion web site holds the documentation, tutorials, and a directory of live data APIs. It has no accounts.

## Scope

v1 (November 17) establishes the core and validates its behavior. v2 (December 1) adds performance work and the remaining source and sink types.

| Version | Contents |
|---|---|
| v1 | Single-threaded engine, interpreter and compiler, MIDI and OSC sources, TUI editor |
| v2 | Multithreaded engine, web data and serial sources and sinks, MIDI and OSC sinks, data directory, AppImage release |

Out of scope:

- Hosting third-party plugins
- Windows and macOS
- Visual output: images, video, and drawing. Front ends are web pages the server serves (`io.md` §12)
- Visual editing
- Accounts, in the native app or on the web site
- Cloud sync of projects

## Milestones

Complete the milestones in order.

| Milestone | Deliverable |
|---|---|
| M1 | Mouse-first TUI editor: syntax highlighting, files and buffers, project file list, search, undo and redo, console, run controls, and selectable key schemes (Emacs, CUA, Vim, nano) |
| M2 | Audio primitives that support audio, OSC, and MIDI input and output |
| M3 | Image primitives: a Sketching subset (`primitives.md` §7.7). Removed on the `refocus` branch: visual output is out of scope |
| M4 | Composition of primitives, tested by implementing basic audio synthesis and effects from primitives |
| M5 | Sample playback, buffers, and FFT: playback of audio files (`samplePlayer`, `primitives.md` §7.6), buffers that primitives write and read, and FFT primitives (`samples.md`) |
| M6 | Serial and web endpoint I/O: sources and sinks for serial devices, HTTP polling, server-sent event and WebSocket streams, HTTP requests on demand, and HTML and JavaScript front-ends for instruments that speak OSC over HTTP and WebSocket (`io.md`) |
| M7 | Multithreading: the multithreaded engine |
| M8 | Human-readable documentation: the user guide and language reference in `docs/` |
| M9 | The web site: documentation, tutorials, and the directory of live data APIs |
| M10 | Quality-of-life improvements and polish |

## Stack

| Component | Use |
|---|---|
| C++20 | Engine, interpreter, and application |
| JUCE (AGPLv3) | Audio devices and MIDI. Server app only |
| FTXUI (MIT) | TUI editor. Syntax highlighting is a custom component that renders tokens from the Lisp reader |
| Tauri 2 (MIT or Apache-2.0) | GUI editor: a window over the system's webview, which starts the client as its sidecar (`src/apps/editor/`) |
| CodeMirror 6 (MIT) | The GUI editor's text editor, loaded as ES modules through an import map, with Replit's Vim and Emacs keymaps (MIT) |
| marked (MIT) | The GUI editor's references: `docs/language.md` and `docs/library.md` drawn as HTML |
| CMake | Build system. Dependencies through `FetchContent` |
| nlohmann/json, json-schema-validator | IR reading, writing, and schema validation |
| PFFFT (BSD) | The FFT behind `fft`, `ifft`, and the spectral primitives in `primitives` |
| dr_wav, dr_flac, dr_mp3 (public domain or MIT-0), stb_vorbis (public domain) | Audio files for buffers: WAV, AIFF, FLAC, MP3, and Ogg Vorbis. Server app only |
| libcurl (curl license) | HTTP, HTTPS, and WebSocket transfers for web sources and sinks, with TLS from the system's OpenSSL. Server app only |
| Catch2 with CTest | Unit and integration tests |
| Valgrind | Memcheck on the test suite and offline renders |
| PipeWire | Target audio server; the app connects as a JACK client |
| Doxygen, Sphinx, Breathe | API reference, user guide, and language reference |
| Vercel, Supabase | Web site hosting and the directory's database |

The project is licensed under AGPLv3 to match JUCE.

## Repository layout

```text
.
├── CMakeLists.txt
├── cmake/                    dependency pins, helper functions
├── include/espresso/
│   ├── primitives/
│   ├── interpreter/
│   ├── workspace/
│   ├── tui/
│   ├── ir/
│   ├── metadata/
│   ├── page/
│   └── protocol/
├── src/
│   ├── primitives/  interpreter/  workspace/  tui/  ir/  metadata/  page/  protocol/
│   └── apps/
│       ├── server/          main.cpp (control loop) and devices.* (JUCE); engineHost.*,
│       │                    offline.*, options.*, oscBridge.*, samples.* (no JUCE, tested)
│       ├── client/          main.cpp (the editor's loop); commandLine.* and serve.*
│       │                    (the sidecar, `--serve`) (tested)
│       └── editor/          the GUI editor: web/ (ES modules) and src-tauri/ (Tauri's Rust)
├── schema/ir.schema.json
├── examples/                 one folder per piece: its .lisp files, samples, and page
├── lib/std/                  standard library (.lisp), embedded in the interpreter
├── tests/                    one folder per module, plus server/, client/, and network/
├── docs/                     human documentation
├── local-docs/               agent specs and research (untracked)
├── third_party/              downloaded dependencies (untracked)
└── AGENTS.md
```

- Headers live in `include/espresso/<module>/` and sources in `src/<module>/`. Include headers by project path: `#include <espresso/primitives/pipe.h>`.

### Modules

| Module | Job |
|---|---|
| `primitives` | Sources, sinks, generators, pipes, `pipeTable`, per-block execution |
| `interpreter` | Reader, evaluator, object system, units, graph building, IR emission |
| `workspace` | The editor's core without a screen: files, settings, highlighting, the console's log, and the link to the interpreter and server |
| `tui` | The terminal editor |
| `ir` | IR JSON reading and writing, schema and semantic validation, data-flow sorting, hazard analysis |
| `metadata` | Primitive parameter tables and graph description types, shared by client and server |
| `page` | A page's layout, from the IR, drawn as the HTML the server serves when a project has no `ui/index.html`, and the client's `--page` writes |
| `protocol` | OSC encoding and decoding, the message set, IR chunking, retries |

### Dependency rules

Each module links only what its row lists:

| Target | Links |
|---|---|
| `metadata` | Nothing |
| `ir` | `metadata`, nlohmann/json, json-schema-validator |
| `page` | `metadata` |
| `protocol` | POSIX sockets only |
| `primitives` | `metadata`, PFFFT |
| `interpreter` | `ir`, `metadata`, `protocol` |
| `workspace` | `interpreter`, `protocol` |
| `tui` | `workspace`, FTXUI |
| Server app | `primitives`, `ir`, `page`, `protocol`, JUCE, stb, dr_libs, libcurl |
| Client app | `tui`, `workspace`, `interpreter`, `page`, `protocol`, nlohmann/json |
| Editor app | Tauri and `tauri-plugin-shell` (Cargo); CodeMirror 6 and the packages it imports, and marked (ES modules). It runs the client as its sidecar and links none of the C++ |

- `primitives` does not depend on JUCE. It can be tested and rendered offline without devices.
- The client does not link JUCE. `protocol` implements OSC itself.

### Dependencies

- Every third-party library comes through CMake `FetchContent`, pinned to a release tag or commit. The GUI editor's web packages are npm tarballs fetched this way, pinned by version and SHA-512 (`cmake/EditorWeb.cmake`); there is no npm.
- The one exception: the GUI editor's Rust crates come through Cargo, pinned by the committed `src/apps/editor/src-tauri/Cargo.lock`.
- `FETCHCONTENT_BASE_DIR` points to `third_party/`, which is untracked.
- Dependencies build as CMake targets and link statically.

## Commands

```bash
cmake -B build -S .
cmake --build build
ctest --test-dir build --output-on-failure
scripts/badges.sh
valgrind --leak-check=full --error-exitcode=1 build/tests/<module>_tests
cmake --build build --target docs
build/bin/espresso-client --compile examples/wavetable/wavetable.lisp > wavetable.ir.json
```

IR is build output: compile it from the `.lisp` files and never commit it. To see a compiler change's effect on the examples, compile them on both revisions and compare the IR.

### Clang tooling

Use the LLVM tools wherever they are installed: on Linux the distribution's LLVM, on macOS Homebrew's (`/usr/local/opt/llvm/bin` or `/opt/homebrew/opt/llvm/bin`). Apple's clang lacks clang-tidy and RealtimeSanitizer.

| Tool | Use |
|---|---|
| clang-format | Formats C++ from the root `.clang-format`. Reformat only lines you change (`git clang-format`); whole-file reformatting goes in its own commit |
| clang-tidy | Checks the modern-cpp skill's rules and common bug classes from the root `.clang-tidy`. Silence a finding only at a site listed in the skill's section 9, with `// NOLINT(check): reason` |
| AddressSanitizer and UndefinedBehaviorSanitizer | Every suite, in their own build tree |
| ThreadSanitizer | Every suite, in its own build tree, after any change to threads, queues, or ownership |
| RealtimeSanitizer | Audio-thread code (`process()`, the device callback) marked `[[clang::nonblocking]]`; aborts on allocation, locks, or system calls |

```bash
git clang-format --diff
run-clang-tidy -p build -quiet "^$PWD/(src|include|tests)/"
cmake -B build-asan -S . -DFETCHCONTENT_BASE_DIR="$PWD/build-asan/_deps" -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" -DESPRESSO_BUILD_SERVER=OFF
cmake -B build-tsan -S . -DFETCHCONTENT_BASE_DIR="$PWD/build-tsan/_deps" -DCMAKE_CXX_FLAGS=-fsanitize=thread -DESPRESSO_BUILD_SERVER=OFF -DESPRESSO_BUILD_CLIENT=OFF
cmake -B build-rtsan -S . -DFETCHCONTENT_BASE_DIR="$PWD/build-rtsan/_deps" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS="-fsanitize=realtime -Wfunction-effects" -DESPRESSO_BUILD_SERVER=OFF -DESPRESSO_BUILD_CLIENT=OFF
```

Configure `build` with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` so clang-tidy sees the real compile flags. Sanitizer trees share dependency sources through `-DFETCHCONTENT_SOURCE_DIR_<NAME>=third_party/<name>-src`, but each builds its dependencies under its own `FETCHCONTENT_BASE_DIR`; a tree without one builds into the shared `third_party/*-build`. Run each `build-*/tests/*_tests` executable directly. On macOS, LLVM's tools need the SDK: add `-isysroot $(xcrun --show-sdk-path)` to the RTSan flags and `--extra-arg=-isysroot$(xcrun --show-sdk-path)` to clang-tidy. `-fsanitize=realtime` checks at run time; `-Wfunction-effects` reports, at compile time, calls from nonblocking functions that cannot be shown not to block.

The build produces two executables: the server and the client. `espresso-server --no-audio` answers the protocol with the engine on a timer, for testing the client without devices.

## Architecture

The system follows SuperCollider's client-server model. The client process runs the TUI editor and the interpreter. The server process runs the audio core and devices. The client sends validated IR to the server over UDP. Both run on the same machine, and the server binds to `127.0.0.1`. UDP leaves room for remote operation later.

1. The interpreter reads a project, evaluates it, and compiles it to the IR.
2. The IR is validated against `ir.schema.json` and a set of semantic checks.
3. A builder thread turns the validated IR into a graph plan: sorted nodes, assigned buffers, and allocated node state.
4. The builder pushes the plan into a lock-free single-producer, single-consumer queue.
5. The audio thread swaps in the plan at the start of a block and pushes the old plan into a retired queue.
6. The message thread frees retired plans.

The interpreter never runs on the audio thread. The engine reads only the IR.

### Primitives and user types

- Built-in synthesis types and effects are C++ primitives that run on the audio thread.
- User-defined instruments and effects are classes whose methods build subgraphs of primitives. The compiler flattens these subgraphs into the IR.
- Mappers are expressions wrapped around a pipe. They compile to generator nodes.
- Polyphony is explicit. Voice assignment is the `voiceSplit` primitive. Standard-library voice constructs in the DSL wrap it.
- User code never runs per sample. The engine does not distinguish built-in types from user-defined types.
- The shipped instruments and effects are written in the Lisp dialect from the same primitives available to users.

### Units

- Primitives are unit agnostic. Parameters and pipe values are canonical units: seconds, hertz, linear amplitude, semitones, beats.
- The interpreter converts unit literals (`250ms`, `440hz`, `-6db`) to canonical values and checks dimensions against each primitive's parameter metadata table.
- Runtime values are converted by conversion generators that the compiler inserts into the graph.
- Tempo is dynamic. The engine owns a tempo `cPipe` and a beat clock. Beat values convert to seconds at runtime.

### Pipes and feedback

- `pipe` is an abstract base class. `aPipe` (audio), `cPipe` (control), and `ePipe` (events) implement it.
- Multiple writers to an `aPipe` sum. Writers to a `cPipe` replace. Writers to an `ePipe` merge, sorted by timestamp.
- A `pipeTable` in the engine owns all pipes, as the SuperCollider server owns buses. It persists across graph swaps. The interpreter allocates pipe indices.
- Connecting mismatched pipe types without a conversion generator is a validation error.
- Generators access a pipe's buffer once per block, not once per sample.
- The IR stores nodes and edges. The compiler sorts the graph topologically, and the engine runs nodes from a flat array in that order. A cycle is valid only through a feedback pipe, which delays its signal by one block.
- Feedback loops shorter than one block (comb filters, allpass diffusers, plucked strings) live inside a single primitive.

### Graph swap

- Node IDs derive deterministically from source location: file, defining form, and instance path. Repeated calls are distinguished by occurrence index, or by an explicit `:key`. Never use global counters or random values for IDs.
- A new node with the same ID and type as an old node adopts its state.
- A changed node, including a matching ID with a different type or parameter shape, crossfades: old and new versions write to the same `aPipe`s with opposite gain ramps for the fade period.
- Swaps can be quantized to the next beat or bar, resolved against the beat clock.
- The old graph retires after its fade completes. Its pipe indices are freed only then.

### Data sinks

- A web data sink sends only when a script calls its update function. The engine applies no rate limiting.
- Update requests pass through a bounded lock-free queue to a network thread. When the queue is full, the request is dropped and a counter increments. The counter is visible to scripts and in the console.

## C++ practices

Modern C++ best practice is the default for all code in this repository, including tests. Depart from it only for an exceedingly good reason: an OS, driver, or library interface that cannot be used otherwise, or a measured performance problem. Convenience, familiarity, and speed of writing are not reasons.

- Every resource is owned by an object (RAII). No `new` or `delete`; ownership is `std::unique_ptr` or, when genuinely shared, `std::shared_ptr`.
- No raw pointers, owning or not: references, `std::reference_wrapper`, optional references, spans, and string views instead.
- Every thread is a `espresso::jthread`; every lock is scoped (`std::lock_guard`, `std::scoped_lock`, `std::unique_lock`).
- Each departure is wrapped in the smallest possible type or function, commented with its reason, and listed in the skill below.

The rules, the patterns that satisfy them on the audio thread, and the list of permitted departures are in `.claude/skills/modern-cpp/SKILL.md`. Follow it when writing, reviewing, or refactoring C++, and run its checklist before committing.

## Real-time rules

Code on the audio thread must not:

- Allocate or free memory, including container growth and releasing the last `std::shared_ptr`
- Lock a mutex or wait on a condition variable
- Perform file, socket, or console I/O
- Throw exceptions
- Run loops without a fixed bound

Use preallocated buffers, `std::atomic` values, and lock-free queues. Enable flush-to-zero and denormals-are-zero at the top of every audio callback with a scoped guard in `primitives`.

## Language

- The syntax is Lisp. The object system follows CLOS: `defclass`, `defmethod`, generic functions, and inheritance.
- The class precedence list uses C3 linearization, not the CLOS algorithm.
- The language has no macros. All syntax forms are built into the evaluator.
- A project is a collection of files. The language specification defines the module form, the entry point, and the project root that sample and data paths resolve against.

## Specification

Implementation specs live in `local-docs/`:

| File | Covers |
|---|---|
| `grammar.md` | The Lisp DSL grammar |
| `interpreter.md` | Reader, evaluator, object system, units, graph building, IR emission, submission |
| `primitives.md` | Primitives, pipes, `pipeTable`, clock domains, plan swap, execution |
| `tui.md` | The editor |
| `protocol.md` | OSC-over-UDP messages between client and server |
| `samples.md` | Audio files, buffers, sample playback, granulation, and FFT: the primitives of M5 |
| `io.md` | Serial and web I/O: the sources, sinks, and bridges of M6 |
| `design-notes.md` | Decisions and their status |
| `lessons.md` | Mistakes made in this repository and the rule that prevents each. Read before build, UI, or terminal work |

- Rules are numbered (for example, §4.3.2) and use MUST and SHOULD for requirements. Cite rule numbers in code review and commit messages.
- Worked examples live in `examples/`, one folder per piece, entered at `NAME/NAME.lisp` or, for a project of several files (`hectodrone`, `organ-pad`, `phasing`), at `main.lisp`. The Catch2 suite compiles each and checks that its IR is valid and the same every time; the server suite plays each piece of one file and checks its behavior.
- `schema/ir.schema.json` defines the IR structure. The specs define its meaning.

## Testing

v1 passes when:

- The Catch2 suite passes for the reader, evaluator, compiler, IR validation, and DSP.
- Every Assignment 5 starter piece, rewritten in the Lisp dialect, compiles to valid IR and plays.
- The same IR and seed always render the same output offline.
- Valgrind reports no errors or leaks on the test suite.

v2 passes when a piece of 8 instruments with 2 effects each and 8 voices per instrument (64 voices) runs for 30 minutes at 256 samples and 48 kHz with no missed deadlines and no xruns in `pw-top`.

## Documentation

| Folder | Contents | Committed |
|---|---|---|
| `docs/` | Material a user or developer needs: user guide, language reference, API reference | Yes |
| `local-docs/` | Implementation specs, design notes, research, reference papers, scratch work | No |

- Put all research in `local-docs/`. Do not add research, notes, or downloaded references to `docs/`.
- When a design decision in `local-docs/` becomes final, document the result in `docs/` and leave the research in `local-docs/`.
- Write `docs/` in a formal reference-manual style: no first person, no hedging, no history or change narration, one term per concept.
- Document public C++ APIs with Doxygen comments.
- `docs/api/` is the Doxygen HTML of `include/espresso/`, with `docs/api.md` as its main page. Regenerate it with `cmake --build build --target docs` and commit it with any change to a public header.
