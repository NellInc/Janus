#!/bin/bash
# Build NT5 WDM VxD using Docker + Open Watcom v2 + NASM
# Usage: ./build.sh [target]
#   ./build.sh          - full build (nt5)
#   ./build.sh v5       - legacy V5 raw build
#   ./build.sh v5small  - legacy V5 small build used by build_sysini_fixed.py
#   ./build.sh ntkshim  - compile single file
#   ./build.sh clean    - clean build artifacts
#   ./build.sh shell    - interactive shell in build container

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
IMAGE_NAME="nt5-vxd-builder"
TARGET="${1:-nt5}"

# Build Docker image if needed
if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
    echo "=== Building Docker image (first time only) ==="
    docker build -t "$IMAGE_NAME" "$SCRIPT_DIR"
fi

if [ "$TARGET" = "shell" ]; then
    echo "=== Interactive shell ==="
    docker run --rm -it \
        -v "$SCRIPT_DIR:/src" \
        -v "$REPO_ROOT:/repo" \
        -v "$REPO_ROOT/binaries:/binaries" \
        -e INCLUDE="/repo/reference/inc:/opt/watcom/h" \
        "$IMAGE_NAME" \
        /bin/bash
else
    echo "=== Building: make $TARGET ==="
    docker run --rm \
        -v "$SCRIPT_DIR:/src" \
        -v "$REPO_ROOT:/repo" \
        -v "$REPO_ROOT/binaries:/binaries" \
        -e INCLUDE="/repo/reference/inc:/opt/watcom/h" \
        "$IMAGE_NAME" \
        make "$TARGET" 2>&1

    if [ "$TARGET" = "nt5" ] && [ -f "$SCRIPT_DIR/NT5RAW.VXD" ]; then
        SIZE=$(stat -f%z "$SCRIPT_DIR/NT5RAW.VXD" 2>/dev/null || stat -c%s "$SCRIPT_DIR/NT5RAW.VXD" 2>/dev/null)
        echo ""
        echo "=== SUCCESS: NT5RAW.VXD ($SIZE bytes) ==="
        cp "$SCRIPT_DIR/NT5RAW.VXD" "$REPO_ROOT/binaries/NT5RAW.VXD"
        echo "Copied to binaries/NT5RAW.VXD"
    fi

    if [ "$TARGET" = "v5" ] && [ -f "$SCRIPT_DIR/V5RAW.VXD" ]; then
        SIZE=$(stat -f%z "$SCRIPT_DIR/V5RAW.VXD" 2>/dev/null || stat -c%s "$SCRIPT_DIR/V5RAW.VXD" 2>/dev/null)
        echo ""
        echo "=== SUCCESS: V5RAW.VXD ($SIZE bytes) ==="
        cp "$SCRIPT_DIR/V5RAW.VXD" "$REPO_ROOT/binaries/V5RAW.VXD"
        echo "Copied to binaries/V5RAW.VXD"
    fi

    if [ "$TARGET" = "v5small" ] && [ -f "$SCRIPT_DIR/V5SMALL.VXD" ]; then
        SIZE=$(stat -f%z "$SCRIPT_DIR/V5SMALL.VXD" 2>/dev/null || stat -c%s "$SCRIPT_DIR/V5SMALL.VXD" 2>/dev/null)
        echo ""
        echo "=== SUCCESS: V5SMALL.VXD ($SIZE bytes) ==="
        cp "$SCRIPT_DIR/V5SMALL.VXD" "$REPO_ROOT/binaries/V5SMALL.VXD"
        echo "Copied to binaries/V5SMALL.VXD"
    fi
fi
