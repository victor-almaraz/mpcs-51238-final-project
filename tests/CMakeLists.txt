# One Catch2 executable per module: build/tests/<module>_tests.

include(Catch)

function(proj_add_tests module)
    set(target ${module}_tests)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${module}/*.cpp")
    add_executable(${target} ${sources})
    target_link_libraries(${target} PRIVATE proj::${module} Catch2::Catch2WithMain)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tests")
    proj_set_warnings(${target})
    catch_discover_tests(${target} TEST_PREFIX "${module}: ")
endfunction()

foreach(module metadata ir protocol primitives interpreter tui)
    proj_add_tests(${module})
endforeach()
