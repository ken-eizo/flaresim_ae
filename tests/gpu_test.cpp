#include "FlareLensGpuBridge.hpp"
#include "FlareLensComposite.hpp"
#include "FlareLensCpuParallel.hpp"
#include <cuda_runtime.h>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
int main(){
 int devices=0;
 if(cudaGetDeviceCount(&devices)!=cudaSuccess||devices<1)return 77;
 LensSystem lens;
 const auto p=std::filesystem::path(__FILE__).parent_path().parent_path()/
 "upstream/lenses/lens_files/AI_Nikkor_50mm_F1.2S.lens";
 if(!lens.load(p.string().c_str()))return 1;
 constexpr int w=192,h=108;
 const size_t n=size_t(w)*h,bytes=n*16;
 std::vector<float> bg(n*4);
 for(size_t i=0;i<n;i++){
  const float alpha=(i%4==0)?0.f:1.f;
  bg[i*4]=.05f*alpha;
  bg[i*4+1]=.1f*alpha;
  bg[i*4+2]=.25f*alpha;
  bg[i*4+3]=alpha;
 }
 void *in=nullptr,*out=nullptr,*scratch=nullptr;
 if(cudaMalloc(&in,bytes)!=cudaSuccess||cudaMalloc(&out,bytes)!=cudaSuccess)return 2;
 const auto clean=[&](){cudaFree(in);cudaFree(out);cudaFree(scratch);};
 const std::vector<fl::Source> sources{{.56f,.44f,1.8f,.8f,.6f}};
 fl::Settings cfg;cfg.gain=2000;cfg.max_pairs=16;cfg.grid=12;
 const size_t size=fl::gpu::scratch_bytes(lens,int(sources.size()),cfg.grid,w,h);
 if(!size||cudaMalloc(&scratch,size)!=cudaSuccess){clean();return 3;}
 fl::gpu::View view{in,out,w,h,w*16,w*16};
 // All sampling modes and 3-11 spectral bands must agree with CPU baseline.
 std::vector<GhostPair> active;
 std::vector<float> boosts;
 GhostConfig gcfg{};
 const float sh=lens.focal_length*std::tan(cfg.fov_h_deg*.00872664626f);
 filter_ghost_pairs(lens,sh,sh*h/float(w),gcfg,active,boosts);
 if(active.empty()){clean();return 20;}
 bool visible_pair=false;
 for(int i=0;i<std::min<int>(int(active.size()),cfg.max_pairs);i++){
  cfg.pair_surface_a=active[i].surf_a;
  cfg.pair_surface_b=active[i].surf_b;
  cfg.pair_mode=1;
  const auto trial=fl::render_cpu(lens,sources,cfg,w,h);
  double sum=0;
  for(float value:trial.rgb)sum+=value;
  if(sum>1e-8){visible_pair=true;break;}
 }
 cfg.pair_mode=0;
 if(!visible_pair){clean();return 21;}
 const struct Case{int spec,blades,jitter,seed,pair_mode;float rot;}cases[]={
  {3,0,0,0,0,0},{5,0,0,0,0,0},{7,6,0,0,0,16},
  {9,5,1,123,0,22},{11,8,2,0,0,-13},
  {3,0,0,0,1,0},{3,0,0,0,2,0},
  {11,7,3,0,0,9}
 };
 for(int k=0;k<8;k++){
  cfg.pair_mode=cases[k].pair_mode;
  cfg.spectral_samples=cases[k].spec;
  cfg.aperture_blades=cases[k].blades;
  cfg.aperture_rotation_deg=cases[k].rot;
  cfg.pupil_jitter=cases[k].jitter;
  cfg.jitter_seed=cases[k].seed;
  if(cudaMemcpy(in,bg.data(),bytes,cudaMemcpyHostToDevice)!=cudaSuccess){clean();return 4;}
  auto t=std::chrono::steady_clock::now();
  if(!fl::gpu::cuda_compose(lens,sources,cfg,view,scratch,size)){clean();return 5;}
  const auto millis=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-t).count();
  std::vector<float> gpu(bg.size());
  if(cudaMemcpy(gpu.data(),out,bytes,cudaMemcpyDeviceToHost)!=cudaSuccess){clean();return 6;}
  const auto reference=fl::render_cpu(lens,sources,cfg,w,h);
  if(reference.rgb.size()!=n*3){clean();return 7;}
  double diff=0,energy=0;
  float peak=0;
  int visible_pixels=0;
  int transparent_emission_pixels=0;
  for(size_t i=0;i<n;i++){
   const float br=gpu[i*4+2]-bg[i*4+2];
   const float gg=gpu[i*4+1]-bg[i*4+1];
   const float bb=gpu[i*4+0]-bg[i*4+0];
   const float expected_alpha=fl::alpha_for_emission(bg[i*4+3],br,gg,bb);
   if(std::fabs(gpu[i*4+3]-expected_alpha)>1e-5){
    std::cerr<<"CUDA transparent alpha mismatch at "<<i<<"\n";
    clean();return 8;
   }
   if(bg[i*4+3]==0&&expected_alpha>.5f/255.f)transparent_emission_pixels++;
   for(int c=0;c<3;c++){
    const float actual=gpu[i*4+2-c]-bg[i*4+2-c],target=reference.rgb[i*3+c];
    if(!std::isfinite(actual)||!std::isfinite(target)){clean();return 9;}
    diff+=std::fabs(double(actual)-target);
    energy+=std::fabs(target);
    peak=std::max(peak,target);
    if(c==1&&target>.005f)visible_pixels++;
   }
  }
  if(k==0&&(peak<=0.f||visible_pixels<1||transparent_emission_pixels<1)){
   std::cerr<<"No visible ghost in the GPU reference! peak="<<peak
            <<" lit_pixels="<<visible_pixels<<"\n";
   clean();return 30;
  }
  const double relative=diff/std::max(1e-9,energy);
  std::cout<<"case "<<k<<" spec="<<cfg.spectral_samples
    <<" aperture="<<cfg.aperture_blades<<" jitter="<<cfg.pupil_jitter
    <<" gpu_ms="<<millis<<" relative_L1="<<relative
    <<" peak="<<peak<<" visible="<<visible_pixels
    <<" transparent_revived="<<transparent_emission_pixels<<"\n";
  if(energy<=0||relative>.04){clean();return 10+k;}
 }
 // Actually execute the new >512 pupil tiling path on CUDA, using the same
 // visible ghost pair on CPU. The full 2048 grid is deliberately NOT used
 // as an automated test because its cost scales quadratically.
 cfg.grid=640;cfg.max_pairs=2048;cfg.pair_mode=1;
 cfg.spectral_samples=3;cfg.aperture_blades=0;cfg.pupil_jitter=3;
 const size_t larger=fl::gpu::scratch_bytes(
    lens,int(sources.size()),cfg.grid,w,h);
 if(larger<=size){clean();return 40;}
 cudaFree(scratch);scratch=nullptr;
 if(cudaMalloc(&scratch,larger)!=cudaSuccess){clean();return 41;}
 if(cudaMemcpy(in,bg.data(),bytes,cudaMemcpyHostToDevice)!=cudaSuccess){
  clean();return 42;
 }
 if(!fl::gpu::cuda_compose(lens,sources,cfg,view,scratch,larger)){
  clean();return 43;
 }
 std::vector<float> high_gpu(bg.size());
 if(cudaMemcpy(high_gpu.data(),out,bytes,cudaMemcpyDeviceToHost)!=cudaSuccess){
  clean();return 44;
 }
 // Above the mesh threshold both renderers use the interpolated pupil mesh
 // (FlareLensMesh.hpp); compare CUDA with the CPU renderer of that method.
 const auto high_cpu=fl::render_sources_parallel(lens,sources,cfg,w,h,{},3);
 double high_diff=0.,high_energy=0.;
 for(size_t i=0;i<n;i++)for(int c=0;c<3;c++){
  const double actual=double(high_gpu[i*4+2-c]-bg[i*4+2-c]);
  const double expected=high_cpu.rgb[i*3+c];
  if(!std::isfinite(actual)||!std::isfinite(expected)){
   clean();return 45;
  }
  high_diff+=std::fabs(actual-expected);
  high_energy+=std::fabs(expected);
 }
 const double high_relative=high_diff/std::max(high_energy,1e-9);
 std::cout<<"HIGH_GRID_640_GOLDEN_ANGLE_CUDA_CPU_PARITY relative_L1="
          <<high_relative<<" sensor_energy="<<high_energy
          <<" scratch="<<larger<<"\n";
 if(high_energy<=0||high_relative>.045){clean();return 46;}
 clean();
 return 0;
}
