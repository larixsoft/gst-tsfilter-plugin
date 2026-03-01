cmake_minimum_required(VERSION 3.10)
project(gst_tsfilter)

set(CMAKE_CXX_STANDARD 17)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0 gstreamer-mpegts-1.0)

include_directories(${GST_INCLUDE_DIRS})
link_directories(${GST_LIBRARY_DIRS})
add_definitions(${GST_CFLAGS_OTHER})

# Build tsfilter plugin
add_subdirectory(tsfilter)

# Build ts_analyzer application from app/ directory
add_subdirectory(app)

# Build test applications
add_subdirectory(test)

