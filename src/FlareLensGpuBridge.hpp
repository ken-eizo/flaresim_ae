#pragma once
#include "FlareLensCore.hpp"
#include <cstddef>
#include <vector>
namespace fl::gpu {
struct View {
 void* input=nullptr;
 void* output=nullptr;
 int width=0,height=0;
 int input_pitch_bytes=0,output_pitch_bytes=0;
 // Defaults preserve existing native CUDA parity-test behavior.
 int output_mode=1;       // 1=Composite, 2=Flare Only, 4=Ghost Only
 float ghost_blur=0.f;    // Nuke diagonal fraction
 int ghost_blur_passes=3;
 float haze_gain=0.f;
 float haze_radius=.15f;
 int haze_passes=3;
 const std::vector<Source>* haze_sources=nullptr; // Nuke haze sees all pre-cluster/cap tiles
 void* command_stream=nullptr; // PF_GPUDeviceInfo.command_queuePV (CUstream)
 int spectral_parallel=-1; // -1 auto, 0 serial, 1 parallel CUDA grid
 int haze_footprint=1; // Nuke fills each source block before haze convolution
 int preview_step=4; // Nuke source.rgb preview block width; independent of crosshair UI
 int (*check_abort)(void*)=nullptr; // host selector safe point (not CUDA kernel)
 void* abort_context=nullptr;
};
size_t scratch_bytes(const LensSystem&,int source_count,int ray_grid,
                     int width,int height,int output_mode=1,bool need_blur=true,
                     bool dense_haze=false,bool analytic_haze=true) noexcept;
bool cuda_compose(const LensSystem&,const std::vector<Source>&,
                  const Settings&,const View&,void* scratch,size_t scratch_size);
}
