#pragma once
// Original FlareSim for Nuke source ordering, angular-space clustering,
// and top-N filtering. See upstream/src/FlareSim.cpp cluster_sources and
// do_compute. No early best-K selection: Nuke clusters BEFORE max_sources.
// The caller separately retains the pre-cluster source list for haze.
#include "FlareLensCore.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
namespace fl {
inline float nuke_source_luma(const Source& s) noexcept {
 return .2126f*s.r+.7152f*s.g+.0722f*s.b;
}
inline std::vector<Source> nuke_select_sources(std::vector<Source> sources,
 int fmt_w,int fmt_h,float fov_h_deg,float fov_v_deg,
 int radius_px,int max_sources){
 if(sources.empty())return sources;
 if(radius_px>0&&sources.size()>1&&fmt_w>0&&fmt_h>0){
  const float tan_h=std::tan(fov_h_deg*.008726646259971648f);
  const float tan_v=fov_v_deg>0?
    std::tan(fov_v_deg*.008726646259971648f):
    tan_h*float(fmt_h)/fmt_w;
  const float per_px=2.f*tan_h/float(fmt_w);
  const float radius_angle=float(radius_px)*per_px;
  const float r2=radius_angle*radius_angle;
  // Copy upstream comparison/order: no secondary sort key on equal luma.
  std::sort(sources.begin(),sources.end(),
    [](const Source&a,const Source&b){
      return nuke_source_luma(a)>nuke_source_luma(b);
    });
  struct AngleSource {Source s;float ax,ay;};
  std::vector<AngleSource> angular;
  angular.reserve(sources.size());
  for(const auto& s:sources)
   angular.push_back({s,
     std::atan((s.x*2.f-1.f)*tan_h),
     std::atan((s.y*2.f-1.f)*tan_v)});
  std::vector<unsigned char> used(sources.size(),0);
  std::vector<Source> merged;
  merged.reserve(sources.size());
  for(size_t i=0;i<angular.size();i++){
   if(used[i])continue;
   const auto& seed=angular[i];
   float weight=nuke_source_luma(seed.s);
   float ax=seed.ax*weight,ay=seed.ay*weight;
   Source sum=seed.s;
   for(size_t j=i+1;j<angular.size();j++){
    if(used[j])continue;
    const float dx=seed.ax-angular[j].ax;
    const float dy=seed.ay-angular[j].ay;
    if(dx*dx+dy*dy<=r2){
      const float w=nuke_source_luma(angular[j].s);
      sum.r+=angular[j].s.r;sum.g+=angular[j].s.g;sum.b+=angular[j].s.b;
      ax+=angular[j].ax*w;ay+=angular[j].ay*w;
      weight+=w;
      used[j]=1;
    }
   }
   if(weight>0){
     const float x=std::tan(ax/weight)/(2.f*tan_h)+.5f;
     const float y=std::tan(ay/weight)/(2.f*tan_v)+.5f;
     sum.x=x;sum.y=y;
   }
   merged.push_back(sum);
  }
  sources.swap(merged);
 }
 if(max_sources>0&&sources.size()>size_t(max_sources)){
  std::partial_sort(sources.begin(),sources.begin()+max_sources,sources.end(),
    [](const Source&a,const Source&b){
      return nuke_source_luma(a)>nuke_source_luma(b);
    });
  sources.resize(size_t(max_sources));
 }
 return sources;
}
}
