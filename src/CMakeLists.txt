# Module libraries. Each module links only what AGENTS.md permits.

proj_add_module(metadata
    SOURCES metadata/module.cpp)

proj_add_module(ir
    SOURCES ir/module.cpp
    LINKS proj::metadata nlohmann_json::nlohmann_json nlohmann_json_schema_validator)

proj_add_module(protocol
    SOURCES protocol/module.cpp)

proj_add_module(primitives
    SOURCES primitives/module.cpp
    LINKS proj::metadata)

proj_add_module(interpreter
    SOURCES interpreter/module.cpp
    LINKS proj::ir proj::metadata proj::protocol)

if(PROJ_BUILD_CLIENT OR PROJ_BUILD_TESTS)
    proj_add_module(tui
        SOURCES tui/module.cpp
        LINKS proj::interpreter ftxui::component ftxui::dom ftxui::screen)
endif()

add_subdirectory(apps)
