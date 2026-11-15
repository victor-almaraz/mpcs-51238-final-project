# AGENTS.md

Instructions for agents working in this repository.

## Project

A Linux-first workstation for acousmatic, aleatory, and automatic music. Pieces are written in a Lisp dialect. A project is a collection of files that compiles to an intermediate representation (IR). A native C++ engine plays the IR.

The core is the `primitives` module. Every primitive is a node in the IR graph:

| Primitive | Role | Examples |
|---|---|---|
| Source | Produces data | MIDI, OSC, web data, serial devices, scripts that generate triggers or synthetic data |
| Sink | Receives data | Audio, visuals, MIDI, OSC, web data, serial devices |
| Generator | Takes parameters and produces a signal or an image | Oscillators, filters, envelopes, mappers |
| Pipe | Connects primitives | `aPipe`, `cPipe`, `ePipe`, `vPipe` |

The engine treats every source and every sink the same way. Any source can drive any sink.

A companion web directory lists live data APIs. Signed-in users save entries, and the native app retrieves saved entries with a personal token.

## Scope

v1 (November 17) establishes the core and validates its behavior. v2 (December 1) adds performance work and the remaining source and sink types.

| Version | Contents |
|---|---|
| v1 | Single-threaded engine, interpreter and compiler, MIDI and OSC sources, one visual layer, TUI editor |
| v2 | Multithreaded engine, web data and serial sources and sinks, MIDI and OSC sinks, data directory, remaining visual layers, AppImage release |

Out of scope:

- Hosting third-party plugins
- Windows and macOS
- Visual editing
- Accounts in the native app
- Cloud sync of projects

## Milestones

Complete the milestones in order.

| Milestone | Deliverable |
|---|---|
| M1 | Mouse-first TUI editor: syntax highlighting, files and buffers, project file list, search, undo and redo, console, run controls, and selectable key schemes (Emacs, CUA, Vim, nano) |
| M2 | Audio primitives that support audio, OSC, and MIDI input and output |
| M3 | Image primitives: a Sketching subset (`primitives.md` §7.7) |
| M4 | Composition of primitives, tested by implementing basic audio synthesis and effects from primitives |

## Stack

| Component | Use |
|---|---|
| C++20 | Engine, interpreter, and application |
| JUCE (AGPLv3) | Audio devices, MIDI, and the OpenGL window. Server app only |
| FTXUI (MIT) | TUI editor. Syntax highlighting is a custom component that renders tokens from the Lisp reader |
| CMake | Build system. Dependencies through `FetchContent` |
| nlohmann/json, json-schema-validator | IR reading, writing, and schema validation |
| Blend2D (zlib) | Frame rasterization in `primitives`, on the CPU without its JIT, so frames are deterministic. The server uploads finished frames to OpenGL |
| stb_image, stb_image_write (public domain) | Image files for `imagePlayer` and PNG frames from offline renders. Server app only |
| Catch2 with CTest | Unit and integration tests |
| Valgrind | Memcheck on the test suite and offline renders |
| PipeWire | Target audio server; the app connects as a JACK client |
| Doxygen, Sphinx, Breathe | API reference, user guide, and language reference |
| Vercel, Supabase, Clerk | Data directory hosting, database, and sign-in |

The project is licensed under AGPLv3 to match JUCE.

## Repository layout

```text
.
├── CMakeLists.txt
├── cmake/                    dependency pins, helper functions
├── include/<proj>/
│   ├── primitives/
│   ├── interpreter/
│   ├── tui/
│   ├── ir/
│   ├── metadata/
│   └── protocol/
├── src/
│   ├── primitives/  interpreter/  tui/  ir/  metadata/  protocol/
│   └── apps/
│       ├── server/          main.cpp (options, control loop) and devices.* (JUCE);
│       │                    engineHost.*, offline.*, oscBridge.* (no JUCE, tested)
│       ├── client/main.cpp
│       └── stub-server/main.cpp
├── schema/ir.schema.json
├── examples/                 paired .lisp and .ir.json files
├── tests/                    one folder per module, plus server/
├── docs/                     human documentation
├── local-docs/               agent specs and research (untracked)
├── third_party/              downloaded dependencies (untracked)
└── AGENTS.md
```

- Headers live in `include/<proj>/<module>/` and sources in `src/<module>/`. Include headers by project path: `#include <proj/primitives/pipe.h>`.
- `<proj>` is a placeholder until the project is named.

### Modules

| Module | Job |
|---|---|
| `primitives` | Sources, sinks, generators, pipes, `pipeTable`, per-block execution |
| `interpreter` | Reader, evaluator, object system, units, graph building, IR emission |
| `tui` | Editor |
| `ir` | IR JSON reading and writing, schema and semantic validation, data-flow sorting, hazard analysis |
| `metadata` | Primitive parameter tables and graph description types, shared by client and server |
| `protocol` | OSC encoding and decoding, the message set, IR chunking, retries |

### Dependency rules

Each module links only what its row lists:

| Target | Links |
|---|---|
| `metadata` | Nothing |
| `ir` | `metadata`, nlohmann/json, json-schema-validator |
| `protocol` | POSIX sockets only |
| `primitives` | `metadata`, Blend2D |
| `interpreter` | `ir`, `metadata`, `protocol` |
| `tui` | `interpreter`, FTXUI |
| Server app | `primitives`, `ir`, `protocol`, JUCE, stb |
| Client app | `tui`, `interpreter`, `protocol` |
| Stub server app | `protocol`, `metadata` |

- `primitives` does not depend on JUCE. It can be tested and rendered offline without devices.
- The client does not link JUCE. `protocol` implements OSC itself.

### Dependencies

- Every third-party library comes through CMake `FetchContent`, pinned to a release tag or commit.
- `FETCHCONTENT_BASE_DIR` points to `third_party/`, which is untracked.
- Dependencies build as CMake targets and link statically.

## Commands

```bash
cmake -B build -S .
cmake --build build
ctest --test-dir build --output-on-failure
cmake --build build --target test-badge
valgrind --leak-check=full --error-exitcode=1 build/tests/<module>_tests
cmake --build build --target docs
build/bin/proj-client --compile examples/delay.lisp > delay.ir.json
PROJ_UPDATE_EXAMPLES=1 build/tests/interpreter_tests "examples compile to their IR files"
```

The last command rewrites `examples/*.ir.json` after an intended compiler change; review the diff before committing it.

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
run-clang-tidy -p build -quiet 'src/|include/|tests/'
cmake -B build-asan -S . -DFETCHCONTENT_BASE_DIR="$PWD/build-asan/_deps" -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" -DPROJ_BUILD_SERVER=OFF
cmake -B build-tsan -S . -DFETCHCONTENT_BASE_DIR="$PWD/build-tsan/_deps" -DCMAKE_CXX_FLAGS=-fsanitize=thread -DPROJ_BUILD_SERVER=OFF -DPROJ_BUILD_CLIENT=OFF
cmake -B build-rtsan -S . -DFETCHCONTENT_BASE_DIR="$PWD/build-rtsan/_deps" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS="-fsanitize=realtime -Wfunction-effects" -DPROJ_BUILD_SERVER=OFF -DPROJ_BUILD_CLIENT=OFF
```

Configure `build` with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` so clang-tidy sees the real compile flags. Sanitizer trees share dependency sources through `-DFETCHCONTENT_SOURCE_DIR_<NAME>=third_party/<name>-src`, but each builds its dependencies under its own `FETCHCONTENT_BASE_DIR`; a tree without one builds into the shared `third_party/*-build`. Run each `build-*/tests/*_tests` executable directly. On macOS, LLVM's tools need the SDK: add `-isysroot $(xcrun --show-sdk-path)` to the RTSan flags and `--extra-arg=-isysroot$(xcrun --show-sdk-path)` to clang-tidy. `-fsanitize=realtime` checks at run time; `-Wfunction-effects` reports, at compile time, calls from nonblocking functions that cannot be shown not to block.

The build produces three executables: the server, the client, and the stub server. The stub server answers the protocol without an audio engine, for testing the client.

## Architecture

The system follows SuperCollider's client-server model. The client process runs the TUI editor and the interpreter. The server process runs the audio core, devices, and a separate OpenGL window for visual output. The client sends validated IR to the server over UDP. Both run on the same machine, and the server binds to `127.0.0.1`. UDP leaves room for remote operation later.

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
- Drawing generators chain through `vPipe`s: each reads an input frame and draws over it.
- User code never runs per sample. The engine does not distinguish built-in types from user-defined types.
- The shipped instruments and effects are written in the Lisp dialect from the same primitives available to users.

### Units

- Primitives are unit agnostic. Parameters and pipe values are canonical units: seconds, hertz, linear amplitude, semitones, beats.
- The interpreter converts unit literals (`250ms`, `440hz`, `-6db`) to canonical values and checks dimensions against each primitive's parameter metadata table.
- Runtime values are converted by conversion generators that the compiler inserts into the graph.
- Tempo is dynamic. The engine owns a tempo `cPipe` and a beat clock. Beat values convert to seconds at runtime.

### Pipes and feedback

- `pipe` is an abstract base class. `aPipe` (audio), `cPipe` (control), `ePipe` (events), and `vPipe` (video) implement it.
- Multiple writers to an `aPipe` sum. Writers to a `cPipe` or `vPipe` replace. Writers to an `ePipe` merge, sorted by timestamp.
- A `pipeTable` in the engine owns all pipes, as the SuperCollider server owns buses. It persists across graph swaps. The interpreter allocates pipe indices.
- Connecting mismatched pipe types without a conversion generator is a validation error.
- Generators access a pipe's buffer once per block, not once per sample.
- The IR stores nodes and edges. The compiler sorts the graph topologically, and the engine runs nodes from a flat array in that order. A cycle is valid only through a feedback pipe, which delays its signal by one block.
- Feedback loops shorter than one block (comb filters, allpass diffusers, plucked strings) live inside a single primitive.

### Clock domains

- The audio clock is the master clock. Audio never runs on the frame clock.
- Audio to video: a ring buffer written by the audio thread and read by the render thread, either every sample since the last read or the latest window. On overflow, the read is skipped and a counter increments.
- Video to audio: a triple buffer. The audio side samples the latest complete frame once per block, with hold or linear interpolation.

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
- Every thread is a `proj::jthread`; every lock is scoped (`std::lock_guard`, `std::scoped_lock`, `std::unique_lock`).
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
| `design-notes.md` | Decisions and their status |
| `lessons.md` | Mistakes made in this repository and the rule that prevents each. Read before build, UI, or terminal work |

- Rules are numbered (for example, §4.3.2) and use MUST and SHOULD for requirements. Cite rule numbers in code review and commit messages.
- Worked examples live in `examples/`: a `.lisp` file and the `.ir.json` file it compiles to. The Catch2 suite compiles every example and compares the output to its IR file.
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
