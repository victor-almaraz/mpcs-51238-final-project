# Executables. The server links JUCE; the client does not.

# The server's protocol engine, without JUCE, so tests can link it.
add_library(espresso_server_core STATIC server/audioCodecs.cpp server/cores.cpp server/engineHost.cpp server/images.cpp
                                   server/offline.cpp server/oscBridge.cpp server/realtime.cpp server/samples.cpp
                                   server/ioStats.cpp server/serialBridge.cpp server/serialLines.cpp server/webBridge.cpp server/webData.cpp
                                   server/webSocket.cpp server/curlTransport.cpp server/pageMessages.cpp
                                   server/pageBridge.cpp server/pagePairing.cpp
                                   server/splash.cpp server/stb.cpp)
# The page helper, server/espresso.js, is embedded as a string literal;
# editing it re-embeds it at the next build.
set(ESPRESSO_PAGE_HELPER "${CMAKE_CURRENT_SOURCE_DIR}/server/espresso.js")
set(ESPRESSO_PAGE_HELPER_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/server/pageHelper.cpp")
add_custom_command(
    OUTPUT "${ESPRESSO_PAGE_HELPER_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" "-DIN=${ESPRESSO_PAGE_HELPER}" "-DOUT=${ESPRESSO_PAGE_HELPER_SOURCE}"
            -DNAMESPACE=server -DFUNCTION=pageHelper -P "${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake"
    DEPENDS "${ESPRESSO_PAGE_HELPER}" "${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake"
    COMMENT "Embedding the page helper"
    VERBATIM)
target_sources(espresso_server_core PRIVATE "${ESPRESSO_PAGE_HELPER_SOURCE}")
add_library(espresso::server ALIAS espresso_server_core)
target_include_directories(espresso_server_core PUBLIC server)
target_link_libraries(espresso_server_core PUBLIC espresso::primitives espresso::ir espresso::protocol PRIVATE espresso_stb espresso_dr_libs CURL::libcurl)
espresso_set_warnings(espresso_server_core)

if(ESPRESSO_BUILD_SERVER)
    juce_add_console_app(espresso_server
        PRODUCT_NAME "espresso-server")
    target_sources(espresso_server PRIVATE server/main.cpp server/devices.cpp server/window.cpp)
    espresso_pin_floating_point(espresso_server)
    target_compile_definitions(espresso_server PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        ESPRESSO_VERSION="${PROJECT_VERSION}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # PipeWire is the target audio server; the app connects as a JACK client.
        target_compile_definitions(espresso_server PRIVATE JUCE_JACK=1 JUCE_ALSA=1)
    endif()
    target_link_libraries(espresso_server PRIVATE
        espresso::server
        juce::juce_core
        juce::juce_events
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_gui_basics
        juce::juce_opengl
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    set_target_properties(espresso_server PROPERTIES
        OUTPUT_NAME "espresso-server"
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
    if(APPLE)
        # A Mach-O executable exports its external symbols, and dyld binds
        # each weak one against every loaded framework at launch: with
        # JUCE's GUI and OpenGL that took 0.7 s before main. Nothing loads
        # symbols from the server, so it exports none. (ELF executables
        # export nothing without -rdynamic.)
        target_link_options(espresso_server PRIVATE LINKER:-no_exported_symbols)
    endif()
endif()

if(ESPRESSO_BUILD_CLIENT)
    add_executable(espresso_client client/main.cpp)
    target_compile_definitions(espresso_client PRIVATE ESPRESSO_VERSION="${PROJECT_VERSION}")
    target_link_libraries(espresso_client PRIVATE
        espresso::tui
        espresso::interpreter
        espresso::protocol)
    set_target_properties(espresso_client PROPERTIES
        OUTPUT_NAME "espresso-client"
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
    espresso_set_warnings(espresso_client)
endif()
