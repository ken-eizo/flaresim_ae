#include "FlareLensCore.hpp"
#include "FlareLensSourceModel.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
int main(){
 LensSystem lens;
 // Manual intensity policy is a separate opt-in from upstream Nuke.
 if(fl::fhd_reference_emission_scale(1920,1080)!=1.f||
    std::abs(fl::fhd_reference_emission_scale(1280,720)-4.f/9.f)>1e-7f||
    std::abs(fl::fhd_reference_emission_scale(960,540)-.25f)>1e-7f||
    std::abs(fl::fhd_reference_emission_scale(3840,2160)-4.f)>1e-7f||
    fl::fhd_reference_emission_scale(0,720)!=1.f)return 54;
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
 // A constant lens/FOV/source in Nuke scales with the output FORMAT:
 // changing 16:9 resolution changes pixel size, not the normalized physical
 // footprint. An accidental fixed-pixel flare will fail these moments.
 const auto smaller=fl::render_cpu(lens,{{.5f,.5f,1,1,1}},
                                   default_ae,160,90);
 if(smaller.rgb.size()!=size_t(160)*90*3)return 51;
 auto centroid=[](const fl::Image& img){
  double mass=0,cx=0,cy=0;
  for(int y=0;y<img.height;y++)for(int x=0;x<img.width;x++){
   const size_t k=(size_t(y)*img.width+x)*3;
   const double value=.2126*img.rgb[k]+.7152*img.rgb[k+1]+.0722*img.rgb[k+2];
   if(!std::isfinite(value)||value<0)return std::array<double,3>{-1,-1,-1};
   mass+=value;cx+=value*((x+.5)/img.width);
   cy+=value*((y+.5)/img.height);
  }
  return std::array<double,3>{mass,mass>0?cx/mass:0,mass>0?cy/mass:0};
 };
 const auto hi=centroid(visible),lo=centroid(smaller);
 if(hi[0]<=0||lo[0]<=0||
    std::abs(hi[1]-lo[1])>.035||std::abs(hi[2]-lo[2])>.035){
   std::cerr<<"NUKE_FORMAT_RESOLUTION_INVARIANCE_FAILED hi=("
     <<hi[0]<<","<<hi[1]<<","<<hi[2]<<") lo=("
     <<lo[0]<<","<<lo[1]<<","<<lo[2]<<")\n";
   return 52;
 }
 // A central light is symmetric and could conceal a fixed-pixel mapping.
 // Confirm the SAME angular emitter away from centre tracks resolution.
 const auto shifted_hi=fl::render_cpu(lens,{{.62f,.43f,1,1,1}},
                                      default_ae,320,180);
 const auto shifted_lo=fl::render_cpu(lens,{{.62f,.43f,1,1,1}},
                                      default_ae,160,90);
 const auto sh=centroid(shifted_hi),sl=centroid(shifted_lo);
 if(sh[0]<=0||sl[0]<=0||
    std::abs(sh[1]-sl[1])>.035||std::abs(sh[2]-sl[2])>.035)
   return 53;
 std::cout<<"NUKE_FORMAT_RESOLUTION_INVARIANCE_PASSED 320x180->160x90 "
          <<"centroid normalized "<<sh[1]<<","<<sh[2]
          <<" -> "<<sl[1]<<","<<sl[2]<<"\n";
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
