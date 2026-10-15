// Executable linking the original, unmodified Nuke FlareSim CUDA launcher.
// Generates a verifiable floating point reference, not a CPU self-reference.
#include "upstream_fixture_cases.hpp"
#include "ghost_cuda.h"
#include <cuda_runtime.h>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

int main(){
 int devices=0;
 if(cudaGetDeviceCount(&devices)!=cudaSuccess||devices==0)return 77;
 const auto all=flare_fixture::cases();
 std::ofstream out(flare_fixture::output_path(),std::ios::binary|std::ios::trunc);
 if(!out)return 1;
 const std::array<uint32_t,4> hdr={
  flare_fixture::signature,unsigned(all.size()),
  unsigned(flare_fixture::width),unsigned(flare_fixture::height)
 };
 out.write(reinterpret_cast<const char*>(hdr.data()),sizeof(hdr));
 const int w=flare_fixture::width,h=flare_fixture::height;
 const size_t pixels=size_t(w)*h;
 std::vector<float> red(pixels),green(pixels),blue(pixels);
 GpuBufferCache cache;
 int nonzero=0;
 for(size_t k=0;k<all.size();k++){
  const auto& c=all[k];
  const auto lens=flare_fixture::lens(c.lens);
  const auto settings=flare_fixture::settings(c);
  const auto source=flare_fixture::sources(c);
  const float sh=lens.focal_length*std::tan(settings.fov_h_deg*.00872664625997f);
  const float sv=sh*h/float(w);
  GhostConfig cfg{};
  cfg.ray_grid=settings.grid;
  cfg.gain=settings.gain;
  cfg.spectral_samples=settings.spectral_samples;
  cfg.aperture_blades=settings.aperture_blades;
  cfg.aperture_rotation_deg=settings.aperture_rotation_deg;
  cfg.pupil_jitter=settings.pupil_jitter;
  cfg.pupil_jitter_seed=settings.jitter_seed;
  std::vector<GhostPair> pairs;
  std::vector<float> boosts;
  filter_ghost_pairs(lens,sh,sv,cfg,pairs,boosts);
  std::vector<BrightPixel> lights;
  for(const auto& s:source)
   lights.push_back({
    std::atan((s.x*2-1)*std::tan(settings.fov_h_deg*.00872664625997f)),
    std::atan((s.y*2-1)*sv/lens.focal_length),s.r,s.g,s.b
   });
  std::fill(red.begin(),red.end(),0);
  std::fill(green.begin(),green.end(),0);
  std::fill(blue.begin(),blue.end(),0);
  std::string err;
  launch_ghost_cuda(lens,pairs,boosts,lights,sh,sv,
     red.data(),green.data(),blue.data(),w,h,w,h,0,0,cfg,cache,&err);
  if(!err.empty()){std::cerr<<"upstream CUDA error: "<<err<<"\n";return 2;}
  double sum=0;
  for(size_t i=0;i<pixels;i++){
   const float rgb[3]={red[i],green[i],blue[i]};
   for(float v:rgb){
    if(!std::isfinite(v)||v<0)return 3;
    sum+=v;
   }
   out.write(reinterpret_cast<const char*>(rgb),sizeof(rgb));
  }
  if(sum>0)nonzero++;
  if(!out)return 4;
  std::cout<<"reference "<<k<<" lens="<<c.lens<<" pupil="<<c.pupil
           <<" wavelengths="<<c.spectral<<" energy="<<sum<<"\n";
 }
 cache.release();
 out.flush();
 if(!out||nonzero<int(all.size()/2))return 5;
 std::cout<<"UPSTREAM_REFERENCE_GENERATED cases="<<all.size()
          <<" positive="<<nonzero<<"\n";
 return 0;
}
