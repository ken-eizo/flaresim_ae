#pragma once
// AE-neutral, fixed-size, trivially-copyable optical prescription.
// No pointers or host resources in saved project data or render threads.
#include "lens.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>
namespace fl::custom {
inline constexpr uint32_t kMagic=0x534E4C46u; // FLNS
inline constexpr uint32_t kVersion=1;
inline constexpr int kMaxSurfaces=64;
struct Element {
 float radius=0, thickness=1, ior=1, abbe=0, semi=10;
 int32_t coating=0, stop=0;
};
struct Document {
 uint32_t magic=kMagic, version=kVersion, count=0, selected=0;
 float focal=50;
 Element elements[kMaxSurfaces]{};
};
static_assert(std::is_trivially_copyable_v<Document>);
inline bool valid(const Document& d) noexcept {
 if(d.magic!=kMagic||d.version!=kVersion||d.count<2||d.count>kMaxSurfaces||
    d.selected>=d.count||!std::isfinite(d.focal)||d.focal<=0||d.focal>2000)return false;
 for(uint32_t i=0;i<d.count;i++){
  const auto& e=d.elements[i];
  if(!std::isfinite(e.radius)||std::abs(e.radius)>1.e8f||
     !std::isfinite(e.thickness)||std::abs(e.thickness)>1000||
     !std::isfinite(e.ior)||e.ior<0.95f||e.ior>3.5f||
     !std::isfinite(e.abbe)||e.abbe<0||e.abbe>150||
     !std::isfinite(e.semi)||e.semi<0.01f||e.semi>1.e7f||
     e.coating<0||e.coating>32||(e.stop!=0&&e.stop!=1))return false;
  // Upstream optical prescriptions may declare a clear semi-aperture
  // larger than |R|. The upstream tracer treats non-intersecting edge
  // rays as misses. Preserve that prescription instead of rejecting it.
 }
 return true;
}
inline Document from_lens(const LensSystem& lens) {
 Document d{};
 d.count=uint32_t(std::min<int>(lens.num_surfaces(),kMaxSurfaces));
 d.focal=std::isfinite(lens.focal_length)&&lens.focal_length>0?lens.focal_length:50.f;
 for(uint32_t i=0;i<d.count;i++){
  const auto& s=lens.surfaces[i];
  d.elements[i]={s.radius,s.thickness,s.ior,s.abbe_v,s.semi_aperture,
                 s.coating,int32_t(s.is_stop)};
 }
 return d;
}
// Animate continuous prescription fields without altering the serialized v1
// layout. Structural edits (surface count) use hold interpolation: blending
// different optical topologies would pair unrelated physical surfaces.
// The UI selection belongs to the left document until the next keyframe.
inline Document interpolate(const Document& left,const Document& right,float t) noexcept {
 if(!valid(left)||!valid(right))return valid(left)?left:right;
 if(!std::isfinite(t)||t<=0.f)return left;
 if(t>=1.f)return right;
 if(left.count!=right.count)return left;
 Document result=left;
 const auto lerp=[t](float a,float b)->float{
  return float(double(a)+(double(b)-double(a))*double(t));
 };
 result.focal=lerp(left.focal,right.focal);
 for(uint32_t i=0;i<left.count;++i){
  const auto& a=left.elements[i];
  const auto& b=right.elements[i];
  auto& e=result.elements[i];
  e.radius=lerp(a.radius,b.radius);
  e.thickness=lerp(a.thickness,b.thickness);
  e.ior=lerp(a.ior,b.ior);
  e.abbe=lerp(a.abbe,b.abbe);
  e.semi=lerp(a.semi,b.semi);
  // Coating IDs and aperture-stop state are categorical, never fractional.
  e.coating=t<.5f?a.coating:b.coating;
  e.stop=t<.5f?a.stop:b.stop;
 }
 // Floating-point operations cannot be assumed to preserve validity at
 // every extreme, even when both saved endpoint prescriptions are valid.
 return valid(result)?result:(t<.5f?left:right);
}
inline LensSystem make_lens(const Document& doc){
 LensSystem lens;
 if(!valid(doc))return lens;
 lens.name="Custom Lens";
 lens.focal_length=doc.focal;
 lens.surfaces.reserve(doc.count);
 for(uint32_t i=0;i<doc.count;i++){
  const auto& e=doc.elements[i];
  Surface s{};
  s.radius=e.radius;s.thickness=e.thickness;s.ior=e.ior;
  s.abbe_v=e.abbe;s.semi_aperture=e.semi;s.coating=e.coating;
  s.is_stop=e.stop!=0;
  lens.surfaces.push_back(s);
 }
 lens.compute_geometry();
 return lens;
}
inline bool change_radius(Document& d,int i,float v){
 if(!valid(d)||i<0||i>=int(d.count)||!std::isfinite(v))return false;
 d.elements[i].radius=std::clamp(v,-1.e8f,1.e8f);
 return valid(d);
}
inline bool change_thickness(Document& d,int i,float v){
 if(!valid(d)||i<0||i>=int(d.count)||!std::isfinite(v))return false;
 d.elements[i].thickness=std::clamp(v,-1000.f,1000.f);
 return valid(d);
}
inline bool change_aperture(Document& d,int i,float v){
 if(!valid(d)||i<0||i>=int(d.count)||!std::isfinite(v))return false;
 auto& e=d.elements[i];
 e.semi=std::clamp(v,.01f,1.e7f);
 return valid(d);
}
inline bool add_after(Document& d,int i){
 if(!valid(d)||d.count>=kMaxSurfaces||i<0||i>=int(d.count))return false;
 for(int k=int(d.count);k>i+1;--k)d.elements[k]=d.elements[k-1];
 d.elements[i+1]=d.elements[i];
 d.elements[i+1].thickness=d.elements[i].thickness*.5f;
 d.elements[i].thickness*=.5f;
 d.count++;d.selected=uint32_t(i+1);
 return valid(d);
}
inline bool remove(Document& d,int i){
 if(!valid(d)||d.count<=2||i<0||i>=int(d.count))return false;
 if(i>0)d.elements[i-1].thickness+=d.elements[i].thickness;
 for(int k=i;k<int(d.count)-1;k++)d.elements[k]=d.elements[k+1];
 --d.count;d.selected=uint32_t(std::min(i,int(d.count)-1));
 return valid(d);
}
} // namespace fl::custom
