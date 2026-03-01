# Test Suite

Test scripts and utilities for gst-tsfilter.

## Quick Start with Docker

The easiest way to run tests is using Docker:

```bash
cd docker
./run.sh build          # Build Docker image (first time only)
./run.sh quick-test     # Run 20-file quick test
./run.sh test           # Run all 281 tests
```

See [docker/README.md](../docker/README.md) for more Docker options.

## Test Files Setup

### Local Testing

Before running tests locally, you need to clone the tsduck-test repository:

```bash
# Clone the test file repository (from project root)
git clone https://github.com/tsduck/tsduck-test.git ../tsduck-test

# Or update if you already have it cloned
cd ../tsduck-test
git pull
```

The test scripts expect files at `../tsduck-test` relative to the project root,
or you can set the `TS_TEST_DIR` environment variable.

### Docker Testing

With Docker, test files are automatically cloned during image build at `/tsduck-test`.

## Running Tests

### Option 1: Docker (Recommended)

```bash
cd docker
./run.sh build          # Build image (first time)
./run.sh quick-test     # Quick test (20 files)
./run.sh test           # Full test suite (281 files)
```

### Option 2: Local (Native)

```bash
cd test
./test_20_files.sh      # Quick test
./test_all_files.sh     # Full test suite
```

## Main Test Scripts

### test_all_files.sh
Tests all 281 TS files from the tsduck-test repository for packet loss.
Tests both default mode (C++ file I/O) and --gs mode (GStreamer filesink).

**Usage:**
```bash
cd test
./test_all_files.sh
```

**What it tests:**
- Packet loss detection for all 281 test files
- Both C++ file I/O mode (default) and GStreamer filesink mode (--gs)
- Byte-for-byte verification of pass-through mode
- PID file packet counts vs input file packet counts

**Output:**
- Pass/fail status for each file
- Summary of total passed/failed tests
- Returns exit code 0 if all pass, 1 if any fail

### test_20_files.sh
Quick test that runs a subset of 20 representative TS files.
Useful for quick validation during development.

**Usage:**
```bash
cd test
./test_20_files.sh
```

### test_single.py
Python wrapper that monitors test_all_files.sh output for a specific file.
Useful for debugging issues with a particular test file.

**Usage:**
```bash
cd test
./test_single.py
```

## C/C++ Test Applications

### test_tsfilter_comprehensive
Comprehensive C test suite for the tsfilter GStreamer plugin.

**Build:**
```bash
# From project root
mkdir -p build && cd build
cmake ..
make

# The binary will be at: build/test/test_tsfilter_comprehensive
```

**Run:**
```bash
# Without test file (runs unit tests only)
export GST_PLUGIN_PATH=../tsfilter/build
./build/test/test_tsfilter_comprehensive

# With test file (runs unit tests + file processing tests)
export GST_PLUGIN_PATH=../tsfilter/build
./build/test/test_tsfilter_comprehensive /path/to/test.ts
```

**What it tests:**
- Element creation and property configuration
- Signal connectivity (bad-packet, packet-size-mismatch, new-pid, dump-pad-added)
- PID filtering (allow list and deny list modes)
- Packet size configuration and auto-detection
- Multiple concurrent instances
- enable-dump property and dump pad functionality
- Pass-through mode (enable-dump=FALSE, no filter-pids)
- new-pid signal for dynamic PID discovery
- Various packet sizes (188, 192, 204, 208 bytes)

**Docker:**
```bash
cd docker
./run.sh shell
cd /src && ./build/test/test_tsfilter_comprehensive /tsduck-test/input/test-001.ts
```

### run_all_tests.sh
Runs the C test suite against multiple test files from tsduck-test.

**Usage:**
```bash
cd test
./run_all_tests.sh
```

**What it tests:**
- Unit tests (27 tests without file input)
- File processing tests (24 different test files)
- Various file sizes (188 bytes to 21 MB)
- Different packet formats (standard TS, M2TS with 192-byte packets)

## Utilities

### ts_reporter
Analyzes TS files and generates reports on packet structure and PIDs.

**Build:**
```bash
# From project root
mkdir -p build && cd build
cmake ..
make

# The binary will be at: build/test/ts_reporter
```

**Run:**
```bash
# Analyze single file
./build/test/ts_reporter /path/to/file.ts

# Analyze directory
./build/test/ts_reporter /path/to/ts_files/

# Verbose mode with detailed reports
./build/test/ts_reporter -v /path/to/file.ts
```

**What it reports:**
- File size and packet count
- Detected packet size (188, 192, 204, or 208 bytes)
- List of all PIDs found with packet counts
- Valid sync byte count
- Continuity counter errors
- File structure analysis

### analyze_ts_files.sh
Wrapper script for ts_reporter that provides convenient analysis.

**Usage:**
```bash
cd test
# Analyze default test directory
./analyze_ts_files.sh

# Analyze specific file
./analyze_ts_files.sh /path/to/file.ts

# Analyze directory
./analyze_ts_files.sh /path/to/dir

# Verbose mode
./analyze_ts_files.sh -v
```

### test_hls_stream.sh

Tests the tsfilter plugin with HLS (HTTP Live Streaming) sources.

**Usage:**

```bash
cd test
# Run HLS stream tests (requires internet connection)
GST_PLUGIN_PATH=../build/tsfilter ./test_hls_stream.sh
```

**What it tests:**

- **Test 1: Pass-through mode** - Verifies tsfilter processes HLS streams without errors
- **Test 2: Dump mode** - Verifies the dump pad successfully captures all packets
- **Test 3: PID dump & analysis** - Captures packets to file and analyzes PID distribution

**Test Streams:**

- Wowza Big Buck Bunny (reliable public HLS stream)
- Mux Big Buck Bunny clip (alternative test stream)

**Output includes:**

- Packet counts per PID
- Percentage distribution
- Identification of common PIDs (PAT, PMT, audio, video)

**Note:** HLS tests are time-limited (5-10 seconds per stream) and require internet access. These tests verify tsfilter works with real-world streaming sources.

### analyze_pids.py

Python utility for analyzing PID distribution in MPEG-TS dump files.

**Usage:**

```bash
# Analyze a TS dump file
python3 analyze_pids.py /path/to/dump.ts

# Or use with test_hls_stream.sh output
python3 analyze_pids.py /tmp/hls_test_1_dump.ts
```

**What it reports:**

- Total packets processed
- Unique PIDs found
- Packet count and percentage for each PID
- PID values in both decimal and hexadecimal

**Common PIDs:**

- PID 0 (0x0000): PAT - Program Association Table
- PID 17 (0x0011): SDT/Other SI - Service Description Table
- PID 256 (0x0100): PMT - Program Map Table
- PID 257+ (0x0101+): Audio/Video streams

## Project Structure

```
test/
├── README.md                          # This file
├── CMakeLists.txt                     # Test build configuration
├── test_all_files.sh                  # Main test script (281 files, 2 modes)
├── test_20_files.sh                   # Quick test (20 files)
├── test_single.py                     # Python test wrapper
├── run_all_tests.sh                   # C test runner script
├── analyze_ts_files.sh                # TS analysis wrapper
├── test_hls_stream.sh                 # HLS stream testing with PID dump
├── analyze_pids.py                    # PID analysis utility
├── test_tsfilter_comprehensive.c      # C test suite source
└── ts_reporter.c                      # TS analysis utility source
```

Test binaries are built in `build/test/` directory by CMake.

## Environment Variables

- `TS_TEST_DIR` - Override default test directory path
  ```bash
  TS_TEST_DIR=/path/to/tsduck-test/input ./test_all_files.sh
  ```

- `GST_PLUGIN_PATH` - GStreamer plugin search path (for local testing)
  ```bash
  export GST_PLUGIN_PATH=/usr/local/lib/gstreamer-1.0
  ```

## Test Results

All 281 test files pass successfully:
- ✓ No packet loss in pass-through mode
- ✓ No packet loss in filtered mode
- ✓ Byte-for-byte verification
- ✓ Continuity counter validation
- ✓ Multiple packet sizes (188, 192, 204, 208 bytes)

For detailed test results, see the test script output.
