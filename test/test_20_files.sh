#!/bin/bash

# Test 20 TS files to verify no packet loss
# Environment variables:
#   TS_TEST_DIR - Override default test directory
#
# Get the project root directory (parent of test directory)
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Detect plugin location (check both possible build locations)
if [ -d "${PROJECT_ROOT}/build/tsfilter" ]; then
    export GST_PLUGIN_PATH=${PROJECT_ROOT}/build/tsfilter
    ANALYZER=${PROJECT_ROOT}/build/ts_analyzer
elif [ -d "${PROJECT_ROOT}/tsfilter/build" ]; then
    export GST_PLUGIN_PATH=${PROJECT_ROOT}/tsfilter/build
    ANALYZER=${PROJECT_ROOT}/build/ts_analyzer
else
    echo "Error: Cannot find tsfilter plugin build directory"
    exit 1
fi
PACKET_SIZE=188
FAILED=0
PASSED=0

# Test directory - can be overridden via TS_TEST_DIR environment variable
# Default to /tsduck-test (root) to find all files recursively
TEST_DIR="${TS_TEST_DIR:-/tsduck-test}"

# Get first 20 TS files from test directory and subdirectories
mapfile -t FILES < <(find "$TEST_DIR" -name "*.ts" -type f | sort | head -20)

echo "=========================================="
echo "Testing ${#FILES[@]} TS files for packet loss"
echo "=========================================="
echo ""

for i in "${!FILES[@]}"; do
    file="${FILES[$i]}"
    num=$((i + 1))
    basename=$(basename "$file")

    printf "[%2d/%2d] Testing %s...\n" "$num" "${#FILES[@]}" "$basename"

    # Clean up previous output
    rm -rf /tmp/tsout_test
    mkdir -p /tmp/tsout_test

    # Run analyzer (suppress stderr output for cleaner logs)
    $ANALYZER "$file" --output-dir /tmp/tsout_test --dump-all --dump-pids > /tmp/tsout_test/log.txt 2>&1

    # Check if output files exist
    if [ ! -f "/tmp/tsout_test/dump_all.ts" ]; then
        echo "  ✗ FAILED: No output generated"
        cat /tmp/tsout_test/log.txt | tail -20
        FAILED=$((FAILED + 1))
        continue
    fi

    # Count packets in input file
    input_size=$(stat -c%s "$file")
    input_packets=$((input_size / PACKET_SIZE))

    # Count packets in dump_all.ts
    dump_size=$(stat -c%s "/tmp/tsout_test/dump_all.ts")
    dump_packets=$((dump_size / PACKET_SIZE))

    # Count packets in all PID files
    pid_files=$(ls /tmp/tsout_test/pid_*.ts 2>/dev/null | wc -l)
    if [ "$pid_files" -gt 0 ]; then
        pid_total_size=$(stat -c%s /tmp/tsout_test/pid_*.ts | awk '{sum+=$1} END {print sum}')
        pid_packets=$((pid_total_size / PACKET_SIZE))
    else
        pid_packets=0
    fi

    # Verify byte-for-byte match
    if cmp -s "$file" "/tmp/tsout_test/dump_all.ts"; then
        match="✓"
    else
        match="✗"
    fi

    # Check for packet loss
    if [ "$input_packets" -eq "$dump_packets" ] && [ "$input_packets" -eq "$pid_packets" ] && [ "$match" = "✓" ]; then
        echo "  ✓ PASSED: $input_packets packets (dump: $dump_packets, PIDs: $pid_packets) $match"
        PASSED=$((PASSED + 1))
    else
        echo "  ✗ FAILED: Input=$input_packets, Dump=$dump_packets, PIDs=$pid_packets, Match=$match"
        FAILED=$((FAILED + 1))
    fi
done

echo ""
echo "=========================================="
echo "Test Results:"
echo "  Passed: $PASSED / ${#FILES[@]}"
echo "  Failed: $FAILED / ${#FILES[@]}"
echo "=========================================="

if [ $FAILED -eq 0 ]; then
    echo "✓ All tests passed!"
    exit 0
else
    echo "✗ Some tests failed"
    exit 1
fi
