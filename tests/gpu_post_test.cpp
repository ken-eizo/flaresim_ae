// Real CUDA post-process regression: compare GPU ray, Nuke-equivalent box
// blur, scene-linear haze and alpha with the CPU optics, for manual emitters.
// GPU pixels stay on device until the final reference-check copy.
#include "FlareLensGpuBridge.hpp"
#include "FlareLensOptics.hpp"
#include "FlareLensNukePreview.hpp"
#include "FlareLensComposite.hpp"
#include <cuda_runtime.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <vector>

struct Case {
 int w,h,grid,mode,blades,spectrum;
 float blur,haze;
 int passes;
 int spectral_parallel=-1;
 int num_sources=1;
 int haze_footprint=1;
};
int main(){
 int device_count=0;
 if(cudaGetDeviceCount(&device_count)!=cudaSuccess||device_count==0)return 77;
 LensSystem lens;
 const auto lensfile=std::filesystem::path(__FILE__).parent_path().parent_path()/
    "upstream/lenses/lens_files/AI_Nikkor_50mm_F1.2S.lens";
 if(!lens.load(lensfile.string().c_str()))return 1;
 const std::array<Case,14> cases{{
  {640,360,32,1,0,3,0,0,3},       // Unfiltered Nuke CUDA
  {640,360,32,2,6,7,0,0,3},       // Write photons directly to AE output
  {640,360,32,4,6,7,0,0,3},       // Ghost-only, zero scratch pixel frames
  {960,540,64,2,6,7,.003f,0,3},  // User scene: flare plate, blur
  {960,540,64,2,6,7,.003f,0,3,0}, // Force wavelength serial
  {960,540,64,2,6,7,.003f,0,3,1}, // Force wavelength parallel
  {960,540,64,1,6,7,.003f,1,3},  // Shape layer Composite + GPU haze
  {640,360,32,4,6,3,.006f,0,3},  // Ghost Only excludes haze
  {960,540,64,5,6,7,.003f,1,3},  // Haze Only excludes ghosts
  {320,180,12,1,6,7,.003f,1,3,-1,24}, // 24 sources: ghost plus dense Haze
  {640,360,12,5,6,7,.003f,1,3,-1,48}, // 48 HDR sources: dense Haze-only
  {320,180,12,5,6,7,.003f,1,3,-1,24,4}, // Nuke's whole 4x4 HDR blocks
  {1920,1280,128,2,6,7,.003f,1,3}, // High-quality screenshot conditions
  {127,91,12,5,0,3,0,2.5f,3,-1,1,4} // Haze from 48 raw blocks AFTER Max Sources 1
 }};
 const std::vector<fl::Source> source_one{{
  1450.f/1920.f,300.f/1280.f,30000,30000,30000
 }};
 std::vector<fl::Source> source_many;
 for(int i=0;i<48;i++)
  source_many.push_back({float(i%8+.5f)/8.f,float(i/8+.5f)/6.f,
    float(40+i),float(60+i),float(80+i)});
 for(size_t test=0;test<cases.size();test++){
  const Case& c=cases[test];
  const std::vector<fl::Source> sources=c.num_sources==1?
   source_one:std::vector<fl::Source>(source_many.begin(),
                                     source_many.begin()+c.num_sources);
  const auto& haze_sources=test==cases.size()-1?source_many:sources;
  const size_t pixels=size_t(c.w)*c.h;
  const size_t bytes=pixels*16;
  fl::Settings s{};
  s.grid=c.grid;s.spectral_samples=c.spectrum;s.max_pairs=2048;
  s.aperture_blades=c.blades;s.pupil_jitter=2;
  s.gain=1000.f;
  fl::gpu::View v{};
  v.width=c.w;v.height=c.h;v.input_pitch_bytes=c.w*16;
  v.output_pitch_bytes=c.w*16;
  v.output_mode=c.mode;v.ghost_blur=c.blur;
  v.ghost_blur_passes=c.passes;v.haze_gain=c.haze;
  v.haze_radius=.15f;v.haze_passes=3;
  v.spectral_parallel=c.spectral_parallel;
  v.haze_footprint=c.haze_footprint;
  v.haze_sources=&haze_sources;
  std::vector<float> bg(pixels*4,0);
  for(size_t i=0;i<pixels;i++)bg[i*4+3]=0; // fully transparent shape
  void* in=nullptr,*out=nullptr,*scratch=nullptr;
  const auto free_mem=[&](){if(in)cudaFree(in);if(out)cudaFree(out);
                            if(scratch)cudaFree(scratch);};
  const bool dense_haze=c.haze>0&&(haze_sources.size()>8||c.haze_footprint>1)&&
      (c.mode==1||c.mode==2||c.mode==5);
  const auto size=fl::gpu::scratch_bytes(lens,int(std::max(sources.size(),haze_sources.size())),
       c.grid,c.w,c.h,c.mode,c.blur>0&&c.passes>0,dense_haze);
  if(cudaMalloc(&in,bytes)!=cudaSuccess||
     cudaMalloc(&out,bytes)!=cudaSuccess||
     cudaMalloc(&scratch,size)!=cudaSuccess){free_mem();return 2;}
  v.input=in;v.output=out;
  if(cudaMemcpy(in,bg.data(),bytes,cudaMemcpyHostToDevice)!=cudaSuccess){
   free_mem();return 3;
  }
  const auto begin=std::chrono::steady_clock::now();
  if(!fl::gpu::cuda_compose(lens,sources,s,v,scratch,size)){
   std::cerr<<"GPU renderer failed test "<<test<<"\n";free_mem();return 4;
  }
  const auto elapsed=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-begin).count();
  std::vector<float> got(pixels*4);
  if(cudaMemcpy(got.data(),out,bytes,cudaMemcpyDeviceToHost)!=cudaSuccess){
   free_mem();return 5;
  }
  free_mem();
  std::vector<float> expected(pixels*3,0);
  if(c.mode!=5){
   const auto ghost=fl::render_cpu(lens,sources,s,c.w,c.h);
   if(ghost.rgb.size()!=expected.size())return 6;
   expected=ghost.rgb;
   if(c.blur>0){
    const int radius=int(std::round(c.blur*std::hypot(float(c.w),float(c.h))));
    fl::box_blur_rgb(expected,c.w,c.h,radius,c.passes);
   }
  }
  if(c.haze>0&&(c.mode==1||c.mode==5)){
   std::vector<float> haze;
   fl::render_haze(haze,haze_sources,c.w,c.h,c.haze,.15f,3,c.haze_footprint);
   if(haze.size()!=expected.size())return 7;
   for(size_t i=0;i<expected.size();i++)expected[i]+=haze[i];
  }
  double rel=0,energy=0;
  double max_error=0;
  int lit=0,wrong_alpha=0;
  for(size_t i=0;i<pixels;i++){
   const float r=got[4*i+2],g=got[4*i+1],b=got[4*i];
   const float alpha=got[4*i+3];
   // Compare alpha to the *actual GPU photon RGB*, not the CPU splat:
   // CPU/CUDA are not bit-identical at sharp unblurred hit boundaries.
   const float expected_a=c.mode==1?
     fl::alpha_for_emission(0,r,g,b):1.f;
   if(!std::isfinite(r)||!std::isfinite(g)||!std::isfinite(b)||
      !std::isfinite(alpha)){std::cerr<<"NaN "<<test<<"\n";return 8;}
   if(std::fabs(double(alpha)-expected_a)>1.e-3f)wrong_alpha++;
   for(int channel=0;channel<3;channel++){
    const float actual=got[4*i+(2-channel)];
    const float reference=expected[3*i+channel];
    const double error=std::abs(double(actual)-reference);
    rel+=error;energy+=std::abs(double(reference));
    max_error=std::max(max_error,error);
   }
   if(std::max({r,g,b})>1.f/255.f&&alpha>.5f/255.f)lit++;
  }
  const double relative=rel/std::max(energy,1.e-10);
  std::cout<<"GPU_POST_PARITY case="<<test<<" "
           <<c.w<<"x"<<c.h<<" grid="<<c.grid<<" spec="<<c.spectrum
           <<" mode="<<c.mode<<" haze="<<c.haze<<" footprint="<<c.haze_footprint
           <<" blur="<<c.blur<<" spectral_parallel="<<c.spectral_parallel
           <<" GPU_wall_ms="<<elapsed
           <<" rel_L1="<<relative<<" max_abs="<<max_error
           <<" alpha_mismatch="<<wrong_alpha<<" lit="<<lit
           <<" scratch_MB="<<double(size)/1048576<<"\n";
  if(relative>.02||wrong_alpha>pixels/1000||lit==0)return 9+int(test);
 }
 // Compare actual GPU source.rgb against Nuke's sequential block map.
 // Overlap must overwrite deterministically, not atomically add or tint.
 for(int step:{1,5,13}){
  const int w=127,h=91;
  const std::vector<fl::Source> lights{
    {15.5f/w,20.5f/h,3.2f,2.f,1.f},
    {16.5f/w,21.5f/h,90.f,23.f,7.f},
    {125.5f/w,89.5f/h,8.f,4.f,3.f},
    {-1.5f/w,36.5f/h,15.f,10.f,3.f},
    {.5f,.5f,30000.f,12500.f,700.f}};
  const auto expected=fl::nuke_preview_rgb(lights,w,h,step);
  const size_t bytes=size_t(w)*h*16;
  void* out=nullptr,*scratch=nullptr;
  const size_t scratch_size=fl::gpu::scratch_bytes(
    lens,int(lights.size()),12,w,h,3,false,false,false);
  if(!scratch_size||cudaMalloc(&out,bytes)!=cudaSuccess||
     cudaMalloc(&scratch,scratch_size)!=cudaSuccess)return 93;
  fl::gpu::View v{};
  v.output=out;v.width=w;v.height=h;v.output_pitch_bytes=w*16;
  v.output_mode=3;v.preview_step=step;v.input=nullptr;
  fl::Settings s{};s.grid=12;
  if(!fl::gpu::cuda_compose(lens,lights,s,v,scratch,scratch_size))return 94;
  std::vector<float> got(size_t(w)*h*4);
  if(cudaMemcpy(got.data(),out,bytes,cudaMemcpyDeviceToHost)!=cudaSuccess)return 95;
  cudaFree(scratch);cudaFree(out);
  double max_error=0;
  for(size_t i=0;i<size_t(w)*h;i++){
   for(int c=0;c<3;c++)
    max_error=std::max(max_error,std::abs(
      double(got[4*i+2-c])-double(expected[3*i+c])));
   max_error=std::max(max_error,std::abs(double(got[4*i+3])-1.));
  }
  if(max_error>1e-6){
   std::cerr<<"CUDA Nuke source.rgb mismatch stride="<<step
            <<" error="<<max_error<<"\n";return 96;
  }
  std::cout<<"CUDA_NUKE_SOURCE_RGB_PREVIEW_PARITY_PASSED stride="
           <<step<<" max_abs_error="<<max_error<<"\n";
 }
 std::cout<<"AE_CUDA_FULL_OPTICAL_POSTPROCESS_PARITY_OK\n";
 return 0;
}
