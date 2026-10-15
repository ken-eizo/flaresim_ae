// Non-default host CUDA stream, pitched BGRA128 worlds, minimum GPU scratch,
// and guard bytes. These are distinct AE-specific hazards that cannot be
// detected by tight-row, default-stream ray-tracing unit tests.
#include "FlareLensGpuBridge.hpp"
#include "FlareLensOptics.hpp"
#include "FlareLensComposite.hpp"
#include <cuda_runtime.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <vector>
#include <array>
#include <atomic>
#include <thread>
#include <numeric>

int main(){
 int devices=0;if(cudaGetDeviceCount(&devices)!=cudaSuccess||devices==0)return 77;
 LensSystem lens;
 const auto filename=std::filesystem::path(__FILE__).parent_path().parent_path()/
  "upstream/lenses/lens_files/AI_Nikkor_50mm_F1.2S.lens";
 if(!lens.load(filename.string().c_str()))return 1;
 // Exact scratch allocator contract for quality levels beyond 512.
 const size_t mem512=fl::gpu::scratch_bytes(lens,1,512,320,180,2,false);
 const size_t mem1024=fl::gpu::scratch_bytes(lens,1,1024,320,180,2,false);
 const size_t mem2048=fl::gpu::scratch_bytes(lens,1,2048,320,180,2,false);
 if(!(mem512>0&&mem1024>mem512&&mem2048>mem1024)||
    fl::gpu::scratch_bytes(lens,1,2049,320,180,2,false)!=0)return 19;
 // Ghost-only render must not reserve 1D haze profiles for 512 sources.
 const auto old_scratch=fl::gpu::scratch_bytes(lens,512,64,1920,1080,4,false,false,true);
 const auto slim_scratch=fl::gpu::scratch_bytes(lens,512,64,1920,1080,4,false,false,false);
 const size_t saved=size_t(512)*(1920+1080)*sizeof(float);
 if(old_scratch<=slim_scratch||old_scratch-slim_scratch!=saved)return 20;
 std::cout<<"AE_GPU_GHOST_MEMORY_SAVED_BYTES="<<saved<<"\n";
 constexpr int w=319,h=181;
 const size_t bytes_per_row=size_t(w)*16;
 void *source=nullptr,*destination=nullptr;
 size_t pitch_in=0,pitch_out=0;
 cudaStream_t stream=nullptr;
 if(cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking)!=cudaSuccess)return 2;
 if(cudaMallocPitch(&source,&pitch_in,bytes_per_row,h)!=cudaSuccess||
    cudaMallocPitch(&destination,&pitch_out,bytes_per_row,h)!=cudaSuccess)return 3;
 if(pitch_in==bytes_per_row||pitch_out==bytes_per_row){
  std::cerr<<"CUDA pitch alignment not exercised\n";return 4;
 }
 fl::Settings settings;
 settings.grid=64;settings.spectral_samples=7;
 settings.aperture_blades=6;settings.pupil_jitter=2;settings.gain=1000;
 std::vector<fl::Source> light{{.62f,.31f,10000,10000,10000}};
 const std::array<int,4> modes={1,2,4,5};
 for(int mode:modes){
  fl::gpu::View view{};
  view.input=source;view.output=destination;
  view.width=w;view.height=h;
  view.input_pitch_bytes=int(pitch_in);
  view.output_pitch_bytes=int(pitch_out);
  view.output_mode=mode;
  if(mode!=1){view.input=nullptr;view.input_pitch_bytes=0;}
  view.ghost_blur=(mode==1)?0.003f:0;
  view.ghost_blur_passes=3;
  view.haze_gain=(mode==1||mode==5)?1.f:0.f;
  view.haze_radius=.15f;view.haze_passes=3;
  view.command_stream=reinterpret_cast<void*>(stream);
  const size_t scratch_size=fl::gpu::scratch_bytes(
    lens,int(light.size()),settings.grid,w,h,mode,
    view.ghost_blur>0&&view.ghost_blur_passes>0,
    false,view.haze_gain>0);
  void* scratch=nullptr;
  if(cudaMalloc(&scratch,scratch_size+128)!=cudaSuccess)return 5;
  if(cudaMemsetAsync(source,0,pitch_in*h,stream)!=cudaSuccess)return 6;
  if(cudaMemsetAsync(scratch,0xA5,scratch_size+128,stream)!=cudaSuccess)return 7;
  if(!fl::gpu::cuda_compose(lens,light,settings,view,scratch,scratch_size)){
   std::cerr<<"Nondefault-stream render failed mode="<<mode<<"\n";return 8;
  }
  std::array<uint8_t,128> guard{};
  if(cudaMemcpy(guard.data(),static_cast<uint8_t*>(scratch)+scratch_size,
                guard.size(),cudaMemcpyDeviceToHost)!=cudaSuccess)return 9;
  if(!std::all_of(guard.begin(),guard.end(),[](uint8_t b){return b==0xA5;})){
   std::cerr<<"Overran minimum device scratch mode "<<mode<<"\n";return 10;
  }
  std::vector<float> pixels(size_t(w)*h*4);
  if(cudaMemcpy2DAsync(pixels.data(),bytes_per_row,destination,pitch_out,
       bytes_per_row,h,cudaMemcpyDeviceToHost,stream)!=cudaSuccess)return 11;
  if(cudaStreamSynchronize(stream)!=cudaSuccess)return 12;
  size_t visible=0;
  for(size_t i=0;i<size_t(w)*h;i++){
   const float peak=std::max({pixels[i*4],pixels[i*4+1],pixels[i*4+2]});
   const float a=pixels[i*4+3];
   if(!std::isfinite(peak)||!std::isfinite(a)||a<0||a>1)return 13;
   if(mode==1&&std::fabs(a-fl::alpha_for_emission(
      0,pixels[i*4+2],pixels[i*4+1],pixels[i*4]))>1e-4)return 14;
   if(peak>.5f/255.f&&a>.5f/255.f)visible++;
  }
  std::cout<<"AE_GPU_PADDED_PITCH mode="<<mode
           <<" rowbytes_src="<<pitch_in<<" rowbytes_dst="<<pitch_out
           <<" scratch_bytes="<<scratch_size<<" visible="<<visible
           <<" canary=ok non_default_stream=ok\n";
  if(visible<1)return 15;
  cudaFree(scratch);
 }
 cudaFree(source);cudaFree(destination);cudaStreamDestroy(stream);
 std::cout<<"AE_GPU_PADDED_PITCH_MINIMUM_SCRATCH_PASSED\n";
 // AE MFR renders simultaneous frames/effect instances. Never share
 // mutable device scratch, CUDA streams or per-frame input buffers.
 std::array<double,4> independent_energy{};
 std::atomic<int> errors{0};
 std::array<std::thread,4> workers;
 for(int t=0;t<4;t++)workers[t]=std::thread([&,t]{
  if(cudaSetDevice(0)!=cudaSuccess){errors++;return;}
  cudaStream_t local=nullptr;
  void *out=nullptr,*scratch=nullptr;
  if(cudaStreamCreateWithFlags(&local,cudaStreamNonBlocking)!=cudaSuccess){
   errors++;return;
  }
  constexpr int tw=320,th=180;
  const size_t scratch_bytes=fl::gpu::scratch_bytes(
    lens,1,settings.grid,tw,th,2,true,false,false);
  const size_t bytes=size_t(tw)*th*16;
  if(cudaMalloc(&out,bytes)!=cudaSuccess||
     cudaMalloc(&scratch,scratch_bytes)!=cudaSuccess){
   errors++;if(out)cudaFree(out);if(scratch)cudaFree(scratch);
   cudaStreamDestroy(local);return;
  }
  fl::gpu::View v{};
  v.output=out;v.width=tw;v.height=th;
  v.output_pitch_bytes=tw*16;v.output_mode=2;
  v.ghost_blur=.003f;v.ghost_blur_passes=3;
  v.command_stream=reinterpret_cast<void*>(local);
  if(!fl::gpu::cuda_compose(lens,light,settings,v,scratch,scratch_bytes)){
   errors++;
  }else{
   std::vector<float> result(size_t(tw)*th*4);
   if(cudaMemcpy(result.data(),out,bytes,cudaMemcpyDeviceToHost)!=cudaSuccess)
    errors++;
   else{
    for(size_t i=0;i<result.size();i+=4){
     independent_energy[t]+=result[i]+result[i+1]+result[i+2];
    }
   }
  }
  cudaFree(scratch);cudaFree(out);cudaStreamDestroy(local);
 });
 for(auto& thread:workers)thread.join();
 const double average=std::accumulate(
   independent_energy.begin(),independent_energy.end(),0.)/4.;
 double worst_rel=0;
 for(const auto energy:independent_energy)
  worst_rel=std::max(worst_rel,
    std::abs(energy-average)/std::max(average,1e-10));
 std::cout<<"AE_MFR_INDEPENDENT_CUDA_STREAMS threads=4 energy="
          <<average<<" worst_relative="<<worst_rel
          <<" errors="<<errors.load()<<"\n";
 return errors.load()||average<=0||worst_rel>.001?18:0;
}
