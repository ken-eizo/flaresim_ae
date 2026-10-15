#!/usr/bin/env bash
# package_release.sh - Package FlareSim for release on Linux
#
# Creates one zip per Nuke version under release_packages/:
#   FlareSim_v<version>_Nuke<N>_linux.zip
#
# Each zip is ready to unpack into ~/.nuke/plugins/
#
# Usage:
#   ./package_release.sh
#   ./package_release.sh --version 1.0.0
#   ./package_release.sh --nuke-versions "13 14 15 16 17"
#   ./package_release.sh --dist-dir ./dist --out-dir ./release_packages

set -euo pipefail

VERSION=""
NUKE_VERSIONS="13 14 15 16 17"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="${SCRIPT_DIR}/dist"
OUT_DIR="${SCRIPT_DIR}/release_packages"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)       VERSION="$2";       shift 2 ;;
        --nuke-versions) NUKE_VERSIONS="$2"; shift 2 ;;
        --dist-dir)      DIST_DIR="$2";      shift 2 ;;
        --out-dir)       OUT_DIR="$2";       shift 2 ;;
        *)
            echo "Unknown argument: $1" >&2
            echo "Usage: $0 --version 1.1.0 [--nuke-versions \"13 14 15 16 17\"] [--dist-dir ./dist] [--out-dir ./release_packages]" >&2
            exit 1 ;;
    esac
done

if [[ -z "${VERSION}" ]]; then
    echo "Error: --version is required (e.g. $0 --version 1.1.0)" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}"

echo ""
echo "FlareSim release packager  v${VERSION}  (Linux)"
echo "Output: ${OUT_DIR}"
echo ""

for NV in $NUKE_VERSIONS; do

    SO="${DIST_DIR}/nuke${NV}/FlareSim.so"
    if [[ ! -f "${SO}" ]]; then
        echo "  Nuke ${NV} : FlareSim.so not found at ${SO} — skipping."
        continue
    fi

    ZIP_NAME="FlareSim_v${VERSION}_Nuke${NV}_linux.zip"
    ZIP_PATH="${OUT_DIR}/${ZIP_NAME}"

    STAGE="$(mktemp -d)"
    trap 'rm -rf "${STAGE}"' EXIT

    # --- Core plugin files ---
    cp "${SO}"                                         "${STAGE}/FlareSim.so"
    cp "${SCRIPT_DIR}/menu.py"                         "${STAGE}/menu.py"
    cp "${SCRIPT_DIR}/FlareSim_LensBrowser.py"        "${STAGE}/FlareSim_LensBrowser.py"

    # --- Lens library ---
    mkdir -p "${STAGE}/lenses"
    cp -r "${SCRIPT_DIR}/lenses/lens_files"           "${STAGE}/lenses/lens_files"
    cp    "${SCRIPT_DIR}/lenses/convert_ob.py"        "${STAGE}/lenses/convert_ob.py"
    cp    "${SCRIPT_DIR}/lenses/convert_p2p.py"       "${STAGE}/lenses/convert_p2p.py"

    # --- Zip it ---
    rm -f "${ZIP_PATH}"
    if command -v zip &>/dev/null; then
        (cd "${STAGE}" && zip -r "${ZIP_PATH}" .)
    else
        (cd "${STAGE}" && python3 -m zipfile -c "${ZIP_PATH}" .)
    fi

    SIZE_KB=$(du -k "${ZIP_PATH}" | cut -f1)
    SIZE_MB=$(awk "BEGIN { printf \"%.1f\", ${SIZE_KB}/1024 }")
    echo "  Nuke ${NV} : ${ZIP_NAME}  (${SIZE_MB} MB)"

    rm -rf "${STAGE}"
    trap - EXIT

done

echo ""
echo "Done. Upload the zips from ${OUT_DIR} as GitHub Release assets."
echo ""
