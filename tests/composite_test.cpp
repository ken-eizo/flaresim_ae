// Regression for the real reported AE failure: the source is an EMPTY Shape
// Layer with zero-alpha input, but the optical flare must be visible as RGBA.
// Pure alpha checks are followed by actual 1920x1080 upstream ray tracer
// output and native AE-8bpc quantization, including the default 0.003 blur.
#include "FlareLensComposite.hpp"
#include "FlareLensCore.hpp"
#include "FlareLensOptics.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <vector>

static int test_opacity_rules(){
 using fl::alpha_for_emission;
 if(alpha_for_emission(0,0,0,0)!=0)return 1;
 if(alpha_for_emission(1,0,0,0)!=1)return 2;
 if(alpha_for_emission(.25f,0,0,0)!=.25f)return 3;
 if(alpha_for_emission(0,1,0,0)!=1)return 4;
 if(std::fabs(alpha_for_emission(0,.125f,.0625f,0)-.125f)>1e-6)return 5;
 if(std::fabs(alpha_for_emission(.25f,.125f,0,0)-.34375f)>1e-6)return 6;
 if(alpha_for_emission(0,2,0,0)!=1)return 7;
 if(alpha_for_emission(0,-1,0,0)!=0)return 8;
 if(alpha_for_emission(0,NAN,NAN,NAN)!=0)return 9;
 return 0;
}

int main(){
 if(const int failure=test_opacity_rules())return failure;
 const auto p=std::filesystem::path(__FILE__).parent_path().parent_path()/
   "upstream/lenses/lens_files/AI_Nikkor_50mm_F1.2S.lens";
 LensSystem lens;
 if(!lens.load(p.string().c_str()))return 10;
 constexpr int w=1920,h=1080;
 fl::Settings cfg;
 cfg.gain=1000;
 cfg.grid=64;
 cfg.max_pairs=2048;
 cfg.spectral_samples=3;
 // Screen shot: empty Shape Layer, manual light, source radiance 1000.
 std::vector<fl::Source> sources{{727.8f/w,320.3f/h,1000,1000,1000}};
 auto emission=fl::render_cpu(lens,sources,cfg,w,h);
 if(emission.rgb.size()!=size_t(w)*h*3)return 11;
 const int blur=int(std::round(.003f*std::hypot(float(w),float(h))));
 fl::box_blur_rgb(emission.rgb,w,h,blur,3);
 int opaque_visible=0,transparent_visible=0;
 float peak=0;
 for(size_t i=0;i<size_t(w)*h;i++){
  const auto off=i*3;
  const float r=emission.rgb[off],g=emission.rgb[off+1],b=emission.rgb[off+2];
  const float derived_alpha=fl::alpha_for_emission(0,r,g,b);
  const float maxrgb=std::max({r,g,b});
  // AE 8bpc requires nonzero *premultiplied* RGB AND nonzero output alpha.
  if(maxrgb>=.5f/255.f)opaque_visible++;
  if(maxrgb>=.5f/255.f && derived_alpha>=.5f/255.f)transparent_visible++;
  peak=std::max(peak,maxrgb);
 }
 std::cout<<"AE_TRANSPARENT_SHAPE_RENDER size="<<w<<"x"<<h
          <<" alpha_input=0 output_8bpc_nonzero="<<transparent_visible
          <<" rgb_nonzero="<<opaque_visible<<" peak="<<peak
          <<" sensor_hits="<<emission.diagnostics.sensor_hits
          <<" blur_radius="<<blur<<"\n";
 if(!emission.diagnostics.sensor_hits||opaque_visible<128||
    transparent_visible<128||!std::isfinite(peak))return 12;
 if(transparent_visible!=opaque_visible)return 13;
 // Reproduce the exact approximate screenshot aspect and optical source
 // coordinates across all four shipping prescriptions. Hostless tests must
 // not assume only the default Nikkor 50 can emit visible ghosts.
 constexpr int cw=960,ch=640;
 constexpr const char* lens_names[]={
  "AI_Nikkor_50mm_F1.2S.lens","AI_Nikkor_28mm_F1.4D.lens",
  "AI_Nikkor_35mm_F1.4S.lens",
  "Angenieux_35mm_f2.5_(Angenieux_35mm_f2.5_R1).lens"
 };
 for(int model=0;model<4;model++){
  const auto file=std::filesystem::path(__FILE__).parent_path().parent_path()/
    "upstream/lenses/lens_files"/lens_names[model];
  LensSystem selected;
  if(!selected.load(file.string().c_str()))return 20+model;
  auto alternate=cfg;
  alternate.grid=64;
  auto frame=fl::render_cpu(selected,
     {{727.8f/1920.f,320.3f/1280.f,1000,1000,1000}},alternate,cw,ch);
  if(frame.rgb.size()!=size_t(cw)*ch*3)return 24+model;
  const int rad=std::max(1,int(std::round(.003f*std::hypot(float(cw),float(ch)))));
  fl::box_blur_rgb(frame.rgb,cw,ch,rad,3);
  size_t nonzero=0;
  float p=0;
  for(float value:frame.rgb){
   nonzero+=value>.5f/255.f;
   p=std::max(p,value);
  }
  std::cout<<"AE_SCREENSHOT_PRESET_VISIBILITY lens="<<model
           <<" pixels="<<nonzero<<" peak="<<p
           <<" valid_rays="<<frame.diagnostics.valid_rays
           <<" sensor_hits="<<frame.diagnostics.sensor_hits<<"\n";
  if(nonzero<12||frame.diagnostics.sensor_hits<1)return 28+model;
 }
 std::cout<<"AE_RGBA_EMISSION_COMPOSITING_PASSED\n";
 return 0;
}
