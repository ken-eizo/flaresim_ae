#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fl {
// Rendering geometry is not the same as the window/ROI checked out by AE.
// AE hands point parameters over already in the pixels of the world it
// renders (AE_Effect.h, PF_InData downsample_x: "the width and height of all
// effect parameters ... will be automatically adjusted"): 960,540 on a
// 1920x1080 layer arrives as 480,270 at Half. Only sliders measuring pixel
// distances need the downsample factor. in_data->width/height stay the
// full-resolution layer size.
// A SmartFX World is an arbitrary subrect with its own origin_x / origin_y.
struct HostGeometry {
 int full_w=0,full_h=0;   // current rendering resolution, not display zoom
 int output_x=0,output_y=0;
 int output_w=0,output_h=0;
 float downsample_x=1,downsample_y=1; // rendered pixels per layer pixel
 float sensor_w=0,sensor_h=0;         // layer size in rendered pixels
 bool valid() const noexcept {
  return full_w>0&&full_h>0&&full_w<=16384&&full_h<=16384&&
         output_w>0&&output_h>0&&
         int64_t(full_w)*full_h<=int64_t(16384)*16384;
 }
 int global_x(int x) const noexcept{return x+output_x;}
 int global_y(int y) const noexcept{return y+output_y;}
 // A point parameter (rendered pixels) as a fraction of the optical sensor.
 float normalized_x(float point_x) const noexcept {
  return sensor_w>0?point_x/sensor_w:.5f;
 }
 float normalized_y(float point_y) const noexcept {
  return sensor_h>0?point_y/sensor_h:.5f;
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
 g.downsample_x=dsx;g.downsample_y=dsy;
 const int inferred_w=source_w>0?int(std::lround(source_w*dsx)):0;
 const int inferred_h=source_h>0?int(std::lround(source_h*dsy)):0;
 // The source layer's full dimensions define the optical sensor. AE may
 // provide a GPU output World padded beyond the layer bounds; never grow
 // the sensor to that checkout rectangle (even for a same-sized solid).
 // Only fall back to the output extent if the host supplied no source size.
 g.full_w=inferred_w>0?inferred_w:
   std::max(1,output_w+std::max(0,output_x));
 g.full_h=inferred_h>0?inferred_h:
   std::max(1,output_h+std::max(0,output_y));
 // Unrounded, so a point keeps its fraction of the layer at every
 // downsample, even where the world size is rounded (1000 px at Third).
 g.sensor_w=inferred_w>0?float(source_w)*dsx:float(g.full_w);
 g.sensor_h=inferred_h>0?float(source_h)*dsy:float(g.full_h);
 return g;
}
}
