#pragma once
#include "lens.h"
#include "ghost.h"
#include "trace.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>
namespace fl {
struct Source {float x=0.5f,y=0.5f,r=1,g=1,b=1;};
struct Settings {
 float fov_h_deg=40, fov_v_deg=0, gain=1000;
 int grid=32,max_pairs=40;
 int aperture_blades=0;float aperture_rotation_deg=0;
 int spectral_samples=3,pupil_jitter=0,jitter_seed=0;
 int pair_mode=0,pair_surface_a=0,pair_surface_b=1;
 bool debug_ray_density=false;
};
struct RenderDiagnostics {
 uint64_t traced_rays=0,valid_rays=0,sensor_hits=0;
 int active_pairs=0,screen_samples=0;
 float max_luminance=0;
 double sum_luminance=0;
};
struct Image {
 int width=0,height=0;
 std::vector<float> rgb;
 int abort_error=0;
 RenderDiagnostics diagnostics{};
};
struct ReconstructionSize {int width=1,height=1;};
inline ReconstructionSize reconstruction_size(int width,int height,int grid) noexcept {
 if(width<=0||height<=0)return {};
 const int target=grid<24?192:grid<48?256:512;
 const int mw=std::min(width,target);
 const int mh=std::max(1,std::min(height,int((int64_t(mw)*height+width/2)/width)));
 return {mw,mh};
}
struct PairSelection {
 std::vector<GhostPair> pairs;
 std::vector<float> boosts;
};
// Geometry-only, immutable, bounded, MFR-safe cache shared by CPU and CUDA.
std::shared_ptr<const PairSelection> cached_pairs(const LensSystem& lens,
                                                 float sensor_half_w,float sensor_half_h);
struct RenderControl {int (*check_abort)(void*)=nullptr;void* context=nullptr;};
Image render_cpu(const LensSystem& lens,const std::vector<Source>& src,const Settings& s,
                 int w,int h,RenderControl control={});
}
