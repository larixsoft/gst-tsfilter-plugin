#!/bin/bash
# Docker run script for gst-tsfilter
#
# This script provides convenient ways to run the gst-tsfilter Docker container
# with proper volume mappings for development and testing.

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Get the project root directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Image name
IMAGE_NAME="gst-tsfilter"
CONTAINER_NAME="gst-tsfilter-container"

# Print usage
print_usage() {
    cat << EOF
${GREEN}gst-tsfilter Docker Run Script${NC}

Usage: $0 [MODE]

Modes:
  build        Build the Docker image
  shell        Start an interactive shell in the container
  test         Run the full test suite (all 281 files)
  quick-test   Run quick test (20 files)
  analyze      Analyze a TS file (usage: $0 analyze <file.ts>)
  inspect      Run gst-inspect on the tsfilter plugin
  clean        Remove the Docker image
  help         Show this help message

Examples:
  $0 build              # Build the Docker image
  $0 shell              # Start interactive shell
  $0 test               # Run all tests
  $0 quick-test         # Run quick test
  $0 analyze input.ts   # Analyze a TS file
  $0 inspect            # Inspect the plugin

Volume Mappings:
  /src           -> Project source code (read-write)
  /tsduck-test   -> Test files (read-only, auto-cloned during build)

Environment Variables:
  GST_PLUGIN_PATH -> Set to plugin install location
  TS_TEST_DIR     -> Set to test files directory

EOF
}

# Build the Docker image
build_image() {
    echo -e "${GREEN}Building Docker image: ${IMAGE_NAME}${NC}"
    cd "$PROJECT_ROOT"
    docker build -t "$IMAGE_NAME" -f docker/Dockerfile .
    echo -e "${GREEN}Build complete!${NC}"
}

# Start interactive shell
run_shell() {
    echo -e "${GREEN}Starting interactive shell...${NC}"
    docker run -it --rm \
        --name "$CONTAINER_NAME" \
        -v "${PROJECT_ROOT}:/src" \
        -e GST_PLUGIN_PATH=/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0 \
        -e TS_TEST_DIR=/tsduck-test/input \
        -w /src \
        "$IMAGE_NAME" \
        /bin/bash
}

# Run all tests
run_tests() {
    echo -e "${GREEN}Running full test suite (281 files)...${NC}"
    docker run -it --rm \
        --name "$CONTAINER_NAME" \
        -v "${PROJECT_ROOT}:/src" \
        -e GST_PLUGIN_PATH=/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0 \
        -e TS_TEST_DIR=/tsduck-test/input \
        -w /src/test \
        "$IMAGE_NAME" \
        /bin/bash -c "./test_all_files.sh"
}

# Run quick test
run_quick_test() {
    echo -e "${GREEN}Running quick test (20 files)...${NC}"
    docker run -it --rm \
        --name "$CONTAINER_NAME" \
        -v "${PROJECT_ROOT}:/src" \
        -e GST_PLUGIN_PATH=/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0 \
        -e TS_TEST_DIR=/tsduck-test/input \
        -w /src/test \
        "$IMAGE_NAME" \
        /bin/bash -c "./test_20_files.sh"
}

# Analyze a TS file
analyze_file() {
    local file_path="$1"
    if [ -z "$file_path" ]; then
        echo -e "${RED}Error: Please specify a file to analyze${NC}"
        echo "Usage: $0 analyze <file.ts>"
        exit 1
    fi

    if [ ! -f "$file_path" ]; then
        echo -e "${RED}Error: File not found: $file_path${NC}"
        exit 1
    fi

    echo -e "${GREEN}Analyzing: $file_path${NC}"
    local file_dir=$(cd "$(dirname "$file_path")" && pwd)
    local file_name=$(basename "$file_path")

    docker run -it --rm \
        --name "$CONTAINER_NAME" \
        -v "${PROJECT_ROOT}:/src" \
        -v "${file_dir}:/input:ro" \
        -e GST_PLUGIN_PATH=/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0 \
        "$IMAGE_NAME" \
        /bin/bash -c "cd /src && ./build/ts_analyzer /input/$file_name"
}

# Inspect the plugin
inspect_plugin() {
    echo -e "${GREEN}Inspecting tsfilter plugin...${NC}"
    docker run -it --rm \
        --name "$CONTAINER_NAME" \
        -e GST_PLUGIN_PATH=/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0 \
        "$IMAGE_NAME" \
        gst-inspect-1.0 tsfilter
}

# Clean up
clean_image() {
    echo -e "${YELLOW}Removing Docker image: ${IMAGE_NAME}${NC}"
    docker rmi "$IMAGE_NAME" 2>/dev/null || echo -e "${RED}Image not found or already removed${NC}"
}

# Main script logic
case "${1:-help}" in
    build)
        build_image
        ;;
    shell)
        run_shell
        ;;
    test)
        run_tests
        ;;
    quick-test)
        run_quick_test
        ;;
    analyze)
        analyze_file "$2"
        ;;
    inspect)
        inspect_plugin
        ;;
    clean)
        clean_image
        ;;
    help|--help|-h)
        print_usage
        ;;
    *)
        echo -e "${RED}Unknown mode: $1${NC}"
        echo ""
        print_usage
        exit 1
        ;;
esac
