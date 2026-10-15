#!/usr/bin/env bash
# docker_build_all.sh — Build FlareSim.so for Nuke 13–17 inside ASWF Docker containers.
#
# Each Nuke version targets a different VFX Reference Platform year and must be
# compiled inside the matching ASWF container to get the correct GCC and ABI.
#
# All ASWF containers already ship nvcc — just not on PATH.  This script locates
# the existing CUDA installation and sets PATH accordingly.  It also restricts
# CMAKE_CUDA_ARCHITECTURES to what the container's CUDA version actually supports
# (sm_120/Blackwell requires CUDA 12.8+, which the ASWF containers do not yet ship).
#
# Usage:
#   ./docker_build_all.sh                          # build all versions
#   ./docker_build_all.sh --versions "16"          # build one version
#   ./docker_build_all.sh --nuke-root /usr/local   # override Nuke install root

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

# ---------------------------------------------------------------------------
# Per-version build parameters
# ---------------------------------------------------------------------------
# Nuke 13 uses the stock ASWF image (GCC 6.3 / CUDA 10.2 — too old for CUDA 12.x).
# Nuke 14-17 use custom images (ASWF + CUDA 12.8) built by build_images.sh,
# which adds sm_120 (Blackwell / RTX 5000) support.
declare -A ASWF_IMAGE=(
    [13]="aswf/ci-vfxall:2020"
    [14]="flaresim-build:nuke14"
    [15]="flaresim-build:nuke15"
    [16]="flaresim-build:nuke16"
    [17]="flaresim-build:nuke17"
)
declare -A CXX_ABI=(
    [13]="0"
    [14]="1"  [15]="1"  [16]="1"  [17]="1"
)
declare -A CXX_STD=(
    [13]="17" [14]="17" [15]="17" [16]="17"
    [17]="20"
)

succeeded=()
failed=()
skipped=()

# Warn if any required custom image is missing (user needs to run build_images.sh first).
MISSING_IMAGES=()
for V in 14 15 16 17; do
    if ! docker image inspect "flaresim-build:nuke${V}" &>/dev/null; then
        MISSING_IMAGES+=("flaresim-build:nuke${V}")
    fi
done
if [[ ${#MISSING_IMAGES[@]} -gt 0 ]]; then
    echo ""
    echo "WARNING: The following custom build images are missing:"
    for IMG in "${MISSING_IMAGES[@]}"; do echo "  ${IMG}"; done
    echo "  Run ./build_images.sh first to create them."
    echo ""
fi

echo ""
echo "FlareSim Docker multi-version build"
echo "Output: ${DIST_DIR}"
echo ""

for VERSION in $VERSIONS; do

    echo "=== Nuke ${VERSION} (${ASWF_IMAGE[$VERSION]}) ==="

    # Locate newest patch install, e.g. /usr/local/Nuke16.1v1
    NUKE_DIR=$(find "${NUKE_ROOT}" -maxdepth 1 -type d -name "Nuke${VERSION}.*" 2>/dev/null \
               | sort -V | tail -1)

    if [[ -z "${NUKE_DIR}" ]]; then
        echo "  Nuke ${VERSION} not found under ${NUKE_ROOT} — skipping."
        skipped+=("${VERSION}")
        continue
    fi

    if [[ ! -f "${NUKE_DIR}/include/DDImage/Iop.h" ]]; then
        echo "  NDK headers not found at ${NUKE_DIR}/include — skipping."
        skipped+=("${VERSION}")
        continue
    fi

    echo "  Nuke install : ${NUKE_DIR}"
    echo "  ASWF image   : ${ASWF_IMAGE[$VERSION]}"
    echo "  C++ standard : ${CXX_STD[$VERSION]}"
    echo "  CXX11 ABI    : ${CXX_ABI[$VERSION]}"

    ABI_FLAG="-D_GLIBCXX_USE_CXX11_ABI=${CXX_ABI[$VERSION]}"
    BUILD_DIR_HOST="${SCRIPT_DIR}/build_nuke${VERSION}"
    OUT_DIR="${DIST_DIR}/nuke${VERSION}"

    echo "  Running build inside container..."
    if docker run --rm \
        --gpus all \
        -v "${SCRIPT_DIR}:/src" \
        -v "${NUKE_DIR}:/nuke:ro" \
        "${ASWF_IMAGE[$VERSION]}" \
        bash -c "
            set -euo pipefail

            # ---- Locate pre-installed nvcc ------------------------------------
            # All ASWF containers ship nvcc at /usr/local/cuda-X.Y/bin/nvcc but
            # do not add it to PATH by default.
            NVCC_PATH=\$(find /usr/local/cuda-*/bin/nvcc 2>/dev/null | sort -V | tail -1 || true)
            if [[ -z \"\${NVCC_PATH}\" ]]; then
                echo 'ERROR: nvcc not found in container — cannot build' >&2
                exit 1
            fi
            CUDA_BIN=\$(dirname \"\${NVCC_PATH}\")
            CUDA_DIR=\$(dirname \"\${CUDA_BIN}\")
            export PATH=\"\${CUDA_BIN}:\${PATH}\"
            export LD_LIBRARY_PATH=\"\${CUDA_DIR}/lib64:\${LD_LIBRARY_PATH:-}\"

            nvcc --version

            # ---- Choose CUDA architectures based on toolkit version -----------
            # sm_120 (Blackwell) requires CUDA 12.8+.  Older toolkits error on
            # unknown SM targets, so we cap the list at what the toolkit supports.
            CUDA_VER=\$(nvcc --version | grep -oP 'release \K[0-9]+\.[0-9]+')
            CUDA_MAJOR=\$(echo \"\${CUDA_VER}\" | cut -d. -f1)
            CUDA_MINOR=\$(echo \"\${CUDA_VER}\" | cut -d. -f2)

            if   [[ \${CUDA_MAJOR} -lt 11 ]]; then
                # CUDA 10.x — max sm_75 (Turing)
                CUDA_ARCHS='50;52;60;61;70;75'
            elif [[ \${CUDA_MAJOR} -eq 11 && \${CUDA_MINOR} -lt 8 ]]; then
                # CUDA 11.0–11.7 — max sm_86 (Ampere)
                CUDA_ARCHS='50;52;60;61;70;75;80;86'
            elif [[ \${CUDA_MAJOR} -lt 12 ]]; then
                # CUDA 11.8+ — adds sm_89 (Ada) and sm_90 (Hopper)
                CUDA_ARCHS='50;52;60;61;70;75;80;86;89;90'
            elif [[ \${CUDA_MAJOR} -eq 12 && \${CUDA_MINOR} -lt 8 ]]; then
                # CUDA 12.0–12.7 — sm_120 not yet available
                CUDA_ARCHS='50;52;60;61;70;75;80;86;89;90'
            else
                # CUDA 12.8+ — full Blackwell support
                CUDA_ARCHS='50;52;60;61;70;75;80;86;89;90;120'
            fi

            echo \"CUDA \${CUDA_VER} -> architectures: \${CUDA_ARCHS}\"

            # ---- Build -------------------------------------------------------
            BUILD=/src/build_nuke${VERSION}
            # Remove stale build directory so CMake reconfigures cleanly.
            # (Files are owned by root from inside the container, so we must
            # clean from within the container rather than from the host.)
            rm -rf "\${BUILD}"
            cmake \
                -G 'Unix Makefiles' \
                -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_CXX_STANDARD=${CXX_STD[$VERSION]} \
                -DCMAKE_CUDA_STANDARD=${CXX_STD[$VERSION]} \
                \"-DCMAKE_CXX_FLAGS=${ABI_FLAG}\" \
                \"-DCMAKE_CUDA_ARCHITECTURES=\${CUDA_ARCHS}\" \
                -DNDK_ROOT=/nuke/include \
                -DNUKE_LIB_DIR=/nuke \
                -S /src \
                -B \"\${BUILD}\"

            cmake --build \"\${BUILD}\" --config Release -j\$(nproc)
        "; then
        echo "  Build SUCCEEDED"
    else
        echo "  Build FAILED"
        failed+=("${VERSION}")
        continue
    fi

    SO_SRC="${BUILD_DIR_HOST}/FlareSim.so"
    if [[ ! -f "${SO_SRC}" ]]; then
        echo "  FlareSim.so not produced — marking failed."
        failed+=("${VERSION}")
        continue
    fi

    mkdir -p "${OUT_DIR}"
    cp "${SO_SRC}" "${OUT_DIR}/FlareSim.so"
    echo "  => ${OUT_DIR}/FlareSim.so"
    succeeded+=("${VERSION}")
    echo ""

done

echo "==============================="
echo "Summary"
echo "==============================="
[[ ${#succeeded[@]} -gt 0 ]] && echo "  Built:   Nuke ${succeeded[*]}"
[[ ${#skipped[@]}   -gt 0 ]] && echo "  Skipped: Nuke ${skipped[*]}"
[[ ${#failed[@]}    -gt 0 ]] && echo "  Failed:  Nuke ${failed[*]}"
echo ""

[[ ${#failed[@]} -gt 0 ]] && exit 1 || exit 0
