# Third-party dependencies. Every library comes through FetchContent,
# pinned to a release tag (or, for projects that publish none, a commit),
# downloaded to third_party/ (untracked), and built as a static CMake
# target.

# Set before include(FetchContent), which otherwise caches its default.
set(FETCHCONTENT_BASE_DIR "${PROJECT_SOURCE_DIR}/third_party" CACHE PATH
    "Download and build directory for FetchContent dependencies")

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- Pins ---------------------------------------------------------------
set(ESPRESSO_NLOHMANN_JSON_TAG    v3.12.0)
set(ESPRESSO_JSON_VALIDATOR_TAG   2.4.1)
set(ESPRESSO_FTXUI_TAG            v7.0.3)
set(ESPRESSO_CATCH2_TAG           v3.16.1)
set(ESPRESSO_JUCE_TAG             9.0.5)
set(ESPRESSO_CURL_TAG             curl-8_19_0)
# stb and dr_libs publish no release tags; these are master commits.
set(ESPRESSO_STB_COMMIT           9600522de53973336fa033a289858a4354cbb04c)
set(ESPRESSO_DR_LIBS_COMMIT       7f032b984ae5dd3ab8a58721375aca9200f91965)
set(ESPRESSO_PFFFT_COMMIT         efc1dd2de6c3562fc464f905d37d9281f08a20dd)

# --- nlohmann/json (ir) -------------------------------------------------
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        ${ESPRESSO_NLOHMANN_JSON_TAG}
    GIT_SHALLOW    TRUE
    SYSTEM)

# --- json-schema-validator (ir) -----------------------------------------
set(JSON_VALIDATOR_BUILD_TESTS OFF CACHE INTERNAL "")
set(JSON_VALIDATOR_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(JSON_VALIDATOR_INSTALL OFF CACHE INTERNAL "")
set(JSON_VALIDATOR_SHARED_LIBS OFF CACHE INTERNAL "")
FetchContent_Declare(json_schema_validator
    GIT_REPOSITORY https://github.com/pboettch/json-schema-validator.git
    GIT_TAG        ${ESPRESSO_JSON_VALIDATOR_TAG}
    GIT_SHALLOW    TRUE
    SYSTEM)

FetchContent_MakeAvailable(nlohmann_json json_schema_validator)

# Schema validation is half of a Debug read of a large IR file
# (threads.md §9g.6.1), so the validator is optimised in Debug builds too.
target_compile_options(nlohmann_json_schema_validator PRIVATE $<$<CONFIG:Debug>:-O2>)

# --- PFFFT (primitives: fft, ifft, wavetable band limiting) -------------
# Only the single-precision transform, built here: the repository's own
# CMake project and its submodules add other FFTs, tests, and benchmarks.
# Its SIMD path is fixed at compile time, so a build's transforms are
# deterministic (samples.md §10.6).
FetchContent_Declare(pffft
    GIT_REPOSITORY https://github.com/marton78/pffft.git
    GIT_TAG        ${ESPRESSO_PFFFT_COMMIT}
    GIT_SUBMODULES ""
    SOURCE_SUBDIR  headers-only
    SYSTEM)
FetchContent_MakeAvailable(pffft)
add_library(espresso_pffft STATIC "${pffft_SOURCE_DIR}/src/pffft.c" "${pffft_SOURCE_DIR}/src/pffft_common.c")
target_include_directories(espresso_pffft PRIVATE "${pffft_SOURCE_DIR}/include/pffft")
target_include_directories(espresso_pffft SYSTEM PUBLIC "${pffft_SOURCE_DIR}/include")
set_target_properties(espresso_pffft PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- stb_vorbis (server: Ogg Vorbis files, samples.md §4.1) -------------
# One C file, which the server core compiles once with dr_libs below.
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        ${ESPRESSO_STB_COMMIT}
    SYSTEM)
FetchContent_MakeAvailable(stb)
add_library(espresso_stb INTERFACE)
target_include_directories(espresso_stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")

# --- dr_wav, dr_flac, dr_mp3 (server: audio files, samples.md §4.1) ---
# Header-only; the server core compiles the implementations once, with
# stb_vorbis from the stb checkout above for Ogg Vorbis. SOURCE_SUBDIR
# names no directory, so the headers are fetched without dr_libs' own
# CMake project and its libraries.
FetchContent_Declare(dr_libs
    GIT_REPOSITORY https://github.com/mackron/dr_libs.git
    GIT_TAG        ${ESPRESSO_DR_LIBS_COMMIT}
    SOURCE_SUBDIR  headers-only
    SYSTEM)
FetchContent_MakeAvailable(dr_libs)
add_library(espresso_dr_libs INTERFACE)
target_include_directories(espresso_dr_libs SYSTEM INTERFACE "${dr_libs_SOURCE_DIR}")

# --- FTXUI (tui) --------------------------------------------------------
if(ESPRESSO_BUILD_CLIENT OR ESPRESSO_BUILD_TESTS)
    set(FTXUI_BUILD_EXAMPLES OFF CACHE INTERNAL "")
    set(FTXUI_BUILD_DOCS OFF CACHE INTERNAL "")
    set(FTXUI_BUILD_TESTS OFF CACHE INTERNAL "")
    set(FTXUI_ENABLE_INSTALL OFF CACHE INTERNAL "")
    FetchContent_Declare(ftxui
        GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
        GIT_TAG        ${ESPRESSO_FTXUI_TAG}
        GIT_SHALLOW    TRUE
        SYSTEM)
    FetchContent_MakeAvailable(ftxui)
endif()

# --- Catch2 (tests) -----------------------------------------------------
if(ESPRESSO_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        ${ESPRESSO_CATCH2_TAG}
        GIT_SHALLOW    TRUE
        SYSTEM)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()

# --- libcurl (server core: the web bridge's transport, io.md §3.3) -----
# HTTP, HTTPS, WS, and WSS only, with TLS from the system's OpenSSL; no
# compression, HTTP/2, IDN, or PSL, so it needs nothing else. On macOS,
# OpenSSL comes from Homebrew unless OPENSSL_ROOT_DIR names another.
if(APPLE AND NOT OPENSSL_ROOT_DIR)
    foreach(prefix /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
        if(EXISTS "${prefix}/include/openssl/ssl.h")
            set(OPENSSL_ROOT_DIR "${prefix}" CACHE PATH "OpenSSL for libcurl")
            break()
        endif()
    endforeach()
endif()
set(BUILD_CURL_EXE OFF CACHE INTERNAL "")
set(BUILD_LIBCURL_DOCS OFF CACHE INTERNAL "")
set(BUILD_MISC_DOCS OFF CACHE INTERNAL "")
set(ENABLE_CURL_MANUAL OFF CACHE INTERNAL "")
set(BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(BUILD_STATIC_LIBS ON CACHE INTERNAL "")
set(CURL_USE_OPENSSL ON CACHE INTERNAL "")
set(CURL_USE_LIBPSL OFF CACHE INTERNAL "")
set(CURL_USE_LIBSSH2 OFF CACHE INTERNAL "")
set(CURL_ZLIB OFF CACHE INTERNAL "")
set(CURL_BROTLI OFF CACHE INTERNAL "")
set(CURL_ZSTD OFF CACHE INTERNAL "")
set(USE_NGHTTP2 OFF CACHE INTERNAL "")
set(USE_LIBIDN2 OFF CACHE INTERNAL "")
set(USE_APPLE_IDN OFF CACHE INTERNAL "")
# Every protocol but HTTP and WebSocket is off; HTTP_ONLY would turn
# WebSocket off too.
foreach(protocol DICT FILE FTP GOPHER IMAP IPFS LDAP LDAPS MQTT POP3 RTSP SMB SMTP TELNET TFTP)
    set(CURL_DISABLE_${protocol} ON CACHE INTERNAL "")
endforeach()
set(CURL_DISABLE_WEBSOCKETS OFF CACHE INTERNAL "")
FetchContent_Declare(curl
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG        ${ESPRESSO_CURL_TAG}
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
    SYSTEM)
# libcurl's own tests stay out of this build's.
set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)
FetchContent_MakeAvailable(curl)
unset(BUILD_TESTING)
unset(BUILD_SHARED_LIBS)

# --- JUCE (server app only) ---------------------------------------------
if(ESPRESSO_BUILD_SERVER)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        ${ESPRESSO_JUCE_TAG}
        GIT_SHALLOW    TRUE
        SYSTEM)
    FetchContent_MakeAvailable(JUCE)
endif()
