"""Generate Metal ray physics from the exact CUDA optical equations.

Only device-side, platform-independent equations are extracted. The shared
math source stays in FlareLensGpu.cu; do not maintain a divergent MSL copy.
"""
from pathlib import Path
import argparse, re
p=argparse.ArgumentParser()
p.add_argument("--cuda",type=Path,required=True)
p.add_argument("--output",type=Path,required=True)
a=p.parse_args()
source=a.cuda.read_text(encoding="utf-8")
begin=source.index("struct DVec3 {")
end=source.index("// GPU-side data structures",begin)
physics=source[begin:end]
physics=physics.replace("__device__ __forceinline__","inline")
physics=physics.replace("const Surface*","device const Surface*")
physics=physics.replace("const Surface&","device const Surface&")
physics=physics.replace("const DRay&","thread const DRay&")
physics=physics.replace("const DVec3&","thread const DVec3&")
physics=re.sub(r"(?<!const )DVec3&","thread DVec3&",physics)
for old,new in (("rsqrtf(","rsqrt("),("ldexpf(","ldexp("),
                ("fabsf(","fabs("),("fminf(","fmin("),
                ("fmaxf(","fmax("),("sqrtf(","sqrt("),
                ("cosf(","cos(")):
    physics=physics.replace(old,new)
# Strip comments containing non-ASCII text from upstream headers.
physics="\n".join(line for line in physics.splitlines() if not (line.lstrip().startswith("//") and not line.isascii()))
header="""// AUTO-GENERATED from FlareLensGpu.cu. Do not edit by hand.
#include <metal_stdlib>
using namespace metal;
// Matches the 32-byte C++ Surface POD (bool + three bytes padding).
struct Surface {
 float radius,thickness,ior,abbe_v,semi_aperture;
 int coating;
 uchar is_stop;
 uchar padding[3];
 float z;
};
"""
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_bytes((header+physics+"\n").encode("utf-8"))
print("METAL_PHYSICS_GENERATED",len(physics.splitlines()),"lines")
