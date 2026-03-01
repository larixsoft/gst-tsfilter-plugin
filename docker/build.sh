#!/bin/bash
# Docker build script for gst-tsfilter
#
# This script builds the Docker image for gst-tsfilter with proper
# exclusions for local build artifacts.

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
IMAGE_TAG="${IMAGE_TAG:-latest}"

echo -e "${GREEN}=== Building gst-tsfilter Docker Image ===${NC}"
echo ""
echo -e "Project root: ${PROJECT_ROOT}"
echo -e "Image name: ${IMAGE_NAME}:${IMAGE_TAG}"
echo ""

# Change to project root
cd "$PROJECT_ROOT"

# Check if .dockerignore exists
if [ ! -f .dockerignore ]; then
    echo -e "${YELLOW}Warning: .dockerignore not found${NC}"
fi

# Build the image
echo -e "${GREEN}Building Docker image...${NC}"
docker build \
    --tag "${IMAGE_NAME}:${IMAGE_TAG}" \
    --tag "${IMAGE_NAME}:latest" \
    -f docker/Dockerfile \
    .

# Check if build was successful
if [ $? -eq 0 ]; then
    echo ""
    echo -e "${GREEN}=== Build successful! ===${NC}"
    echo ""
    echo "Image tags:"
    echo "  - ${IMAGE_NAME}:${IMAGE_TAG}"
    echo "  - ${IMAGE_NAME}:latest"
    echo ""
    echo "To run the container:"
    echo "  docker run -it --rm ${IMAGE_NAME}"
    echo ""
    echo "Or use the run script:"
    echo "  cd docker && ./run.sh shell"
else
    echo ""
    echo -e "${RED}=== Build failed! ===${NC}"
    exit 1
fi
