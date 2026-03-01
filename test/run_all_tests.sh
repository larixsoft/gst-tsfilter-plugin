#!/bin/bash
# Test runner script for tsfilter comprehensive test
# Runs tests against 20+ diverse TS files in the test directory

# Get the project root directory (parent of test directory)
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Test directory - can be overridden via TS_TEST_DIR environment variable
TEST_DIR="${TS_TEST_DIR:-../tsduck-test/input}"
TEST_BIN="$PROJECT_ROOT/build/test/test_tsfilter_comprehensive"
PLUGIN_PATH="$PROJECT_ROOT/tsfilter/build"

# Check if test binary exists, build if needed
if [ ! -f "$TEST_BIN" ]; then
    echo "Test binary not found at: $TEST_BIN"
    echo "Building test..."
    cd "$PROJECT_ROOT"
    mkdir -p build && cd build
    cmake .. && make
    cd -
fi

# Set plugin path
export GST_PLUGIN_PATH="$PLUGIN_PATH:$GST_PLUGIN_PATH"

echo "=========================================="
echo "TSFilter Comprehensive Test Suite"
echo "Testing 20+ diverse TS files"
echo "=========================================="
echo ""

# Test 1: Run with no file (unit tests only)
echo "--- Unit Tests (No File) ---"
$TEST_BIN 2>&1 | grep -E "(PASS|FAIL|Total|Passed|Failed|Result)"

echo ""
echo "=========================================="
echo "File Processing Tests (24 files)"
echo "=========================================="

# Array of 20+ diverse test files to process
declare -a test_files=(
    "test-023a.ts:Single packet (188B)"
    "test-023b.ts:Two packets (376B)"
    "test-023c.ts:Three packets (564B)"
    "test-018.ts:Small file (19KB)"
    "test-024.ts:Medium-small (6.1KB)"
    "test-041.ts:Medium-small (3.9KB)"
    "test-032.1.ts:Fragment 1 (752B)"
    "test-032.2.ts:Fragment 2 (1.7KB)"
    "test-043.pmtvct.ts:PMT-VCT order (564B)"
    "test-043.vctpmt.ts:VCT-PMT order (564B)"
    "test-044.aa.ts:Audio-only (188B)"
    "test-026.ts:Medium file (977KB)"
    "test-030.ts:Medium file (211KB)"
    "test-012.ts:Medium file (365KB)"
    "test-025.ts:Medium file (1.8MB)"
    "test-028.ts:Large file (7.7MB)"
    "test-039.ts:Large file (7.7MB)"
    "test-010.ts:Large file (11MB)"
    "test-016.ts:Large file (11MB)"
    "test-013.ts:Large file (9.1MB)"
    "test-040.ts:Large file (20MB)"
    "test-001.ts:Large file (21MB)"
    "test-002.ts:Large file (21MB)"
    "test-003.ts:Large file (21MB)"
    "test-065.m2ts:M2TS 192-byte packets (10MB)"
)

# Run each test file
for test_entry in "${test_files[@]}"; do
    IFS=':' read -r filename description <<< "$test_entry"
    filepath="$TEST_DIR/$filename"

    if [ -f "$filepath" ]; then
        echo ""
        echo "--- Testing: $description ---"

        # Run just the file processing test
        GST_DEBUG=2 $TEST_BIN "$filepath" 2>&1 | \
            grep -A 5 "Process Valid TS File" | \
            grep -E "(PASS|FAIL|Processing|Total bad)"
    else
        echo ""
        echo "--- SKIP: $description (file not found: $filename) ---"
    fi
done

echo ""
echo "=========================================="
echo "Test Complete"
echo "=========================================="
