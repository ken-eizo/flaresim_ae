// The grouped CPU tracers must reproduce every ray of the per-pair tracers
// bit for bit: same valid flag, and the same position and weight for every
// ray that reaches the sensor. Checked for scalar and AVX2, for all pairs of
// each second bounce and for a sparse subset, on a 64-surface zoom. AVX2
// calls of 8 samples (one batch) must also match calls of 16 (two batches
// stepped together).
#include "FlareLensCpuTrace.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <vector>

namespace {
bool same(float a,float b){return std::memcmp(&a,&b,sizeof a)==0;}
using Batch=void(*)(const fl::cpu::TraceInput&,const fl::PupilSample*,int,
                    uint8_t*,float*,float*,float*);
using GroupBatch=void(*)(const fl::cpu::GroupInput&,const fl::PupilSample*,int,int,
                         uint8_t*,float*,float*,float*);
} // namespace

int main(){
 const auto dir=std::filesystem::path(__FILE__).parent_path().parent_path()/
  "upstream/lenses/lens_files";
 const char* names[]={
  "AI_Nikkor_50mm_F1.2S.lens",
  "Nikon_180-400mm_F4_IF_OS_(AF-S_Nikkor_180-400mm_F4E_TC1.4_FL_ED_VR_with_TC).lens"};
 std::mt19937 rng(7);
 std::uniform_real_distribution<float> uniform(-1.f,1.f);
 std::vector<fl::PupilSample> samples;
 while(samples.size()<203){
  const float u=uniform(rng),v=uniform(rng);
  if(u*u+v*v<=1.f)samples.push_back({u,v});
 }
 const int count=int(samples.size());
 struct Variant {const char* name;Batch pair;GroupBatch group;};
 std::vector<Variant> variants{{"scalar",fl::cpu::trace_batch_scalar,fl::cpu::trace_group_scalar}};
#if defined(FLARELENS_HAVE_AVX2_TRACE)
 if(fl::cpu::avx2_supported())
  variants.push_back({"avx2",fl::cpu::trace_batch_avx2,fl::cpu::trace_group_avx2});
#endif
 size_t checked=0,reached=0;
 for(const char* name:names){
  LensSystem lens;
  if(!lens.load((dir/name).string().c_str())){std::cerr<<"cannot load "<<name<<"\n";return 1;}
  const int n=lens.num_surfaces();
  for(const float lambda:{450.f,550.f,650.f}){
   std::vector<float> ior(static_cast<size_t>(n));
   for(int s=0;s<n;s++)ior[size_t(s)]=lens.surfaces[size_t(s)].ior_at(lambda);
   fl::cpu::TraceInput in;
   in.surfaces=lens.surfaces.data();in.n=n;in.sensor_z=lens.sensor_z;
   in.lambda=lambda;in.ior=ior.data();
   in.dir=Vec3f(.08f,-.05f,1.f).normalized();
   in.front_radius=lens.surfaces.front().semi_aperture;
   in.start_z=lens.surfaces.front().z-20.f;
   for(int b=1;b<n;b++){
    for(int sparse=0;sparse<2;sparse++){
     std::vector<int> a;
     for(int k=b-1;k>=0;k--)if(!sparse||(k%3)==1)a.push_back(k);
     if(a.empty())continue;
     fl::cpu::GroupInput group;
     group.base=in;group.base.b=b;group.a=a.data();group.count=int(a.size());
     const size_t lanes=a.size()*size_t(count);
     for(const auto& variant:variants){
      std::vector<uint8_t> gv(lanes);
      std::vector<float> gx(lanes),gy(lanes),gw(lanes);
      variant.group(group,samples.data(),count,count,gv.data(),gx.data(),gy.data(),gw.data());
      if(variant.group!=fl::cpu::trace_group_scalar){
       // The same group in calls of at most 8 samples: single batches only.
       std::vector<uint8_t> sv(lanes);
       std::vector<float> sx(lanes),sy(lanes),sw(lanes);
       for(int k=0;k<count;k+=8)
        variant.group(group,samples.data()+k,std::min(8,count-k),count,
                      sv.data()+k,sx.data()+k,sy.data()+k,sw.data()+k);
       for(size_t o=0;o<lanes;o++){
        if(sv[o]!=gv[o]||(sv[o]&&(!same(sx[o],gx[o])||!same(sy[o],gy[o])||!same(sw[o],gw[o])))){
         std::cerr<<variant.name<<" 8 vs 16 lane mismatch: "<<name<<" b="<<b<<"\n";
         return 4;
        }
       }
      }
      for(size_t j=0;j<a.size();j++){
       fl::cpu::TraceInput pair=in;
       pair.a=a[j];pair.b=b;
       std::vector<uint8_t> pv(static_cast<size_t>(count));
       std::vector<float> px(pv.size()),py(pv.size()),pw(pv.size());
       variant.pair(pair,samples.data(),count,pv.data(),px.data(),py.data(),pw.data());
       if(variant.pair!=fl::cpu::trace_batch_scalar){
        std::vector<uint8_t> qv(pv.size());
        std::vector<float> qx(pv.size()),qy(pv.size()),qw(pv.size());
        for(int k=0;k<count;k+=8)
         variant.pair(pair,samples.data()+k,std::min(8,count-k),
                      qv.data()+k,qx.data()+k,qy.data()+k,qw.data()+k);
        for(size_t k=0;k<pv.size();k++){
         if(qv[k]!=pv[k]||(pv[k]&&(!same(qx[k],px[k])||!same(qy[k],py[k])||!same(qw[k],pw[k])))){
          std::cerr<<variant.name<<" pair 8 vs 16 lane mismatch: "<<name<<"\n";
          return 5;
         }
        }
       }
       for(int k=0;k<count;k++){
        const size_t o=j*size_t(count)+size_t(k);
        const bool hit=pv[size_t(k)]!=0;
        if(hit!=(gv[o]!=0)||
           (hit&&(!same(px[size_t(k)],gx[o])||!same(py[size_t(k)],gy[o])||
                  !same(pw[size_t(k)],gw[o])))){
         std::cerr<<variant.name<<" mismatch: "<<name<<" lambda="<<lambda
                  <<" pair=("<<a[j]<<","<<b<<") sample="<<k<<"\n";
         return 2;
        }
        checked++;reached+=hit;
       }
      }
     }
    }
   }
  }
 }
 if(!reached)return 3;
 std::cout<<"CPU_GROUP_TRACE_BIT_EXACT rays="<<checked<<" reached_sensor="<<reached
          <<" variants="<<variants.size()<<"\n";
 return 0;
}
