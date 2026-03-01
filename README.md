# TSFilter - GStreamer MPEG-TS PID Filter Plugin

[![CI](https://github.com/larixsoft/gst-tsfilter/actions/workflows/ci.yml/badge.svg)](https://github.com/larixsoft/gst-tsfilter/actions/workflows/ci.yml)
[![Docker](https://github.com/larixsoft/gst-tsfilter/actions/workflows/docker-publish.yml/badge.svg)](https://github.com/larixsoft/gst-tsfilter/actions/workflows/docker-publish.yml)

A GStreamer 1.0 element for filtering MPEG-TS (Transport Stream) packets by PID with advanced output options.

## Overview

**tsfilter** is a GStreamer plugin that provides flexible MPEG-TS PID filtering and extraction capabilities. It can:

- Filter packets by PID (allow list or deny list)
- Extract individual PIDs to separate output pads
- Pass through all packets unfiltered
- Auto-detect packet sizes (188, 192, 204, 208 bytes)
- Signal new PID discovery dynamically
- Validate continuity counters

This repository also includes:
- **ts_analyzer**: Demo application showing how to use the tsfilter plugin
- **Test Suite**: Comprehensive testing utilities

## Components

### tsfilter (GStreamer Plugin)

A GStreamer 1.0 element that filters MPEG-TS packets by PID with advanced features:

**Features:**
- Filter by PID (allow list or deny list modes)
- PID-specific output pads (`src_<PID>`) for extracting individual PIDs
- Unfiltered `dump` pad for pass-through mode
- Auto-detection of packet sizes (188, 192, 204, 208 bytes)
- Dynamic PID discovery via `new-pid` signal
- Bad packet detection and signaling
- Buffer pool optimization for performance

**Pad Types:**
- `sink` (request): Input MPEG-TS stream
- `src` (src): Filtered output (only specified PIDs)
- `dump` (src): Unfiltered output (all PIDs)
- `src_<PID>` (request): PID-specific outputs

**Properties:**

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `filter-pids` | GArray* | NULL | Array of PIDs to filter (NULL = pass all) |
| `invert-filter` | gboolean | FALSE | If TRUE, filter-pids is a deny list (exclude) |
| `packet-size` | guint | 188 | MPEG-TS packet size in bytes (188, 192, 204, 208) |
| `auto-detect` | gboolean | TRUE | Auto-detect packet size from stream |
| `emit-pid-signals` | gboolean | TRUE | Emit new-pid signals when PIDs discovered |
| `enable-dump` | gboolean | FALSE | Enable dump pad (unfiltered output) |
| `enable-crc-validation` | gboolean | FALSE | Validate CRC-32 in PSI/SI table packets |
| `enable-stats` | gboolean | FALSE | Collect detailed stream statistics |
| `bad-packet-count` | guint | 0 (read-only) | Total packets discarded due to bad sync |
| `stream-stats` | gpointer | NULL (read-only) | Comprehensive stream statistics pointer |

**Signals:**

| Signal | Parameters | Description |
|--------|-----------|-------------|
| `new-pid` | `pid` (guint) | Emitted when a new PID is discovered in the stream |
| `bad-packet` | `count` (guint) | Emitted when bad packets (invalid sync bytes) are detected |
| `packet-size-mismatch` | `user_size`, `detected_size` (guint) | Emitted when auto-detected size differs from user-specified |
| `dump-pad-added` | (none) | Emitted when dump pad is created (enable-dump set to TRUE) |
| `crc-error` | `count` (guint) | Emitted when CRC-32 validation fails (if enable-crc-validation is TRUE) |

### ts_analyzer (Demo Application)

Example command-line application demonstrating how to use the tsfilter plugin. It shows:

- How to create and configure tsfilter in C++
- How to use new-pid signals for dynamic PID discovery
- How to request and link PID-specific pads
- Two output modes: C++ file I/O and GStreamer filesink

**Usage:**
```bash
# Analyze file and show PID statistics
ts_analyzer input.ts

# Extract specific PIDs
ts_analyzer input.ts --filter-pids 0,1,256 --output-dir ./output

# Dump all packets (pass-through)
ts_analyzer input.ts --dump-all --output-dir ./output

# Extract all PIDs to separate files
ts_analyzer input.ts --dump-pids --output-dir ./output

# Use GStreamer filesink instead of C++ I/O
ts_analyzer input.ts --gs --output-dir ./output
```

**Note:** This is a reference implementation. See [app/main.cpp](app/main.cpp) for the complete example.

## Building

### Prerequisites

**For CMake (full build):**
- GCC/Clang with C++17 support
- CMake 3.10 or later
- GStreamer 1.0 development packages
- pkg-config

**For Meson (plugin only):**
- GCC/Clang with C11 support
- Meson 0.50 or later
- Ninja
- GStreamer 1.0 development packages

On Debian/Ubuntu:
```bash
sudo apt-get install build-essential cmake meson ninja-build \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good
```

### Build Steps

#### Option 1: CMake (Recommended - Full Project)

Builds the plugin, demo application, and test suite:

```bash
# Clone repository
git clone https://github.com/larixsoft/gst-tsfilter.git
cd gst-tsfilter

# Build all components
mkdir build && cd build
cmake ..
make

# Output binaries:
# - ts_analyzer (demo application)
# - test/test_tsfilter_comprehensive (C test suite)
# - test/ts_reporter (TS analysis utility)
```

#### Option 2: Meson (Plugin Only)

Builds only the GStreamer plugin:

```bash
# Clone repository
git clone https://github.com/larixsoft/gst-tsfilter.git
cd gst-tsfilter/tsfilter

# Setup build
meson setup build

# Compile
meson compile -C build

# The plugin will be at: build/libgsttsfilter.so
```

#### Option 3: Docker (Containerized)

Build and run in a Docker container with all dependencies (test files auto-cloned):

```bash
# Clone repository
git clone https://github.com/larixsoft/gst-tsfilter.git
cd gst-tsfilter

# Using run script (recommended)
cd docker
./run.sh build          # Build Docker image
./run.sh shell          # Start interactive shell
./run.sh test           # Run all tests
./run.sh quick-test     # Run quick test (20 files)
./run.sh inspect        # Inspect the plugin

# Using docker-compose
docker-compose -f docker/docker-compose.yml build
docker-compose -f docker/docker-compose.yml run --rm gst-tsfilter

# Or using docker directly
docker build -t gst-tsfilter -f docker/Dockerfile .
docker run -it --rm gst-tsfilter
```

See [docker/README.md](docker/README.md) for more details.

### Install (Optional)

**CMake build:**

```bash
# Install GStreamer plugin to system
cd build
sudo make install

# Or use GST_PLUGIN_PATH to load from build directory
export GST_PLUGIN_PATH=/path/to/gst-tsfilter/tsfilter/build
```

**Meson build:**

```bash
# Install GStreamer plugin to system
cd tsfilter
meson install -C build

# Or use GST_PLUGIN_PATH to load from build directory
export GST_PLUGIN_PATH=/path/to/gst-tsfilter/tsfilter/build
```

## Testing

### Quick Test with Docker (Recommended)

```bash
cd docker
./run.sh build          # Build Docker image (first time)
./run.sh quick-test     # Run 20-file quick test
./run.sh test           # Run all 281 tests
```

### Test Files Setup (Local)

Clone the tsduck-test repository for local testing:

```bash
# From project root
git clone https://github.com/tsduck/tsduck-test.git ../tsduck-test
```

### Run Tests (Local)

**Main test suite (all 281 files, 2 modes):**
```bash
cd test
./test_all_files.sh
```

**Quick test (20 files):**
```bash
cd test
./test_20_files.sh
```

**C test suite:**
```bash
cd test
./run_all_tests.sh
```

**TS file analysis:**
```bash
cd test
./analyze_ts_files.sh /path/to/file.ts
```

### Environment Variables

- `TS_TEST_DIR`: Override default test directory path
  ```bash
  TS_TEST_DIR=/path/to/tsduck-test/input ./test_all_files.sh
  ```

## CI/CD

The project uses GitHub Actions for continuous integration:

- **[CI Workflow](.github/workflows/ci.yml)**: Runs on every push and PR
  - Docker-based testing (Ubuntu 24.04)
  - Native CMake build (Ubuntu latest)
  - Meson build (plugin only)

- **[Docker Publish](.github/workflows/docker-publish.yml)**: Builds and publishes Docker images
  - Automatic builds on main branch
  - Versioned releases for tags
  - Published to `ghcr.io/larixsoft/gst-tsfilter`

### Using CI Docker Images

```bash
# Pull the latest CI-built image
docker pull ghcr.io/larixsoft/gst-tsfilter:latest

# Run tests with CI image
docker run --rm ghcr.io/larixsoft/gst-tsfilter \
  /src/build/test/test_tsfilter_comprehensive
```

See [.github/workflows/README.md](.github/workflows/README.md) for more details.

## Project Structure

```
gst-tsfilter/
├── tsfilter/                        # GStreamer plugin (main component)
│   ├── src/
│   │   ├── tsfilter.c              # Main plugin code
│   │   ├── tsfilter.h              # Header
│   │   ├── tsfilter_crc.c          # CRC calculations
│   │   ├── tsfilterplugin.c        # Plugin registration
│   │   └── config.h                # Plugin configuration
│   ├── CMakeLists.txt              # Plugin CMake config
│   └── meson.build                 # Plugin Meson config
├── app/                             # Demo application
│   ├── CMakeLists.txt              # App build config
│   └── main.cpp                     # ts_analyzer (example usage)
├── test/                            # Test suite
│   ├── CMakeLists.txt              # Test build config
│   ├── README.md                    # Test documentation
│   ├── test_all_files.sh           # Main test (281 files, 2 modes)
│   ├── test_20_files.sh            # Quick test (20 files)
│   ├── test_single.py              # Python test wrapper
│   ├── run_all_tests.sh            # C test runner
│   ├── analyze_ts_files.sh         # TS analysis wrapper
│   ├── test_tsfilter_comprehensive.c  # C test suite
│   └── ts_reporter.c               # TS analysis utility
├── docker/                          # Docker configuration
│   ├── Dockerfile                  # Container image definition
│   ├── docker-compose.yml          # Docker Compose config
│   ├── build.sh                    # Docker build script
│   ├── run.sh                      # Docker run script (build, test, shell, etc.)
│   └── README.md                   # Docker usage guide
├── .github/                         # GitHub Actions CI/CD
│   └── workflows/                  # CI workflow definitions
│       ├── ci.yml                  # Main CI workflow
│       ├── docker-publish.yml      # Docker image publishing
│       └── README.md               # Workflow documentation
├── build/                           # Build output (generated)
│   ├── ts_analyzer                 # Demo binary
│   └── test/                       # Test binaries
│       ├── test_tsfilter_comprehensive
│       └── ts_reporter
├── CMakeLists.txt                   # Root CMake config
├── .dockerignore                    # Docker build exclusions
└── README.md                        # This file
```

## GStreamer Pipeline Examples

### Extract PIDs to Separate Files

```bash
gst-launch-1.0 filesrc location=input.ts ! tsfilter name=filter \
  filter.src_0 ! queue ! filesink location=pid_0.ts \
  filter.src_1 ! queue ! filesink location=pid_1.ts
```

### Filter Specific PIDs

```bash
# Pass only PIDs 256 and 257
gst-launch-1.0 filesrc location=input.ts ! tsfilter name=filter \
  filter-pids=<256,257> ! queue ! filesink location=filtered.ts
```

### Pass-Through with Dump

```bash
# Get filtered output + complete dump
gst-launch-1.0 filesrc location=input.ts ! tsfilter name=filter \
  filter-pids=<256> ! queue ! filesink location=filtered.ts \
  filter.dump ! queue ! filesink location=complete.ts
```

## Technical Notes

### Queue Elements Required

When connecting tsfilter to sink elements, always use queue elements:

```c
/* CORRECT */
gst_pad_link(pid_pad, queue_sink_pad);
gst_pad_link(queue_src_pad, filesink_sink_pad);

/* WRONG - will block/hang */
gst_pad_link(pid_pad, filesink_sink_pad);
```

### Packet Size Detection

The plugin automatically detects MPEG-TS packet sizes:
- Standard TS: 188 bytes
- M2TS (Blu-ray): 192 bytes
- ATSC: 208 bytes
- DVB with FEC: 204 bytes

Detection uses sync byte (0x47) pattern matching at positions 0, packet_size, and 2*packet_size.

### Continuity Counter Validation

The plugin validates MPEG-TS continuity counters to detect packet loss. CC validation includes:
- Per-PID CC tracking
- CC rollover detection (0 → 15)
- Duplicate CC detection
- Gap detection for missing packets

## License

GNU Library General Public License v2 or later

## Contributing

Contributions are welcome! Please ensure:
- All tests pass (281 test files)
- Code follows project style
- New features include tests
- Documentation is updated

## See Also

- [test/README.md](test/README.md) - Detailed test documentation
- [GStreamer Documentation](https://gstreamer.freedesktop.org/documentation/)
- [MPEG-TS Specification](https://www.etsi.org/deliver/etsi_en/300400_300499/30046801/01.04.01_60/en_30046801v010401p.pdf)
