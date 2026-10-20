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
 void* command_stream=nullptr; // PF_GPUDeviceInfo.command_queuePV (CUstream)
 int spectral_parallel=-1; // -1 auto, 0 serial, 1 parallel CUDA grid
 int haze_footprint=1; // Nuke fills each source block before haze convolution
 int (*check_abort)(void*)=nullptr; // host selector safe point (not CUDA kernel)
 void* abort_context=nullptr;
 // Diffraction starburst (Composite / Flare Only), same as the CPU pass.
 float star_gain=0.f,star_scale=.15f,star_rotation=0.f;
 int star_blades=0;
 // Rendered pixels per full-resolution pixel (AE Half = .5): blur and haze
 // radii are the Full frame's, scaled.
 float resolution_scale=1.f;
 // Integer AE preview (Half = 2): the ghost layer is traced and blurred at
 // full resolution, then box-averaged onto this output; diffraction is
 // supersampled the same way. 1 renders at the output size.
 int raster_x=1,raster_y=1;
};
size_t scratch_bytes(const LensSystem&,int source_count,int ray_grid,
                     int width,int height,int output_mode=1,bool need_blur=true,
                     bool dense_haze=false,bool analytic_haze=true,
                     bool starburst=false,int raster_x=1,int raster_y=1) noexcept;
bool cuda_compose(const LensSystem&,const std::vector<Source>&,
                  const Settings&,const View&,void* scratch,size_t scratch_size);
}
