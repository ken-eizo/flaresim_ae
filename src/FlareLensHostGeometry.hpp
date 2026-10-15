#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fl {
// Rendering geometry is not the same as the window/ROI checked out by AE.
// Point parameters are stored in full-resolution layer pixel coordinates.
// A SmartFX World is an arbitrary subrect with its own origin_x / origin_y.
struct HostGeometry {
 int full_w=0,full_h=0;   // current rendering resolution, not display zoom
 int output_x=0,output_y=0;
 int output_w=0,output_h=0;
 float light_scale_x=1,light_scale_y=1;
 bool valid() const noexcept {
  return full_w>0&&full_h>0&&full_w<=16384&&full_h<=16384&&
         output_w>0&&output_h>0&&
         int64_t(full_w)*full_h<=int64_t(16384)*16384;
 }
 int global_x(int x) const noexcept{return x+output_x;}
 int global_y(int y) const noexcept{return y+output_y;}
 float normalized_x(float full_res_x) const noexcept {
  return full_w>0?full_res_x*light_scale_x/float(full_w):.5f;
 }
 float normalized_y(float full_res_y) const noexcept {
  return full_h>0?full_res_y*light_scale_y/float(full_h):.5f;
 }
};

// Adobe's downsample rationals may be zero in legacy/minimal test hosts.
// Never infer the optical full frame from a partial checkout buffer.
inline HostGeometry host_geometry(int source_w,int source_h,
                                  int num_x,int den_x,int num_y,int den_y,
                                  int output_x,int output_y,
                                  int output_w,int output_h) noexcept{
 HostGeometry g;
 g.output_x=output_x;g.output_y=output_y;
 g.output_w=output_w;g.output_h=output_h;
 const float dsx=den_x>0&&num_x>0?
   std::clamp(float(num_x)/den_x,1.f/32.f,1.f):1.f;
 const float dsy=den_y>0&&num_y>0?
   std::clamp(float(num_y)/den_y,1.f/32.f,1.f):1.f;
 g.light_scale_x=dsx;g.light_scale_y=dsy;
 const int inferred_w=source_w>0?int(std::lround(source_w*dsx)):0;
 const int inferred_h=source_h>0?int(std::lround(source_h*dsy)):0;
 g.full_w=std::max({1,inferred_w,output_w+std::max(0,output_x)});
 g.full_h=std::max({1,inferred_h,output_h+std::max(0,output_y)});
 return g;
}
}
