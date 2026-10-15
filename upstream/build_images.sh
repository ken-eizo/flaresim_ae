#!/usr/bin/env bash
# build_images.sh — Build custom FlareSim Docker images (ASWF + CUDA 12.8).
#
# Run this once before docker_build_all.sh.  After the first build the layers
# are cached, so re-runs are instant unless the Dockerfile changes.
#
# Usage:
#   ./build_images.sh              # build images for Nuke 14-17
#   ./build_images.sh --push       # also push to a registry (set REGISTRY below)
#
# Images produced:
#   flaresim-build:nuke14  (aswf/ci-vfxall:2022 + CUDA 12.8)
#   flaresim-build:nuke15  (aswf/ci-vfxall:2023 + CUDA 12.8)
#   flaresim-build:nuke16  (aswf/ci-vfxall:2024 + CUDA 12.8)
#   flaresim-build:nuke17  (aswf/ci-vfxall:2025 + CUDA 12.8)
#
# Nuke 13 uses aswf/ci-vfxall:2020 (GCC 6.3 / CUDA 10.2) unchanged — that
# toolchain is too old for CUDA 12.x.

set -euo pipefail

PUSH=false
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --push) PUSH=true; shift ;;
        *)
            echo "Unknown argument: $1" >&2
            echo "Usage: $0 [--push]" >&2
            exit 1 ;;
    esac
done

declare -A BASE_IMAGE=(
    [14]="aswf/ci-vfxall:2022"
    [15]="aswf/ci-vfxall:2023"
    [16]="aswf/ci-vfxall:2024"
    [17]="aswf/ci-vfxall:2025"
)

echo ""
echo "Building FlareSim Docker images (ASWF + CUDA 12.8)"
echo ""

for VERSION in 14 15 16 17; do
    TAG="flaresim-build:nuke${VERSION}"
    BASE="${BASE_IMAGE[$VERSION]}"

    echo "--- ${TAG} (base: ${BASE}) ---"
    docker build \
        --build-arg BASE_IMAGE="${BASE}" \
        -t "${TAG}" \
        "${SCRIPT_DIR}/docker"

    if [[ "${PUSH}" == "true" ]]; then
        docker push "${TAG}"
    fi

    echo ""
done

echo "Done. Images built:"
for VERSION in 14 15 16 17; do
    docker images --format "  {{.Repository}}:{{.Tag}}  {{.Size}}" "flaresim-build:nuke${VERSION}"
done
echo ""
