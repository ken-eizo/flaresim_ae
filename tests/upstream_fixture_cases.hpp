#pragma once
#include "FlareLensCore.hpp"
#include <array>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace flare_fixture {
constexpr int width=256,height=144;
constexpr unsigned signature=0xF1A6E322u;
struct Case {
 int lens=0,pupil=0,spectral=3;
 float x=.5f,y=.5f;
};
inline std::vector<Case> cases(){
 std::vector<Case> result;
 for(int lens=0;lens<4;lens++)
  for(int pupil=0;pupil<3;pupil++)
   for(int spectral:{3,5,11})
    result.push_back({lens,pupil,spectral,
      .39f+.035f*lens,.48f+.05f*pupil});
 return result;
}
inline const std::array<const char*,4>& names(){
 static const std::array<const char*,4> v={{
  "AI_Nikkor_50mm_F1.2S.lens",
  "AI_Nikkor_28mm_F1.4D.lens",
  "AI_Nikkor_35mm_F1.4S.lens",
  "Angenieux_35mm_f2.5_(Angenieux_35mm_f2.5_R1).lens"
 }};
 return v;
}
inline LensSystem lens(int which){
 if(which<0||which>=int(names().size()))throw std::runtime_error("invalid lens");
 LensSystem l;
 const auto p=std::filesystem::path(__FILE__).parent_path().parent_path()/
  "upstream/lenses/lens_files"/names()[which];
 if(!l.load(p.string().c_str()))throw std::runtime_error("missing lens "+p.string());
 return l;
}
inline fl::Settings settings(const Case& c){
 fl::Settings s;
 s.gain=1000.f;s.grid=24;s.max_pairs=2048;
 s.fov_h_deg=40;s.pupil_jitter=c.pupil;
 s.spectral_samples=c.spectral;
 s.jitter_seed=137;
 s.aperture_blades=c.lens%2==0?0:6;
 s.aperture_rotation_deg=c.lens*11.f;
 return s;
}
inline std::vector<fl::Source> sources(const Case& c){
 // A genuine Nuke-like scene-linear highlight, NOT LDR RGB=(1,1,1).
 return {{c.x,c.y,300.f,140.f,75.f}};
}
inline std::filesystem::path output_path(){
 return std::filesystem::current_path()/"FlareLensUpstreamRGB32f.fixture";
}
}
