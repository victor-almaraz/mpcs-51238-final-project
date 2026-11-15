# Executables. The server links JUCE; the client does not.

# The server's protocol engine, without JUCE, so tests can link it.
add_library(proj_server_core STATIC server/engineHost.cpp server/images.cpp server/offline.cpp server/oscBridge.cpp
                                   server/stb.cpp)
add_library(proj::server ALIAS proj_server_core)
target_include_directories(proj_server_core PUBLIC server)
target_link_libraries(proj_server_core PUBLIC proj::primitives proj::ir proj::protocol PRIVATE proj_stb)
proj_set_warnings(proj_server_core)

if(PROJ_BUILD_SERVER)
    juce_add_console_app(proj_server
        PRODUCT_NAME "proj-server")
    target_sources(proj_server PRIVATE server/main.cpp server/devices.cpp)
    target_compile_definitions(proj_server PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        PROJ_VERSION="${PROJECT_VERSION}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # PipeWire is the target audio server; the app connects as a JACK client.
        target_compile_definitions(proj_server PRIVATE JUCE_JACK=1 JUCE_ALSA=1)
    endif()
    target_link_libraries(proj_server PRIVATE
        proj::server
        juce::juce_core
        juce::juce_events
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    set_target_properties(proj_server PROPERTIES
        OUTPUT_NAME "proj-server"
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
endif()

if(PROJ_BUILD_CLIENT)
    add_executable(proj_client client/main.cpp)
    target_compile_definitions(proj_client PRIVATE PROJ_VERSION="${PROJECT_VERSION}")
    target_link_libraries(proj_client PRIVATE
        proj::tui
        proj::interpreter
        proj::protocol)
    set_target_properties(proj_client PROPERTIES
        OUTPUT_NAME "proj-client"
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
    proj_set_warnings(proj_client)
endif()

# Development server: the protocol without an audio engine. Links no JUCE.
add_executable(proj_stub_server stub-server/main.cpp)
target_compile_definitions(proj_stub_server PRIVATE PROJ_VERSION="${PROJECT_VERSION}")
target_link_libraries(proj_stub_server PRIVATE proj::protocol proj::metadata)
set_target_properties(proj_stub_server PROPERTIES
    OUTPUT_NAME "proj-stub-server"
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
proj_set_warnings(proj_stub_server)
