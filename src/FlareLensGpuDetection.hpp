#pragma once
#include "FlareLensCore.hpp"
#include <cstddef>
#include <vector>

namespace fl::gpu {
// Parameters are snapshot per AE pre-render and never shared mutably across
// MFR render callbacks. Coordinates are in the *actual* input GPU world.
// This stage only extracts the scene-linear HDR sources. The unchanged Nuke
// Fresnel / wavelength ghost-ray kernel executes in the second stage.
struct DetectionSettings {
 int width=0,height=0,input_pitch_bytes=0;
 int scan_step=4;
 int max_sources=512;
 float cluster_radius=0.f; // in rendered pixels, like width/height
 float source_threshold=2.f,source_cap=0.f;
 float tint_r=1.f,tint_g=1.f,tint_b=1.f;
 bool gate_alpha=false;
};
// Returns 0 for disallowed memory-intensive grids. This value may be passed
// to the Adobe GPUDeviceSuite allocator; no plugin-owned global allocations.
size_t detection_scratch_bytes(int width,int height,int scan_step) noexcept;
// input and scratch are device pointers, output is CPU metadata only.
// No full GPU frame is read to CPU: only an array of 4-float tile summaries.
// Every CUDA operation is submitted to the host's command_queuePV stream.
// Numeric comparator uses descending Nuke luma then ascending tile index.
bool detect_sources_cuda(const void* input,void* device_scratch,
 size_t scratch_size,void* host_cuda_stream,const DetectionSettings& options,
 std::vector<fl::Source>& out);
}
