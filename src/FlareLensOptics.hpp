#pragma once
#include "FlareLensCore.hpp"
#include "starburst.h"
#include <cmath>
#include <algorithm>
#include <vector>
#include <memory>
namespace fl {
struct PupilSample {float u=0,v=0;};
struct SpectralSample {float lambda=550,r=0,g=0,b=0;};
// Immutable shared samples: avoid copying up to 2 MiB per 512-grid CPU frame.
std::shared_ptr<const std::vector<PupilSample>> cached_pupil(const Settings& s);
// The same samples in Morton order, so neighbouring samples (one SIMD batch
// or GPU warp) sit in one small pupil patch and vignette together. Cached.
std::shared_ptr<const std::vector<PupilSample>> coherent_pupil(
  const std::shared_ptr<const std::vector<PupilSample>>& pupil);
std::vector<PupilSample> make_pupil(const Settings& s);
std::vector<SpectralSample> make_spectrum(const Settings& s);
// fraction (0..1) adds outer taps at radius+1 with that weight, so a blur
// radius need not be a whole pixel; 0 keeps the plain box filter.
void box_blur_rgb(std::vector<float>& rgb,int w,int h,int radius,int passes,
                  float fraction=0.f);
// A box blur whose radius is a fraction of the frame diagonal. At Full it is
// the whole-pixel radius. For a frame rendered at resolution_scale (AE Half
// = .5) it is the box with the same variance, in full-resolution pixels, as
// the Full frame's: whole radius plus outer taps of weight fraction.
struct BlurRadius {int whole=0;float fraction=0.f;};
inline BlurRadius frame_blur_radius(float relative,int w,int h,float resolution_scale,
                                    bool at_least_one) noexcept {
 const float s=resolution_scale>0.f&&resolution_scale<1.f?resolution_scale:1.f;
 float full=std::round(relative*std::hypot(float(w)/s,float(h)/s));
 if(at_least_one)full=std::max(1.f,full);
 if(!(full>0.f))return {};
 full=std::min(full,65536.f);
 if(s==1.f)return {int(full),0.f};
 // A radius-r box has variance r(r+1)/3; with outer taps f at r+1 it has
 // (r(r+1)(2r+1)/3+2f(r+1)^2)/(2r+1+2f).
 const double v=double(full)*(double(full)+1.)/3.*double(s)*double(s);
 int r=int((std::sqrt(1.+12.*v)-1.)*.5);
 while(r>0&&double(r)*(r+1)/3.>v)r--;
 while(double(r+1)*(r+2)/3.<=v)r++;
 const double f=(2.*r+1.)*(v-double(r)*(r+1)/3.)/(2.*((r+1.)*(r+1.)-v));
 return {r,float(std::clamp(f,0.,1.))};
}
// Box average of an RGB image by integer factors (w and h divisible by them).
std::vector<float> box_downscale_rgb(const std::vector<float>& rgb,int w,int h,int fx,int fy);
void render_haze(std::vector<float>& rgb,const std::vector<Source>& sources,int w,int h,
                 float gain,float radius,int passes,int source_footprint=1,
                 float resolution_scale=1.f);
// supersample > 1: each pixel averages supersample^2 full-resolution pixels
// (an AE preview raster), as the Full frame scaled down.
void render_diffraction(std::vector<float>& rgb,const std::vector<Source>& sources,
                        int w,int h,int blades,float rotation,float gain,float scale,
                        int supersample=1);

// Diffraction starburst geometry shared by the CPU and CUDA renderers so both
// place every source and sample the PSF with identical numbers.
struct StarPlacement {float px=0,py=0,rgb[3]{};int x0=0,y0=0,x1=0,y1=0;};
struct StarburstSetup {
 std::shared_ptr<const StarburstPSF> psf;
 int N=0;
 float half_N=0,r_ref=0,gain=0,scale_ch[3]{};
 std::vector<StarPlacement> stars; // sources with a visible footprint
};
std::shared_ptr<const StarburstPSF> cached_starburst_psf(int blades,float rotation);
bool starburst_setup(const std::vector<Source>& sources,int w,int h,int blades,
                     float rotation,float gain,float scale,StarburstSetup& out);
}
