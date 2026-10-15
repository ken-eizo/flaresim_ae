#!/usr/bin/env bash
# build_all.sh - Build FlareSim.so for multiple Nuke versions on Linux
#
# Usage:
#   ./build_all.sh
#   ./build_all.sh --versions "13 14 15"        # build specific versions only
#   ./build_all.sh --nuke-root /usr/local        # override Nuke install root
#   ./build_all.sh --dist-dir /tmp/flaresim_dist # override output directory
#
# Output: dist/nuke13/FlareSim.so, dist/nuke14/FlareSim.so, etc.
#
# Compiler selection:
#   Different Nuke versions target different VFX Reference Platform years, which
#   require different compiler versions for ABI compatibility:
#
#     Nuke 13  (CY2020) — GCC 9.x  (old ABI: _GLIBCXX_USE_CXX11_ABI=0)
#     Nuke 14  (CY2022) — GCC 11.x (new ABI: _GLIBCXX_USE_CXX11_ABI=1)
#     Nuke 15  (CY2023) — GCC 11.x (new ABI)
#     Nuke 16  (CY2024) — GCC 11.x (new ABI)
#     Nuke 17  (CY2026) — GCC 14.x (new ABI)
#
#   The recommended approach is to use ASWF Docker containers, which provide
#   the exact correct compiler for each platform year.  Compiler selection is
#   the builder's environment responsibility — this script passes the correct
#   ABI flag but does not select the compiler.

set -euo pipefail

VERSIONS="13 14 15 16 17"
NUKE_ROOT="/usr/local"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="${SCRIPT_DIR}/dist"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --versions)  VERSIONS="$2";  shift 2 ;;
        --nuke-root) NUKE_ROOT="$2"; shift 2 ;;
        --dist-dir)  DIST_DIR="$2";  shift 2 ;;
        *)
            echo "Unknown argument: $1" >&2
            echo "Usage: $0 [--versions \"13 14 15 16 17\"] [--nuke-root /usr/local] [--dist-dir ./dist]" >&2
            exit 1 ;;
    esac
done

succeeded=()
failed=()
skipped=()

echo ""
echo "FlareSim multi-version build (Linux)"
echo "Output: ${DIST_DIR}"
echo ""

for VERSION in $VERSIONS; do

    echo "--- Nuke ${VERSION} ---"

    # Find Nuke installation (e.g. "Nuke16.0v1") — pick newest patch if multiple exist.
    NUKE_DIR=$(find "${NUKE_ROOT}" -maxdepth 1 -type d -name "Nuke${VERSION}.*" 2>/dev/null \
               | sort -V | tail -1)

    if [[ -z "${NUKE_DIR}" ]]; then
        echo "  Nuke ${VERSION} not found under ${NUKE_ROOT} — skipping."
        skipped+=("${VERSION}")
        continue
    fi

    NDK_ROOT="${NUKE_DIR}/include"
    NUKE_LIB_DIR="${NUKE_DIR}"

    if [[ ! -f "${NDK_ROOT}/DDImage/Iop.h" ]]; then
        echo "  NDK headers not found at ${NDK_ROOT} — skipping."
        skipped+=("${VERSION}")
        continue
    fi

    echo "  Found: ${NUKE_DIR}"

    # Nuke 17 targets VFX CY2026 which specifies C++20; all others use C++17.
    if [[ "${VERSION}" -ge 17 ]]; then
        CXX_STANDARD=20
    else
        CXX_STANDARD=17
    fi

    # Nuke 13 uses the old libstdc++ ABI (_GLIBCXX_USE_CXX11_ABI=0).
    # Building with the wrong ABI compiles and links fine but crashes at runtime.
    if [[ "${VERSION}" -le 13 ]]; then
        ABI_FLAG="-DCMAKE_CXX_FLAGS=-D_GLIBCXX_USE_CXX11_ABI=0"
    else
        ABI_FLAG="-DCMAKE_CXX_FLAGS=-D_GLIBCXX_USE_CXX11_ABI=1"
    fi

    BUILD_DIR="${SCRIPT_DIR}/build_nuke${VERSION}"

    echo "  Configuring..."

    if ! cmake \
        -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE=Release \
        "-DCMAKE_CXX_STANDARD=${CXX_STANDARD}" \
        "${ABI_FLAG}" \
        "-DNDK_ROOT=${NDK_ROOT}" \
        "-DNUKE_LIB_DIR=${NUKE_LIB_DIR}" \
        -S "${SCRIPT_DIR}" \
        -B "${BUILD_DIR}"; then
        echo "  CMake configure FAILED"
        failed+=("${VERSION}")
        continue
    fi

    echo "  Building..."
    if ! cmake --build "${BUILD_DIR}" --config Release -- -j"$(nproc)"; then
        echo "  Build FAILED"
        failed+=("${VERSION}")
        continue
    fi

    SO_SRC="${BUILD_DIR}/FlareSim.so"
    if [[ ! -f "${SO_SRC}" ]]; then
        echo "  FlareSim.so not found — build may have failed."
        failed+=("${VERSION}")
        continue
    fi

    OUT_DIR="${DIST_DIR}/nuke${VERSION}"
    mkdir -p "${OUT_DIR}"
    cp "${SO_SRC}" "${OUT_DIR}/FlareSim.so"

    echo "  OK -> ${OUT_DIR}/FlareSim.so"
    succeeded+=("${VERSION}")

done

echo ""
echo "--- Summary ---"
if [[ ${#succeeded[@]} -gt 0 ]]; then
    echo "  Built:   Nuke ${succeeded[*]}"
fi
if [[ ${#skipped[@]} -gt 0 ]]; then
    echo "  Skipped: Nuke ${skipped[*]}"
fi
if [[ ${#failed[@]} -gt 0 ]]; then
    echo "  Failed:  Nuke ${failed[*]}"
fi
echo ""

if [[ ${#failed[@]} -gt 0 ]]; then
    exit 1
else
    exit 0
fi
