# Dockerfile for gst-tsfilter
# Base image: Ubuntu 24.04 (Noble)
#
# Build and run:
#   docker build -t gst-tsfilter -f docker/Dockerfile .
#   docker run -it --rm gst-tsfilter
#
# Mount local files for development:
#   docker run -it --rm -v $(pwd):/src gst-tsfilter bash

FROM ubuntu:24.04

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies and GStreamer
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    meson \
    ninja-build \
    pkg-config \
    git \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-tools \
    gstreamer1.0-x \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /src

# Copy project files
COPY . /src/

# Build the project with CMake
RUN mkdir -p build && cd build && \
    cmake .. && \
    make && \
    make install

# Set GStreamer plugin path (includes both system and install locations)
ENV GST_PLUGIN_PATH=/usr/local/lib/gstreamer-1.0:/gstreamer-1.0:/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0:/usr/lib/x86_64-linux-gnu/gstreamer-1.0

# Set up test file directory and clone tsduck-test repository
RUN mkdir -p /tsduck-test && \
    git clone https://github.com/tsduck/tsduck-test.git /tsduck-test

# Default working directory when container starts
WORKDIR /src

# Default command: start a bash shell
CMD ["/bin/bash"]
