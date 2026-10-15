// Real-world high-quality GPU benchmark. Includes CUDA host wrapper, optical
// device scratch, native Nuke ray splats, GPU sliding blur + GPU haze + alpha.
// This is not an AE timeline frame time: AE scheduling/convert/cache excluded.
#include "FlareLensGpuBridge.hpp"
#include <cuda_runtime.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <vector>
struct Case{int w,h,grid,spec,blades;float blur,haze;int repeats;int spec_kernel=-1;int mode=2;int source_count=1;};
int main(int argc,char** argv){
 int devices=0;if(cudaGetDeviceCount(&devices)!=cudaSuccess||!devices)return 77;
 LensSystem lens;
 const auto p=std::filesystem::path(__FILE__).parent_path().parent_path()/
   "upstream/lenses/lens_files/AI_Nikkor_50mm_F1.2S.lens";
 if(!lens.load(p.string().c_str()))return 1;
 const std::vector<fl::Source> lights{{1450.f/1920.f,300.f/1280.f,
                                        30000,30000,30000}};
 std::vector<Case> cases{
  {1920,1080,32,3,0,0,0,15},
  {1920,1280,64,3,6,.003f,1,12},
  {1920,1280,64,7,6,.003f,1,10,-1},
  {1920,1280,64,7,6,.003f,1,10,0},
  {1920,1280,64,7,6,.003f,1,10,1},
  {1920,1280,128,7,6,.003f,1,7,-1},
  {1920,1280,128,7,6,.003f,1,7,0},
  {1920,1280,128,7,6,.003f,1,7,1},
  {1920,1280,256,11,6,.003f,1,3,1},
  {1920,1080,32,3,6,.003f,0,3,1,2,32},
  {1920,1080,32,3,6,.003f,0,3,1,2,128},
  {1920,1080,32,3,6,.003f,0,2,1,2,512},
  {3840,2160,64,3,6,.003f,0,5}
 };
 // Opt-in high-grid test avoids repeatedly loading massive optical jobs
 // during normal benchmark runs. Do not advertise a fixed 10ms target
 // without source/pair/spectrum dimensions.
 if(argc>1 && std::string(argv[1])=="--grid512")cases={
  {1920,1080,256,3,0,0,0,5,1,4,1},
  {1920,1080,512,3,0,0,0,5,1,4,1},
  {1920,1080,512,7,0,0,0,4,1,4,1},
  {1920,1080,512,3,0,0,0,3,1,4,4},
 };
 const bool only_pair=argc>1 && std::string(argv[1])=="--selected512";
 if(only_pair)cases={
  {1920,1080,512,3,0,0,0,9,1,4,1},
  {1920,1080,512,7,0,0,0,7,1,4,1}
 };
 for(const auto& c:cases){
  std::vector<fl::Source> frame_lights;
  frame_lights.reserve(c.source_count);
  for(int i=0;i<c.source_count;i++){
   if(c.source_count==1)frame_lights.push_back(lights[0]);
   else frame_lights.push_back({float((i*37)%1024+.5f)/1024.f,
       float((i*67)%768+.5f)/768.f,100,80,60});
  }
  fl::Settings cfg{};
  cfg.gain=1000;cfg.grid=c.grid;cfg.max_pairs=2048;
  cfg.spectral_samples=c.spec;cfg.aperture_blades=c.blades;
  cfg.pupil_jitter=2;
  if(only_pair){
   const float sx=lens.focal_length*std::tan(cfg.fov_h_deg*.00872664625997f);
   const auto selected=fl::cached_pairs(lens,sx,sx*c.h/float(c.w));
   if(selected->pairs.empty())return 10;
   cfg.pair_mode=1;
   cfg.pair_surface_a=selected->pairs.front().surf_a;
   cfg.pair_surface_b=selected->pairs.front().surf_b;
  }
  const size_t bytes=size_t(c.w)*c.h*16;
  void *src=nullptr,*out=nullptr,*scratch=nullptr;
  const bool dense_haze=c.haze>0&&frame_lights.size()>8;
  const size_t size=fl::gpu::scratch_bytes(lens,int(frame_lights.size()),
              cfg.grid,c.w,c.h,c.mode,c.blur>0,dense_haze);
  if(!size||cudaMalloc(&src,bytes)!=cudaSuccess||
    cudaMalloc(&out,bytes)!=cudaSuccess||
    cudaMalloc(&scratch,size)!=cudaSuccess){
    std::cerr<<"allocation failure size="<<size<<"\n";return 2;
  }
  fl::gpu::View v{src,out,c.w,c.h,c.w*16,c.w*16};
  v.output_mode=c.mode;v.ghost_blur=c.blur;v.ghost_blur_passes=3;
  v.spectral_parallel=c.spec_kernel;
  v.haze_gain=c.haze;v.haze_radius=.15f;v.haze_passes=3;
  cudaMemset(src,0,bytes);
  if(!fl::gpu::cuda_compose(lens,frame_lights,cfg,v,scratch,size))return 3;
  std::vector<double> samples;
  samples.reserve(c.repeats);
  for(int i=0;i<c.repeats;i++){
   const auto t=std::chrono::steady_clock::now();
   if(!fl::gpu::cuda_compose(lens,frame_lights,cfg,v,scratch,size))return 4;
   samples.push_back(std::chrono::duration<double,std::milli>(
       std::chrono::steady_clock::now()-t).count());
  }
  std::sort(samples.begin(),samples.end());
  const int p95=std::min<int>(int(samples.size())-1,int(samples.size()*.95));
  const double mean=std::accumulate(samples.begin(),samples.end(),0.)/
                    samples.size();
  const float sx=lens.focal_length*std::tan(cfg.fov_h_deg*.00872664625997f);
  const float sy=sx*c.h/float(c.w);
  const auto pairs=fl::cached_pairs(lens,sx,sy);
  const int pair_count=only_pair?1:int(std::min<size_t>(pairs->pairs.size(),size_t(cfg.max_pairs)));
  std::cout<<"BENCH_DEVICE_SCRATCH "
      <<c.w<<"x"<<c.h<<" grid="<<c.grid<<" spectral="<<c.spec
      <<" pairs="<<pair_count
      <<" nominal_rays="<<uint64_t(c.grid)*c.grid*c.spec*c.source_count*pair_count
      <<" ghost_blur="<<c.blur<<" haze="<<c.haze
      <<" spectral_kernel="<<c.spec_kernel
      <<" sources="<<c.source_count
      <<" latency_median_ms="<<std::fixed<<std::setprecision(3)
      <<samples[samples.size()/2]<<" p95_ms="<<samples[p95]
      <<" mean_ms="<<mean<<" scratch_MB="<<double(size)/1048576
      <<" pixel_frames_MB="<<double(bytes)*2/1048576<<"\n";
  cudaFree(src);cudaFree(out);cudaFree(scratch);
 }
 return 0;
}
