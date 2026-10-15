// Real HD frame regression: quantify AE 8/16bpc quantization of Nuke's HDR
// output. The original Nuke node renders a FLOAT buffer, not an 8bpc solid.
#include "upstream_fixture_cases.hpp"
#include "FlareLensOptics.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

int main(){
 constexpr int w=1920,h=1080;
 int valid_lenses=0;
 for(int model=0;model<4;model++){
  const auto lens=flare_fixture::lens(model);
  fl::Settings settings;
  settings.grid=16;settings.max_pairs=2048;
  settings.gain=1000.f;
  settings.spectral_samples=3;
  settings.pupil_jitter=2; // Halton ensures regular dots are not correlated.
  const fl::Source light{.5f,.5f,1000.f,1000.f,1000.f};
  const auto hdr=fl::render_cpu(lens,{light},settings,w,h);
  if(hdr.rgb.size()!=size_t(w)*h*3)return 1;
  size_t visible8=0,visible16=0;
  double total=0;
  float peak=0;
  for(float v:hdr.rgb){
   if(!std::isfinite(v)||v<0)return 2;
   visible8+=v>0.5f/255.f;
   visible16+=v>0.5f/32768.f;
   total+=v;peak=std::max(peak,v);
  }
  // These are production-world 1920x1080 Nuke-equivalent source radiances.
  std::cout<<"HD_VISIBILITY lens="<<model
    <<" grid=16 HDR_source=1000 gain=1000"
    <<" peak_linear="<<peak<<" RGB8_nonzero="<<visible8
    <<" RGB16_nonzero="<<visible16<<" total="<<total
    <<" valid_hits="<<hdr.diagnostics.sensor_hits<<"\n";
  if(hdr.diagnostics.sensor_hits>0&&visible8>=8)valid_lenses++;
 }
 if(valid_lenses<2){
  std::cerr<<"FAIL: 8bpc output is quantized to black for manual source=1000.\n";
  return 3;
 }
 // Nuke defaults: grid64, optical ghost blur radius .003, three passes.
 // Check full-resolution 8bpc visibility after the exact Nuke box blur.
 {
  const auto lens=flare_fixture::lens(0);
  fl::Settings quality;
  quality.grid=64;quality.gain=1000;quality.max_pairs=2048;
  auto full=fl::render_cpu(lens,{{.5f,.5f,1000,1000,1000}},quality,w,h);
  const int radius=int(std::round(.003f*std::hypot(float(w),float(h))));
  fl::box_blur_rgb(full.rgb,w,h,radius,3);
  size_t nonzero=0;
  float peak=0;
  for(float value:full.rgb){
   nonzero+=value>0.5f/255.f;
   peak=std::max(peak,value);
  }
  std::cout<<"HD_UPSTREAM_DEFAULT_GHOST_BLUR radius="<<radius
           <<" grid=64 peak="<<peak<<" 8bpc_nonzero="<<nonzero<<"\n";
  if(nonzero<32||!std::isfinite(peak))return 6;
 }
 const auto lens=flare_fixture::lens(0);
 fl::Settings settings;
 settings.gain=1000;settings.max_pairs=2048;settings.grid=16;
 const auto ldr=fl::render_cpu(lens,{{.5f,.5f,1,1,1}},settings,w,h);
 const auto hdr=fl::render_cpu(lens,{{.5f,.5f,1000,1000,1000}},settings,w,h);
 if(ldr.rgb.size()!=hdr.rgb.size())return 4;
 // The manual emitter scale changes only the source HDR radiance; the exact
 // optics are linear. Never silently normalize per-frame ghost intensity.
 double source_ratio_error=0,signal=0;
 for(size_t i=0;i<hdr.rgb.size();i++){
  const double intended=double(ldr.rgb[i])*1000;
  signal+=std::fabs(hdr.rgb[i]);
  source_ratio_error+=std::fabs(hdr.rgb[i]-intended);
 }
 const double relative=source_ratio_error/std::max(1e-12,signal);
 std::cout<<"HD_SOURCE_LINEARITY_RMS_L1="<<relative<<"\n";
 if(relative>1e-4)return 5;
 return 0;
}
