cmake_minimum_required(VERSION 3.10)
project(tsfilter C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")

# Find GStreamer dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED
    gstreamer-1.0
    gstreamer-base-1.0
)

# Include directories
include_directories(
    ${GST_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Link directories
link_directories(${GST_LIBRARY_DIRS})

# Compiler definitions
add_definitions(${GST_CFLAGS_OTHER})

# Source files for tsfilter plugin
set(TSFILTER_SOURCES
    src/tsfilter.c
    src/tsfilter.h
    src/tsfilter_crc.c
    src/tsfilter_crc.h
    src/tsfilterplugin.c
)

# Build the plugin as a shared library
add_library(tsfilter SHARED
    ${TSFILTER_SOURCES}
)

# Link libraries
target_link_libraries(tsfilter
    ${GST_LIBRARIES}
)

# Set library output properties
set_target_properties(tsfilter PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libgsttsfilter"
    VERSION "1.0"
    SOVERSION "1"
)

# Installation - install to GStreamer's plugin directory
install(TARGETS tsfilter
    LIBRARY DESTINATION lib/gstreamer-1.0
)

# Print configuration
message(STATUS "GStreamer include dirs: ${GST_INCLUDE_DIRS}")
message(STATUS "GStreamer library dirs: ${GST_LIBRARY_DIRS}")
message(STATUS "GStreamer CFLAGS: ${GST_CFLAGS_OTHER}")
