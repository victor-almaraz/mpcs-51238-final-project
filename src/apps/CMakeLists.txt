# Executables. The server links JUCE; the client does not.

# The server's protocol engine, without JUCE, so tests can link it.
add_library(espresso_server_core STATIC server/engineHost.cpp server/images.cpp server/offline.cpp server/oscBridge.cpp
                                   server/splash.cpp server/stb.cpp)
add_library(espresso::server ALIAS espresso_server_core)
target_include_directories(espresso_server_core PUBLIC server)
target_link_libraries(espresso_server_core PUBLIC espresso::primitives espresso::ir espresso::protocol PRIVATE espresso_stb)
espresso_set_warnings(espresso_server_core)

if(ESPRESSO_BUILD_SERVER)
    juce_add_console_app(espresso_server
        PRODUCT_NAME "espresso-server")
    target_sources(espresso_server PRIVATE server/main.cpp server/devices.cpp server/window.cpp)
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
