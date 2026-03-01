# Find additional GStreamer libraries needed by the app
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST_APP REQUIRED gstreamer-app-1.0)
pkg_check_modules(GST_MPEGTS REQUIRED gstreamer-mpegts-1.0)

# ts_analyzer application
add_executable(ts_analyzer main.cpp)
target_link_libraries(ts_analyzer
    ${GST_LIBRARIES}
    ${GST_APP_LIBRARIES}
    ${GST_MPEGTS_LIBRARIES}
)
target_include_directories(ts_analyzer PRIVATE
    ${GST_INCLUDE_DIRS}
    ${GST_APP_INCLUDE_DIRS}
    ${GST_MPEGTS_INCLUDE_DIRS}
)
target_link_options(ts_analyzer PRIVATE
    ${GST_LDFLAGS_OTHER}
    ${GST_APP_LDFLAGS_OTHER}
    ${GST_MPEGTS_LDFLAGS_OTHER}
)
target_link_directories(ts_analyzer PRIVATE
    ${GST_LIBRARY_DIRS}
    ${GST_APP_LIBRARY_DIRS}
    ${GST_MPEGTS_LIBRARY_DIRS}
)

# Set output directory
set_target_properties(ts_analyzer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
