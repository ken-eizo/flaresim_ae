#!/usr/bin/env python3
"""Verify that a macOS After Effects EFFECT bundle is discoverable by its PiPL."""
import argparse
import plistlib
import struct
import sys
from pathlib import Path

def u16(data, off):
    return struct.unpack_from(">H", data, off)[0]

def u32(data, off):
    return struct.unpack_from(">I", data, off)[0]

def require(ok, message):
    if not ok:
        raise ValueError(message)

def pipl_resource(blob):
    """Extract PiPL ID 16000 from a classic Mac Resource Manager data-fork file."""
    require(len(blob) >= 40, "PiPL .rsrc file is empty or truncated")
    data_offset, map_offset, data_len, map_len = struct.unpack_from(">IIII", blob)
    require(data_offset >= 16 and map_offset >= 16, "Bad resource-fork offsets")
    require(data_offset + data_len <= len(blob), "Resource data out of bounds")
    require(map_offset + map_len <= len(blob), "Resource map out of bounds")
    type_list = map_offset + u16(blob, map_offset + 24)
    require(type_list + 2 <= map_offset + map_len, "Missing type list")
    count = u16(blob, type_list) + 1
    require(count <= 1000, "Implausible resource type count")
    for index in range(count):
        entry = type_list + 2 + 8 * index
        require(entry + 8 <= map_offset + map_len, "Truncated resource type")
        if blob[entry:entry + 4] != b"PiPL":
            continue
        ref_count = u16(blob, entry + 4) + 1
        refs = type_list + u16(blob, entry + 6)
        for ref_index in range(ref_count):
            ref = refs + 12 * ref_index
            require(ref + 12 <= map_offset + map_len, "Truncated PiPL reference")
            if u16(blob, ref) != 16000:
                continue
            relative = int.from_bytes(blob[ref + 5:ref + 8], "big")
            loc = data_offset + relative
            require(loc + 4 <= data_offset + data_len, "PiPL data offset invalid")
            length = u32(blob, loc)
            require(loc + 4 + length <= data_offset + data_len, "PiPL payload truncated")
            return blob[loc + 4:loc + 4 + length]
    raise ValueError("Missing PiPL resource ID 16000")

def check(plugin):
    require(plugin.is_dir(), f"Missing bundle: {plugin}")
    content = plugin / "Contents"
    info = plistlib.loads((content / "Info.plist").read_bytes())
    for key, expected in {
        "CFBundlePackageType": "eFKT",
        "CFBundleSignature": "FXTC",
        "CFBundleExecutable": "FlareSimAE",
        "CFBundleIdentifier": "com.ken-eizo.flaresimae",
    }.items():
        require(info.get(key) == expected, f"{key}: expected {expected!r}, got {info.get(key)!r}")
    require((content / "PkgInfo").read_bytes() == b"eFKTFXTC", "Missing/incorrect PkgInfo")
    exe = content / "MacOS" / "FlareSimAE"
    require(exe.stat().st_size > 100000, "Missing/truncated Mach-O effect binary")
    metal = content / "Resources" / "FlareLensMetal.metallib"
    require(metal.stat().st_size > 1024, "Missing Metal shader library")
    pipl = content / "Resources" / "FlareSimAE.rsrc"
    payload = pipl_resource(pipl.read_bytes())
    for text in [b"ma64", b"mi64", b"EffectMain", b"FlareSimAE", b"FlareLensAE_v1", b"eVER", b"eSVR", b"eGLO", b"eGL2", b"eMNA"]:
        require(text in payload, f"Required PiPL property absent: {text.decode()}")
    print("AE_MAC_PLUGIN_DISCOVERY_VALID=1")
    print("PiPL data bytes:", len(payload))
    print("Bundle version:", info.get("CFBundleShortVersionString"))

def main():
    arg = argparse.ArgumentParser()
    arg.add_argument("bundle", type=Path)
    args = arg.parse_args()
    try:
        check(args.bundle)
    except (OSError, ValueError, TypeError, KeyError) as err:
        print(f"AE_MAC_PLUGIN_DISCOVERY_VALID=0: {err}", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
