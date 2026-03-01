# Test applications

# Find GStreamer for test apps
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST_TEST REQUIRED gstreamer-1.0)

# Output directory for test binaries
set(TEST_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test)
file(MAKE_DIRECTORY ${TEST_OUTPUT_DIR})

# test_tsfilter_comprehensive - comprehensive C test suite
add_executable(test_tsfilter_comprehensive test_tsfilter_comprehensive.c)
target_link_libraries(test_tsfilter_comprehensive ${GST_TEST_LIBRARIES})
target_include_directories(test_tsfilter_comprehensive PRIVATE ${GST_TEST_INCLUDE_DIRS})
target_compile_options(test_tsfilter_comprehensive PRIVATE ${GST_TEST_CFLAGS_OTHER})
set_target_properties(test_tsfilter_comprehensive PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${TEST_OUTPUT_DIR}
)

# ts_reporter - TS file analysis utility
add_executable(ts_reporter ts_reporter.c)
set_target_properties(ts_reporter PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${TEST_OUTPUT_DIR}
)

# Installation (optional)
install(TARGETS test_tsfilter_comprehensive ts_reporter
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
