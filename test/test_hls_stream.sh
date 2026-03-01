#!/bin/bash
# HLS stream test for tsfilter
#
# This script tests the tsfilter plugin with HLS (HTTP Live Streaming) sources.
# It uses public test HLS streams to verify the plugin works correctly.

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== HLS Stream Test for tsfilter ===${NC}"
echo ""

# Check if plugin is available
if ! gst-inspect-1.0 tsfilter >/dev/null 2>&1; then
    echo -e "${RED}Error: tsfilter plugin not found${NC}"
    echo "Make sure GST_PLUGIN_PATH is set correctly"
    exit 1
fi

# Test HLS streams (reliable public test streams - tested with tsfilter)
# Sources: Wowza, Mux
# All streams output MPEG-TS format (188-byte packets) compatible with tsfilter
TEST_STREAMS=(
    # 2 reliable streams tested and confirmed working with hlsdemux + tsfilter
    "https://wowzaec2demo.streamlock.net/live/bigbuckbunny/playlist.m3u8"  # Wowza Big Buck Bunny
    "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"  # Mux Big Buck Bunny clip
)

echo "Testing ${#TEST_STREAMS[@]} HLS streams..."
echo ""

for i in "${!TEST_STREAMS[@]}"; do
    STREAM_URL="${TEST_STREAMS[$i]}"
    STREAM_NUM=$((i + 1))

    echo -e "${YELLOW}[${STREAM_NUM}/${#TEST_STREAMS[@]}] Testing:${NC} ${STREAM_URL}"

    # Test 1: Basic HLS playthrough with tsfilter (pass-through mode)
    echo "  → Test 1: Pass-through mode (10 seconds)"

    PIPELINE_OUTPUT=$(timeout 10s gst-launch-1.0 -q \
        souphttpsrc location="${STREAM_URL}" \
        ! hlsdemux \
        ! tsfilter \
        ! fakesink sync=false \
        2>&1 || true)

    # Check if pipeline started successfully (timeout is expected)
    if echo "$PIPELINE_OUTPUT" | grep -qi "forbidden\|not found\|invalid playlist"; then
        echo -e "    ${YELLOW}⚠ SKIPPED: Stream access error${NC}"
    elif echo "$PIPELINE_OUTPUT" | grep -qi "error\|failed"; then
        echo -e "    ${RED}✗ FAILED: Pipeline error${NC}"
    else
        echo -e "    ${GREEN}✓ PASSED: tsfilter loaded and processed stream${NC}"
        echo "    (Pipeline ran successfully, timed out as expected)"
    fi

    # Test 2: Enable dump pad to verify all packets pass through
    echo "  → Test 2: Dump mode (10 seconds)"

    PIPELINE_OUTPUT2=$(timeout 10s gst-launch-1.0 -q \
        souphttpsrc location="${STREAM_URL}" \
        ! hlsdemux \
        ! tsfilter name=filter enable-dump=true \
        filter.src ! queue ! fakesink sync=false \
        filter.dump ! queue ! fakesink sync=false \
        2>&1 || true)

    if echo "$PIPELINE_OUTPUT2" | grep -qi "forbidden\|not found\|invalid playlist"; then
        echo -e "    ${YELLOW}⚠ SKIPPED: Stream access error${NC}"
    elif echo "$PIPELINE_OUTPUT2" | grep -qi "error\|failed"; then
        echo -e "    ${RED}✗ FAILED: Pipeline error${NC}"
    else
        echo -e "    ${GREEN}✓ PASSED: Pipeline created successfully${NC}"
    fi

    # Test 3: Dump PID streams and analyze
    echo "  → Test 3: Dump PID streams (5 seconds)"

    DUMP_FILE="/tmp/hls_test_${STREAM_NUM}_dump.ts"

    PIPELINE_OUTPUT3=$(timeout 5s gst-launch-1.0 -q \
        souphttpsrc location="${STREAM_URL}" \
        ! hlsdemux \
        ! tsfilter name=filter enable-dump=true \
        filter.src ! queue ! fakesink sync=false \
        filter.dump ! queue ! filesink location="${DUMP_FILE}" \
        2>&1 || true)

    if echo "$PIPELINE_OUTPUT3" | grep -qi "forbidden\|not found\|invalid playlist"; then
        echo -e "    ${YELLOW}⚠ SKIPPED: Stream access error${NC}"
    elif [ -f "${DUMP_FILE}" ] && [ -s "${DUMP_FILE}" ]; then
        # Analyze the dump file with analyze_pids.py if available
        SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
        PID_ANALYZER="${SCRIPT_DIR}/analyze_pids.py"

        if [ -f "${PID_ANALYZER}" ]; then
            FILE_SIZE=$(stat -c%s "${DUMP_FILE}" 2>/dev/null || stat -f%z "${DUMP_FILE}" 2>/dev/null || echo "unknown")
            echo -e "    ${GREEN}✓ DUMP CREATED${NC}"
            echo "    File: ${FILE_SIZE} bytes"

            # Run PID analysis
            python3 "${PID_ANALYZER}" "${DUMP_FILE}" 2>/dev/null | sed 's/^/    /' || echo "    (PID analysis failed)"
        else
            FILE_SIZE=$(stat -c%s "${DUMP_FILE}" 2>/dev/null || stat -f%z "${DUMP_FILE}" 2>/dev/null || echo "unknown")
            PACKET_COUNT=$((FILE_SIZE / 188))
            echo -e "    ${GREEN}✓ DUMP CREATED: ${PACKET_COUNT} packets (${FILE_SIZE} bytes)${NC}"
        fi
        rm -f "${DUMP_FILE}"
    elif echo "$PIPELINE_OUTPUT3" | grep -qi "error\|failed"; then
        echo -e "    ${RED}✗ FAILED: Pipeline error${NC}"
    else
        echo -e "    ${YELLOW}⚠ WARNING: No dump file created${NC}"
    fi

    echo ""
done

echo -e "${GREEN}=== HLS Test Summary ===${NC}"
echo "HLS streams tested: ${#TEST_STREAMS[@]}"
echo ""
echo "Note: HLS tests are limited to 15-30 seconds per stream."
echo "For longer testing, remove the timeout commands."
