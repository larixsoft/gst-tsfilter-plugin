# Docker for gst-tsfilter-plugin

This directory contains Docker configuration for building and running the gst-tsfilter-plugin project in a containerized environment.

## Quick Start

### Option 1: Using the Run Script (Recommended)

The run script provides convenient commands for common tasks:

```bash
# From project root
cd docker

# Build the Docker image
./run.sh build

# Start interactive shell
./run.sh shell

# Run tests
./run.sh test          # All 281 files
./run.sh quick-test    # Quick test (20 files)

# Analyze a TS file
./run.sh analyze /path/to/file.ts

# Inspect the plugin
./run.sh inspect

# Show all options
./run.sh help
```

### Option 2: Using Docker Compose

```bash
# From project root
docker-compose -f docker/docker-compose.yml build
docker-compose -f docker/docker-compose.yml run --rm gst-tsfilter
```

### Option 3: Using Docker Directly

```bash
# From project root
docker build -t gst-tsfilter -f docker/Dockerfile .
docker run -it --rm gst-tsfilter
```

## Run Script Options

| Command | Description |
|---------|-------------|
| `./run.sh build` | Build the Docker image |
| `./run.sh shell` | Start interactive shell in container |
| `./run.sh test` | Run full test suite (281 files) |
| `./run.sh quick-test` | Run quick test (20 files) |
| `./run.sh analyze <file>` | Analyze a TS file |
| `./run.sh inspect` | Run gst-inspect on tsfilter plugin |
| `./run.sh clean` | Remove the Docker image |
| `./run.sh help` | Show help message |

## Volume Mappings

When using the run script or docker-compose:

- `/src` → Project source code (read-write, for development)
- `/tsduck-test` → Test files (read-only, auto-cloned during build)

## Environment Variables

- `GST_PLUGIN_PATH` → GStreamer plugin installation path (`/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0`)
- `TS_TEST_DIR` → Test files directory (`/tsduck-test/input`)

## Container Details

- **Base Image**: ubuntu:24.04
- **Installed Dependencies**:
  - build-essential, cmake, meson, ninja-build
  - GStreamer 1.0 development packages
  - All GStreamer plugins (base, good, bad)
- **Working Directory**: `/src`
- **Test Files**: Auto-cloned from https://github.com/tsduck/tsduck-test.git during build

## Interactive Usage

Start a shell in the container:

```bash
./run.sh shell
```

Inside the container, you can:

```bash
# Run ts_analyzer
./build/ts_analyzer --help

# Run C test suite
cd /src/test
./run_all_tests.sh

# Use gst-launch to test the plugin
gst-inspect-1.0 tsfilter
```

## Development Mode

Mount your local source directory to make changes without rebuilding:

```bash
docker run -it --rm \
    -v $(pwd):/src \
    -e GST_PLUGIN_PATH=/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0 \
    gst-tsfilter bash
```

Note: The run script automatically mounts the project directory, so `./run.sh shell` is ready for development.
