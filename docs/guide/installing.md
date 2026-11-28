# Installing

espresso is built from source with CMake. The build produces two programs in `build/bin/`, the server `espresso-server` and the client `espresso-client`, and prepares the GUI editor, which Cargo builds. The programs run from the build directory; the build has no install step.

Linux is the release platform. macOS builds and runs the same programs from the build directory, without an application bundle or code signing.

## Requirements

- CMake 3.25 or later
- A C++20 compiler: GCC 12 or later, or Clang 15 or later
- Git, which CMake uses to download the libraries
- OpenSSL, for libcurl's TLS
- Optional: Ninja, Doxygen, Valgrind

CMake downloads every library into `third_party/` at configure time: JUCE, FTXUI, nlohmann/json, json-schema-validator, Catch2, stb, dr_libs, PFFFT, and libcurl, each pinned to a release or a commit. The libraries build as part of the project and link statically.

### Linux

The server opens audio and MIDI devices through JUCE, which needs the ALSA and JACK headers and `pkg-config`. On Debian and Ubuntu:

```bash
sudo apt install build-essential cmake ninja-build git pkg-config libasound2-dev libjack-jackd2-dev libssl-dev
```

Other distributions name the same packages differently: a compiler, CMake, Git, `pkg-config`, and the development packages of ALSA, JACK, and OpenSSL.

At run time the server connects to PipeWire through its JACK interface (`pipewire-jack`), or to a JACK server, and falls back to ALSA when neither lists a device ([Playing](playing.md#the-server)). Real-time scheduling needs an rtprio limit, which the `audio` group grants on most distributions ([Performance](performance.md#threads)).

### macOS

- Xcode Command Line Tools (`xcode-select --install`), which provide Apple clang 15 or later and the macOS SDK
- CMake, Ninja, and OpenSSL from Homebrew: `brew install cmake ninja openssl@3`
- Optional: Homebrew LLVM (`brew install llvm`) for clang-format, clang-tidy, and RealtimeSanitizer, which Apple's toolchain does not include

CMake finds Homebrew's `openssl@3` in `/opt/homebrew` or `/usr/local`; `-DOPENSSL_ROOT_DIR=DIR` names another. The server uses Core Audio through JUCE; JACK and ALSA are not used. Audio input needs microphone permission for the terminal application (System Settings › Privacy & Security › Microphone); without it, the server runs with no inputs.

## Building

```bash
git clone https://github.com/victor-almaraz/mpcs-51238-final-project.git espresso
cd espresso
cmake -B build -S . -G Ninja
cmake --build build
```

The first configure downloads the libraries, and the first build compiles them. The programs are `build/bin/espresso-server` and `build/bin/espresso-client`; `--version` prints each one's version.

| Option | Default | Effect |
|---|---|---|
| `ESPRESSO_BUILD_SERVER` | `ON` | Builds the server and downloads JUCE |
| `ESPRESSO_BUILD_CLIENT` | `ON` | Builds the client |
| `ESPRESSO_BUILD_EDITOR` | The value of `ESPRESSO_BUILD_CLIENT` | Prepares the GUI editor: fetches its web packages and places the client and the server in `src/apps/editor/src-tauri/binaries/` |
| `ESPRESSO_BUILD_TESTS` | `ON` | Builds the Catch2 suite |
| `ESPRESSO_WARNINGS_AS_ERRORS` | `OFF` | Adds `-Werror` |
| `ESPRESSO_COVERAGE` | `OFF` | Instruments the project's targets for LLVM source-based coverage (Clang only) and adds the `coverage-badge` target |

Options are set at configure time, for example `cmake -B build -S . -DESPRESSO_BUILD_TESTS=OFF`.

The build type is Debug unless `CMAKE_BUILD_TYPE` names another. A Debug engine is compiled without optimization, so heavy pieces play live from a Release build, in a tree of its own:

```bash
cmake -B build-release -S . -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

## The GUI editor

The GUI editor is a Tauri application over the client. It needs, beyond the C++ build:

- Rust 1.90 or later, with Cargo
- Tauri's command-line tool: `cargo install tauri-cli --locked`
- On Linux, the WebKitGTK and libsoup development packages: `libwebkit2gtk-4.1-dev` and `libsoup-3.0-dev` on Debian and Ubuntu

After `cmake --build build`, Cargo builds and opens it:

```bash
cd src/apps/editor/src-tauri
cargo tauri dev
```

`cargo tauri build` builds the application's executable, `espresso`, in release mode. The window starts `espresso-server` and the client itself ([The GUI editor](gui-editor.md)).

## Tests

```bash
ctest --test-dir build --output-on-failure
```

The test executables are in `build/tests/`, one for each module, and each runs on its own. `scripts/badges.sh` builds the project, runs the suite, rewrites the two badges of `README.md`, and adds any missing badge line under its title; it exits non-zero when a build or a test fails:

```bash
scripts/badges.sh
```

| Badge | Build tree | Target | Content |
|---|---|---|---|
| `docs/badges/tests.svg` | `build/` | `test-badge` | Passed tests, counted from a JUnit report (`build/Testing/test-badge.xml`) |
| `docs/badges/coverage.svg` | `build-coverage/` | `coverage-badge` | Line coverage of `src/` and `include/`, from LLVM source-based coverage |

The coverage tree is configured with Clang and `-DESPRESSO_COVERAGE=ON` on first use; `LLVM_BIN` selects the directory of `clang++`, `llvm-profdata`, and `llvm-cov`. `--tests` and `--coverage` regenerate one badge, `-j N` sets the build's parallel jobs, and `--help` lists every option.

### LLVM tools on macOS

The LLVM tools need the SDK path. clang-tidy takes it as an extra argument, and the RealtimeSanitizer tree takes it in its compiler flags, with Homebrew's C and C++ compilers:

```bash
run-clang-tidy -p build -quiet -extra-arg=-isysroot$(xcrun --show-sdk-path) 'src/|include/|tests/'
```

```bash
cmake -B build-rtsan -S . -G Ninja -DFETCHCONTENT_BASE_DIR="$PWD/build-rtsan/_deps" -DCMAKE_C_COMPILER="$(brew --prefix llvm)/bin/clang" -DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++" -DCMAKE_CXX_FLAGS="-fsanitize=realtime -Wfunction-effects -isysroot $(xcrun --show-sdk-path)" -DESPRESSO_BUILD_SERVER=OFF -DESPRESSO_BUILD_CLIENT=OFF
```

AddressSanitizer runs without leak detection on macOS, which LeakSanitizer does not support: set `ASAN_OPTIONS=detect_leaks=0` when running the test executables. Valgrind is not available on current macOS; leak checks run on Linux.
