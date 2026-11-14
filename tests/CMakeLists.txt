# One Catch2 executable per module: build/tests/<module>_tests.

include(Catch)

function(proj_add_tests module)
    set(target ${module}_tests)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${module}/*.cpp")
    if(NOT sources)
        return()  # the module has no tests yet
    endif()
    add_executable(${target} ${sources})
    target_link_libraries(${target} PRIVATE proj::${module} Catch2::Catch2WithMain)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(${target} PRIVATE PROJ_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tests")
    proj_set_warnings(${target})
    catch_discover_tests(${target} TEST_PREFIX "${module}: ")
    set_property(GLOBAL APPEND PROPERTY PROJ_TEST_TARGETS ${target})
endfunction()

foreach(module metadata ir protocol primitives interpreter tui server)
    proj_add_tests(${module})
endforeach()

# test-badge runs the suite and rewrites docs/badges/tests.svg.
get_property(testTargets GLOBAL PROPERTY PROJ_TEST_TARGETS)
add_custom_target(test-badge
    COMMAND ${CMAKE_COMMAND}
        -DBUILD_DIR=${PROJECT_BINARY_DIR}
        -DOUTPUT=${PROJECT_SOURCE_DIR}/docs/badges/tests.svg
        -P ${PROJECT_SOURCE_DIR}/cmake/TestBadge.cmake
    DEPENDS ${testTargets}
    COMMENT "Running the test suite and writing docs/badges/tests.svg"
    VERBATIM)
