#pragma once
#include "FlareLensGpuBridge.hpp"
#include "FlareLensGpuDetection.hpp"
#include <algorithm>
#include <cstddef>
namespace fl::gpu {
// GPU scratch is allocated by AE's GPUDeviceSuite, not by the plugin.
size_t metal_scratch_bytes(const LensSystem&,int,int,int,int,int,
                          bool,bool,bool) noexcept;
inline size_t metal_detection_scratch_bytes(int w,int h,int step) noexcept {
 if(w<1||h<1||w>16384||h>16384||step<1||step>32)return 0;
 const size_t bytes=size_t(std::max(1,w/step))*size_t(std::max(1,h/step))*16;
 return bytes<=size_t(128)*1024*1024?bytes:0;
}
bool metal_zero(void* buffer,size_t bytes,void* command_queue);
bool metal_copy_region(void* source,void* destination,void* command_queue,
 size_t src_pitch,size_t dst_pitch,int left,int top,int right,int bottom,
 int origin_x,int origin_y,int output_width,int output_height,bool padded);
bool detect_sources_metal(const void*,void*,size_t,void*,
 const DetectionSettings&,std::vector<fl::Source>&,
 std::vector<fl::Source>* raw_sources=nullptr);
bool metal_compose(const LensSystem&,const std::vector<Source>&,
                   const Settings&,const View&,void*,size_t);
}
