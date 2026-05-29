# All test translation units are compiled into one binary called tests. The file
# glob uses CONFIGURE_DEPENDS so newly added test files are picked up on the next
# build without a manual reconfigure.
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)

if(TEST_SOURCES)
    add_executable(tests ${TEST_SOURCES})
    target_link_libraries(tests PRIVATE nano_core GTest::gtest_main)
    set_target_properties(tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    include(GoogleTest)
    gtest_discover_tests(tests)
endif()
