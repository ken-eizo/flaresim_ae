// Compare AE-native CUDA and CPU numerically against the TRUE unmodified
// LocalStarlight/flaresim_nuke CUDA renderer. Keep independent executables,
// original thin-film coating physics, immutable original lens prescriptions
// and a pixel-space HDR float reference; never compare two AE implementations
// to each other and call the result upstream parity.
#include "upstream_fixture_cases.hpp"
#include "FlareLensGpuBridge.hpp"
#include <cuda_runtime.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

int main(){
 int devices=0;
 if(cudaGetDeviceCount(&devices)!=cudaSuccess||devices==0)return 77;
 const auto all=flare_fixture::cases();
 const int w=flare_fixture::width,h=flare_fixture::height;
 const size_t n=size_t(w)*h;
 std::ifstream file(flare_fixture::output_path(),std::ios::binary);
 if(!file){std::cerr<<"Missing upstream fixture. Run generator first.\n";return 1;}
 std::array<uint32_t,4> hdr{};
 file.read(reinterpret_cast<char*>(hdr.data()),sizeof(hdr));
 if(!file||hdr[0]!=flare_fixture::signature||hdr[1]!=all.size()||
    hdr[2]!=unsigned(w)||hdr[3]!=unsigned(h))return 2;
 std::vector<float> reference(n*3),pixels(n*4);
 std::vector<float> input(n*4,0);
 for(size_t i=0;i<n;i++)input[4*i+3]=1.f;
 void *src=nullptr,*dst=nullptr;
 const auto cleanup=[&](){if(src)cudaFree(src);if(dst)cudaFree(dst);};
 if(cudaMalloc(&src,n*16)!=cudaSuccess||
    cudaMalloc(&dst,n*16)!=cudaSuccess){cleanup();return 3;}
 const fl::gpu::View view{src,dst,w,h,w*16,w*16};
 int checked=0;
 for(size_t k=0;k<all.size();k++){
  file.read(reinterpret_cast<char*>(reference.data()),
            std::streamsize(reference.size()*sizeof(float)));
  if(!file){cleanup();return 4;}
  const auto& c=all[k];
  const auto lens=flare_fixture::lens(c.lens);
  const auto settings=flare_fixture::settings(c);
  const auto sources=flare_fixture::sources(c);
  const size_t scratch_size=fl::gpu::scratch_bytes(lens,int(sources.size()),
                                                   settings.grid,w,h);
  void* scratch=nullptr;
  if(cudaMalloc(&scratch,scratch_size)!=cudaSuccess){cleanup();return 5;}
  const auto ok=cudaMemcpy(src,input.data(),n*16,cudaMemcpyHostToDevice)==cudaSuccess
    && fl::gpu::cuda_compose(lens,sources,settings,view,scratch,scratch_size)
    && cudaMemcpy(pixels.data(),dst,n*16,cudaMemcpyDeviceToHost)==cudaSuccess;
  cudaFree(scratch);
  if(!ok){cleanup();std::cerr<<"AE CUDA launch failed case "<<k<<"\n";return 6;}
  const auto cpu=fl::render_cpu(lens,sources,settings,w,h);
  if(cpu.rgb.size()!=reference.size()){cleanup();return 7;}
  double total=0,diff_gpu=0,diff_cpu=0;
  float worst_gpu=0,worst_cpu=0;
  for(size_t i=0;i<n;i++){
   if(std::fabs(pixels[4*i+3]-1.f)>1e-6f){cleanup();return 8;}
   for(int channel=0;channel<3;channel++){
    const float expected=reference[3*i+channel];
    const float actual_gpu=pixels[4*i+2-channel];
    const float actual_cpu=cpu.rgb[3*i+channel];
    if(!std::isfinite(expected)||!std::isfinite(actual_gpu)||
       !std::isfinite(actual_cpu)){cleanup();return 9;}
    const float dg=std::fabs(actual_gpu-expected);
    const float dc=std::fabs(actual_cpu-expected);
    diff_gpu+=dg;diff_cpu+=dc;total+=std::fabs(expected);
    worst_gpu=std::max(worst_gpu,dg);
    worst_cpu=std::max(worst_cpu,dc);
   }
  }
  const double rel_gpu=diff_gpu/std::max(1e-12,total);
  const double rel_cpu=diff_cpu/std::max(1e-12,total);
  std::cout<<"UPSTREAM_PARITY case="<<k<<" lens="<<c.lens
           <<" pupil="<<c.pupil<<" spectrum="<<c.spectral
           <<" ref_energy="<<total<<" AE_GPU_L1="<<rel_gpu
           <<" AE_CPU_L1="<<rel_cpu<<" GPU_max_error="<<worst_gpu
           <<" CPU_max_error="<<worst_cpu<<"\n";
  // A black reference does not establish parity. At least half the cases
  // must produce energy and pass the scale-independent numeric error test.
  if(total>1e-7)checked++;
  if((total>1e-7&&(rel_gpu>.00001||rel_cpu>.001))||
     (total<=1e-7&&(diff_gpu>1e-6||diff_cpu>1e-6))){cleanup();return 10;}
 }
 cleanup();
 if(checked<int(all.size()/2))return 11;
 std::cout<<"UNMODIFIED_NUKE_CUDA_GPU_CPU_REFERENCE_PARITY_OK nonzero_cases="<<checked
          <<" total_cases="<<all.size()<<"\n";
 return 0;
}
