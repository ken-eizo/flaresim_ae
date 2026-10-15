# Audit numerical domains of the original upstream *.lens prescriptions.
from pathlib import Path
import math
from collections import Counter
root=Path(__file__).resolve().parent.parent / "upstream" / "lenses" / "lens_files"
rejects=Counter()
examples={}
counts=[]
values={k:[] for k in ["radius","thickness","ior","abbe","semi","coating","focal"]}
for file in sorted(root.glob("*.lens")):
    surfaces=[]
    focal=0
    reading=False
    for text in file.read_text(encoding="utf-8",errors="replace").splitlines():
        text=text.strip()
        if not text or text.startswith("#"):continue
        if text.startswith("focal_length:"):
            try:focal=float(text.split(":",1)[1])
            except: pass
            continue
        if text.startswith("surfaces:"): reading=True;continue
        if not reading:continue
        c=text.split()
        if len(c)<6:continue
        try:
            r=0 if c[0].lower() in ("stop","inf") else float(c[0])
            t,i,a,s,coat=map(float,c[1:6])
        except:continue
        if s<=0:continue
        surfaces.append((r,t,i,a,s,coat))
        for key,value in zip(("radius","thickness","ior","abbe","semi","coating"),surfaces[-1]):
            values[key].append(value)
        for why,condition in [
            ("radius",not math.isfinite(r) or abs(r)>100000),
            ("thickness",not math.isfinite(t) or abs(t)>1000),
            ("ior",not math.isfinite(i) or i<.95 or i>3.5),
            ("abbe",not math.isfinite(a) or a<0 or a>150),
            ("semi",not math.isfinite(s) or s<.01 or s>250),
            ("coat",not math.isfinite(coat) or coat<0 or coat>32)
        ]:
            if condition:
                rejects[why]+=1
                examples.setdefault(why,[]).append((file.name,len(surfaces),value))
    counts.append(len(surfaces))
    values["focal"].append(focal)
    if len(surfaces)>64:rejects["surfaces>64"]+=1
    if len(surfaces)>=2 and (not math.isfinite(focal) or focal<=0 or focal>2000):
        rejects["focal"]+=1
    if len(surfaces)>=2 and all(x[1]>=0 for x in surfaces):pass
print("FILES",len(counts),"SURFACES max",max(counts),"min",min(counts))
for k,v in values.items():
    print(k,"count",len(v),"min",min(v),"max",max(v))
print("REJECTIONS",rejects)
for why,ex in examples.items():
    print("EXAMPLES",why,ex[:8])
