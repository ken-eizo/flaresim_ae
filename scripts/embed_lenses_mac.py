#!/usr/bin/env python3
"""Embed original .lens bytes in the macOS AEX equivalent (no sidecar lens files).

Source manifest is the exact immutable 1,370-name Nuke catalog. The generator
rejects missing/duplicate/unordered input; data stays in the build directory.
This script contains NO Adobe SDK material.
"""
import argparse
from pathlib import Path

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    names = args.manifest.read_text(encoding="utf-8").splitlines()
    if len(names) != 1370 or len(set(names)) != 1370:
        raise SystemExit("Expected exactly 1370 unique lens names")
    if any("/" in name or "\\" in name or name.startswith(".") for name in names):
        raise SystemExit("Invalid catalog path")
    blobs = [(args.root / name).read_bytes() for name in names]
    offsets = [0]
    for blob in blobs:
        offsets.append(offsets[-1] + len(blob))
    out = [
        "// GENERATED FILE. Mac read-only embedded Nuke optical catalog.\n",
        '#include "FlareLensEmbeddedMac.hpp"\n',
        "#include <cstddef>\n#include <cstdint>\n\n",
        "namespace fl::catalog::embedded {\n",
        "static const unsigned char bytes[] = {\n",
    ]
    # The 256-column chunk prevents extremely long clang source lines.
    packed = b"".join(blobs)
    for i in range(0, len(packed), 24):
        out.append(" " + ",".join(str(n) for n in packed[i:i+24]) + ",\n")
    out.extend([
        "};\n",
        "static constexpr std::uint32_t offsets[] = {\n",
    ])
    for i in range(0, len(offsets), 16):
        out.append(" " + ",".join(str(n) for n in offsets[i:i+16]) + ",\n")
    out.extend([
        "};\n",
        "EmbeddedLens get(int zero_based) noexcept {\n",
        " if (zero_based < 0 || zero_based >= 1370) return {};\n",
        " const auto start = offsets[zero_based];\n",
        " return {reinterpret_cast<const char*>(bytes + start),\n",
        "         std::size_t(offsets[zero_based+1] - start)};\n",
        "}\n}\n",
    ])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    content = "".join(out)
    if not args.output.exists() or args.output.read_text(encoding="utf-8") != content:
        args.output.write_text(content, encoding="utf-8")
    print(f"Embedded {len(blobs)} lenses, {len(packed)} source bytes")

if __name__ == "__main__":
    main()
