#!/bin/bash

# Test ALL TS files to verify no packet loss
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

# Test modes: default (C++ file I/O) and --gs (GStreamer filesink)
MODES=("default" "--gs")
OVERALL_PASSED=0
OVERALL_FAILED=0

# Test directory - can be overridden via TS_TEST_DIR environment variable
# Default to /tsduck-test (root) to find all files recursively
TEST_DIR="${TS_TEST_DIR:-/tsduck-test}"

# Get all TS files from test directory and subdirectories
mapfile -t FILES < <(find "$TEST_DIR" -name "*.ts" -type f | sort)

echo "=========================================="
echo "Testing ALL ${#FILES[@]} TS files for packet loss"
echo "Modes: ${MODES[*]}"
echo "=========================================="
echo ""

for MODE in "${MODES[@]}"; do
    echo "=========================================="
    echo "Testing mode: $MODE"
    echo "=========================================="
    echo ""

    for i in "${!FILES[@]}"; do
    file="${FILES[$i]}"
    num=$((i + 1))
    basename=$(basename "$file")

    printf "[%3d/%3d] Testing %s [$MODE]...\n" "$num" "${#FILES[@]}" "$basename"

    # Clean up previous output
    rm -rf /tmp/tsout_test
    mkdir -p /tmp/tsout_test

    # Run analyzer (suppress stderr output for cleaner logs)
    if [ "$MODE" = "default" ]; then
        timeout 30s $ANALYZER "$file" --output-dir /tmp/tsout_test --dump-all --dump-pids > /tmp/tsout_test/log.txt 2>&1
    else
        timeout 30s $ANALYZER "$file" --output-dir /tmp/tsout_test --dump-all --dump-pids $MODE > /tmp/tsout_test/log.txt 2>&1
    fi
    ret=$?

    # Check for timeout
    if [ $ret -eq 124 ]; then
        echo "  ✗ FAILED: Timeout after 30 seconds"
        FAILED=$((FAILED + 1))
        continue
    fi

    # Check if output files exist
    if [ ! -f "/tmp/tsout_test/dump_all.ts" ]; then
        echo "  ✗ FAILED: No output generated (exit code: $ret)"
        FAILED=$((FAILED + 1))
        continue
    fi

    # Detect packet size from file size
    # Check which packet size divides the file size evenly
    input_size=$(stat -c%s "$file")
    dump_size=$(stat -c%s "/tmp/tsout_test/dump_all.ts")

    # Determine actual packet size by checking divisibility
    remainder_188=$((input_size % 188))
    remainder_192=$((input_size % 192))
    remainder_204=$((input_size % 204))

    # If multiple sizes divide evenly, check which has more valid sync bytes
    valid_188=0
    valid_192=0
    valid_204=0

    if [ "$remainder_188" -eq 0 ]; then
        valid_188=$(python3 -c "with open('$file', 'rb') as f: data=f.read(); print(sum(1 for i in range(0, len(data), 188) if data[i]==0x47))")
    fi
    if [ "$remainder_192" -eq 0 ]; then
        valid_192=$(python3 -c "with open('$file', 'rb') as f: data=f.read(); print(sum(1 for i in range(0, len(data), 192) if data[i]==0x47))")
    fi
    if [ "$remainder_204" -eq 0 ]; then
        valid_204=$(python3 -c "with open('$file', 'rb') as f: data=f.read(); print(sum(1 for i in range(0, len(data), 204) if data[i]==0x47))")
    fi

    # Choose packet size with most valid sync bytes
    if [ "$valid_188" -ge "$valid_192" ] && [ "$valid_188" -ge "$valid_204" ] && [ "$remainder_188" -eq 0 ]; then
        PACKET_SIZE=188
        input_packets=$((input_size / 188))
        dump_packets=$((dump_size / 188))
        valid_sync_count=$valid_188
    elif [ "$valid_192" -ge "$valid_188" ] && [ "$valid_192" -ge "$valid_204" ] && [ "$remainder_192" -eq 0 ]; then
        PACKET_SIZE=192
        input_packets=$((input_size / 192))
        dump_packets=$((dump_size / 192))
        valid_sync_count=$valid_192
    elif [ "$remainder_204" -eq 0 ]; then
        PACKET_SIZE=204
        input_packets=$((input_size / 204))
        dump_packets=$((dump_size / 204))
        valid_sync_count=$valid_204
    else
        # No exact match, default to 188
        PACKET_SIZE=188
        input_packets=$((input_size / 188))
        dump_packets=$((dump_size / 188))
        valid_sync_count=$(python3 -c "with open('$file', 'rb') as f: data=f.read(); print(sum(1 for i in range(0, len(data), 188) if data[i]==0x47))")
    fi

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
    # Note: valid_sync_count may be less than input_packets if file has corrupted sync bytes
    # PID files should only contain valid packets with sync byte 0x47
    # dump_all.ts should always match input file (pass-through mode)
    if [ "$input_packets" -eq "$dump_packets" ] && [ "$valid_sync_count" -eq "$pid_packets" ] && [ "$match" = "✓" ]; then
        echo "  ✓ PASSED: $input_packets packets (valid: $valid_sync_count, dump: $dump_packets, PIDs: $pid_packets) $match"
        PASSED=$((PASSED + 1))
    else
        echo "  ✗ FAILED: Input=$input_packets, Valid=$valid_sync_count, Dump=$dump_packets, PIDs=$pid_packets, Match=$match"
        FAILED=$((FAILED + 1))
    fi
done  # End of file loop

    echo ""
    echo "=========================================="
    echo "Mode [$MODE] Results:"
    echo "  Passed: $PASSED / ${#FILES[@]}"
    echo "  Failed: $FAILED / ${#FILES[@]}"
    echo "=========================================="
    echo ""

    # Track overall results
    if [ $PASSED -eq ${#FILES[@]} ]; then
        OVERALL_PASSED=$((OVERALL_PASSED + 1))
    else
        OVERALL_FAILED=$((OVERALL_FAILED + 1))
    fi

    # Reset counters for next mode
    PASSED=0
    FAILED=0
done  # End of mode loop

echo "=========================================="
echo "Overall Test Results:"
echo "  Modes passed: $OVERALL_PASSED / ${#MODES[@]}"
echo "  Modes failed: $OVERALL_FAILED / ${#MODES[@]}"
echo "=========================================="

if [ $OVERALL_FAILED -eq 0 ]; then
    echo "✓ All modes passed!"
    exit 0
else
    echo "✗ Some modes failed"
    exit 1
fi
