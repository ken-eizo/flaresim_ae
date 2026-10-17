#!/usr/bin/env python3
"""Create a minimal three-item binary ZIP for FlareSimAE Windows or macOS."""
import argparse
import hashlib
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
NOTICE_SOURCES = (
    ("Project MIT License", ROOT / "LICENSE"),
    ("Upstream MIT License", ROOT / "upstream" / "LICENSE"),
    ("Third-party notices", ROOT / "NOTICE.md"),
)

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--platform", required=True, choices=("Windows", "macOS"))
    p.add_argument("--version", required=True)
    p.add_argument("--binary", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    a = p.parse_args()
    if not a.version.startswith("v") or len(a.version.split(".")) != 3:
        p.error("version must be vMajor.Minor.Patch")
    if a.output.exists():
        p.error("Refusing to overwrite existing release ZIP: " + str(a.output))
    if not a.binary.exists():
        p.error("Missing built plug-in: " + str(a.binary))
    a.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="flaresim_pkg_") as temp:
        stage = Path(temp)
        expected = "FlareLensAE.aex" if a.platform == "Windows" else "FlareSimAE.plugin"
        if a.binary.name != expected:
            p.error("Unexpected binary name: " + a.binary.name)
        dest = stage / expected
        if a.platform == "Windows":
            shutil.copy2(a.binary, dest)
        else:
            if not a.binary.is_dir():
                p.error("macOS plug-in bundle is not a directory")
            version = plistlib.loads((a.binary / "Contents" / "Info.plist").read_bytes()).get("CFBundleShortVersionString")
            if version != a.version.lstrip("v"):
                p.error("Bundle version differs from release: " + str(version))
            subprocess.run(["ditto", str(a.binary), str(dest)], check=True)
        shutil.copy2(ROOT / "scripts" / "RELEASE_INSTALL.txt", stage / "INSTALL.txt")
        notices = ["FlareSimAE - LICENSE and third-party notices"]
        for name, source in NOTICE_SOURCES:
            notices.extend(("\n--- " + name + " ---\n", source.read_text(encoding="utf-8")))
        (stage / "LICENSE.txt").write_text("\n".join(notices) + "\n", encoding="utf-8")
        try:
            if a.platform == "Windows":
                with zipfile.ZipFile(a.output, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
                    for entry in sorted(stage.iterdir()):
                        z.write(entry, arcname=entry.name)
            else:
                subprocess.run(["ditto", "-c", "-k", str(stage), str(a.output)], check=True)
            with zipfile.ZipFile(a.output) as z:
                roots = {n.split("/")[0] for n in z.namelist()}
                required = {expected, "INSTALL.txt", "LICENSE.txt"}
                if roots != required:
                    raise ValueError("ZIP has unexpected top-level entries: " + str(roots))
                for name in ("INSTALL.txt", "LICENSE.txt"):
                    if len(z.read(name)) < 100:
                        raise ValueError(name + " missing/empty")
                if a.platform == "Windows":
                    if hashlib.sha256(z.read(expected)).digest() != hashlib.sha256(a.binary.read_bytes()).digest():
                        raise ValueError("AEX hash mismatch")
            print("PACKAGE_PATH=" + str(a.output))
            print("PACKAGE_BYTES=" + str(a.output.stat().st_size))
            print("PACKAGE_SHA256=" + hashlib.sha256(a.output.read_bytes()).hexdigest())
            print("PACKAGE_READY=1")
        except BaseException:
            a.output.unlink(missing_ok=True)
            raise

if __name__ == "__main__":
    sys.exit(main())
