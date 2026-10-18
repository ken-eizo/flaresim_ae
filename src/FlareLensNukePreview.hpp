#pragma once
// Original Nuke FlareSim::do_compute() source.rgb semantics:
// after clustering + Max Sources, overwrite (not add) each source's RGB
// over a scan_step x scan_step block at its reconstructed pixel centre.
#include "FlareLensCore.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>
namespace fl {
struct NukePreviewRect {int x0,y0,x1,y1;};
inline NukePreviewRect nuke_preview_rect(const Source& source,
 int width,int height,int scan_step) noexcept {
 const int step=std::clamp(scan_step,1,32);
 if(width<1||height<1||!std::isfinite(source.x)||
    !std::isfinite(source.y))return {0,0,0,0};
 const int cx=int(source.x*width),cy=int(source.y*height);
 return {std::max(0,cx-step/2),std::max(0,cy-step/2),
         std::min(width,cx-step/2+step),
         std::min(height,cy-step/2+step)};
}
inline std::vector<float> nuke_preview_rgb(
 const std::vector<Source>& sources,int width,int height,int scan_step){
 if(width<1||height<1)return {};
 std::vector<float> rgb(size_t(width)*height*3,0.f);
 for(const auto& source:sources){
  const auto r=nuke_preview_rect(source,width,height,scan_step);
  for(int y=r.y0;y<r.y1;y++)for(int x=r.x0;x<r.x1;x++){
   const size_t i=(size_t(y)*width+x)*3;
   rgb[i]=source.r;rgb[i+1]=source.g;rgb[i+2]=source.b;
  }
 }
 return rgb;
}
}
