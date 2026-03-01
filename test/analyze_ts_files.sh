#!/bin/bash
# TS File Analyzer - Wrapper script for ts_reporter
#
# Usage:
#   ./analyze_ts_files.sh [directory-or-file] [-v|--verbose]
#
# Examples:
#   ./analyze_ts_files.sh                    # Analyze all files in default test directory
#   ./analyze_ts_files.sh -v                 # Verbose mode with detailed reports
#   ./analyze_ts_files.sh /path/to/file.ts   # Analyze single file
#   ./analyze_ts_files.sh /path/to/dir       # Analyze directory
#
# Environment variables:
#   TS_TEST_DIR - Override default test directory

# Get the project root directory (parent of test directory)
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPORTER="$PROJECT_ROOT/build/test/ts_reporter"
DEFAULT_DIR="${TS_TEST_DIR:-../tsduck-test}"

# Check if reporter is compiled, build if needed
if [ ! -f "$REPORTER" ]; then
    echo "ts_reporter not found at: $REPORTER"
    echo "Building test applications..."
    cd "$PROJECT_ROOT"
    mkdir -p build && cd build
    cmake .. && make
    cd -
    if [ ! -f "$REPORTER" ]; then
        echo "Error: Failed to build ts_reporter"
        exit 1
    fi
fi

# Parse arguments
VERBOSE=""
TARGET=""

if [ $# -eq 0 ]; then
    TARGET="$DEFAULT_DIR"
else
    if [ "$1" = "-v" ] || [ "$1" = "--verbose" ]; then
        VERBOSE="-v"
        TARGET="$DEFAULT_DIR"
    else
        TARGET="$1"
        if [ "$2" = "-v" ] || [ "$2" = "--verbose" ]; then
            VERBOSE="-v"
        fi
    fi
fi

# Run reporter
echo "Running TS File Reporter..."
echo "Target: $TARGET"
$REPORTER "$TARGET" $VERBOSE

# Save report if in non-verbose mode
if [ -z "$VERBOSE" ]; then
    echo ""
    echo "Full report saved to: /tmp/ts_analysis_report.txt"
fi
