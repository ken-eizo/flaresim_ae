"""Protect the pinned, unmodified Nuke reference against accidental drift."""
from pathlib import Path
import hashlib
import sys

root=Path(__file__).resolve().parents[2]
expected={
  "upstream/src/fresnel.h":
      "cb16064d51708d098fe44b696d2eaa75c44e76d09542ee3767c511fc9147cda8",
  "upstream/src/ghost_cuda.cu":
      "14c3728a28b7de4cb9d3dc12f660b0372168ab4be51aa437f65e91d62198e0ef",
  "tests/reference/nuke-original-0e28ead-case0-256x144-rgb32f.bin":
      "902edee8e1955d9666e145d0870d3708b50f76783206fd2bb24a8ee86a3239b8",
}
for rel,digest in expected.items():
    data=(root/rel).read_bytes()
    if rel.endswith((".h",".cu")):
        data=data.replace(b"\r\n",b"\n")
    actual=hashlib.sha256(data).hexdigest()
    if actual!=digest:
        print("NUKE_PRISTINE_REFERENCE_DIGEST_MISMATCH",rel,digest,actual,
              file=sys.stderr)
        raise SystemExit(1)
def physics(source):
    a=source.index("// Single-layer MgF2 AR coating")
    b=source.index("// ---- Ray",a)
    return source[a:b]
nuke=(root/"upstream/src/ghost_cuda.cu").read_text(encoding="utf-8").replace("\r\n","\n")
ae=(root/"src/FlareLensGpu.cu").read_text(encoding="utf-8").replace("\r\n","\n")
if physics(nuke)!=physics(ae):
    print("CUDA_OPTICAL_THIN_FILM_FORMULA_DIFFERS_FROM_NUKE",file=sys.stderr)
    raise SystemExit(2)
print("UNMODIFIED_NUKE_GIT_0e28ead_PHYSICS_AND_GOLDEN_REFERENCE_VERIFIED")
