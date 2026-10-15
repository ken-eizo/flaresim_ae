#include "FlareLensCore.hpp"
#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
int main(){
 LensSystem lens;
 const auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/
   "upstream/lenses/lens_files/AI_Nikkor_50mm_F1.2S.lens";
 if(!lens.load(path.string().c_str())){std::cerr<<"lens fixture could not load: "<<path<<"\n";return 1;}
 if(lens.num_surfaces()<2 || !(lens.focal_length>0) || !std::isfinite(lens.sensor_z))return 2;
 fl::Settings s; s.grid=4; s.max_pairs=3; s.gain=0;
 const auto empty=fl::render_cpu(lens,{},s,64,36);
 if(!empty.rgb.empty())return 3;
 const auto zero=fl::render_cpu(lens,{{.5f,.5f,1,1,1}},s,64,36);
 if(!zero.rgb.empty())return 4;
 s.gain=100; 
 const auto rendered=fl::render_cpu(lens,{{.5f,.5f,1,1,1}},s,64,36);
 if(rendered.width!=64 || rendered.height!=36 || rendered.rgb.size()!=64*36*3)return 5;
 for(const float v:rendered.rgb)if(!std::isfinite(v)||v<0)return 6;
 const auto invalid=fl::render_cpu(lens,{{.5f,.5f,1,1,1}},s,16385,36);
 if(!invalid.rgb.empty())return 7;
 // Regression: an all-black output with a successful return value is NOT a
 // correctly rendered flare. Verify the exact new-effect default settings
 // against the shipping lens and an opaque black plate at SDR resolution.
 fl::Settings default_ae;
 default_ae.grid=16;
 default_ae.max_pairs=2048;
 default_ae.gain=1000.f;
 const auto visible=fl::render_cpu(lens,{{.5f,.5f,1,1,1}},
                                   default_ae,320,180);
 std::cout<<"default lens: candidate_pairs="<<visible.diagnostics.active_pairs
          <<" valid_rays="<<visible.diagnostics.valid_rays
          <<" sensor_hits="<<visible.diagnostics.sensor_hits
          <<" lit_pixels="<<visible.diagnostics.screen_samples
          <<" peak="<<visible.diagnostics.max_luminance
          <<" sum="<<visible.diagnostics.sum_luminance<<"\n";
 if(visible.rgb.size()!=size_t(320)*180*3||
    visible.diagnostics.active_pairs<1||
    visible.diagnostics.valid_rays<1||
    visible.diagnostics.sensor_hits<1||
    visible.diagnostics.screen_samples<1||
    !(visible.diagnostics.max_luminance>0.f))return 8;
 // Ray-hit preview must be independent of Fresnel strength/gain.
 default_ae.debug_ray_density=true;
 default_ae.gain=0;
 const auto rays=fl::render_cpu(lens,{{.5f,.5f,1,1,1}},
                                default_ae,160,90);
 if(rays.diagnostics.sensor_hits==0 ||
    rays.diagnostics.max_luminance<=0)return 9;
 const auto first=fl::cached_pairs(lens,12.f,8.f);
 const auto second=fl::cached_pairs(lens,12.f,8.f);
 if(!first||first!=second)return 10;
 const char* names[]={
  "AI_Nikkor_50mm_F1.2S.lens",
  "AI_Nikkor_28mm_F1.4D.lens",
  "AI_Nikkor_35mm_F1.4S.lens",
  "Angenieux_35mm_f2.5_(Angenieux_35mm_f2.5_R1).lens"
 };
 for(int i=0;i<4;i++){
  const auto p=std::filesystem::path(__FILE__).parent_path().parent_path()/
     "upstream/lenses/lens_files"/names[i];
  LensSystem candidate;
  if(!candidate.load(p.string().c_str())||candidate.num_surfaces()<2||
     !(candidate.focal_length>0)||!std::isfinite(candidate.sensor_z)){
   std::cerr<<"invalid optical preset: "<<p<<"\n";
   return 20+i;
  }
  const auto preview=fl::render_cpu(candidate,{{.5f,.5f,1,1,1}},s,64,36);
  if(preview.rgb.size()!=64*36*3)return 30+i;
  for(float v:preview.rgb)if(!std::isfinite(v)||v<0)return 40+i;
 }
 const auto aborted=fl::render_cpu(lens,{{.5f,.5f,1,1,1}},s,64,36,
    {[](void*)->int{return 123;},nullptr});
 if(aborted.abort_error!=123)return 50;
 std::cout<<"optical input/render and all 4 real lens presets plus cancellation verified\n";
 return 0;
}
