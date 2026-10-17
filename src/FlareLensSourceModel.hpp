#pragma once
// An After Effects Point Control is a zero-area *location*, but Nuke's
// FlareSim constructs each optical source from one NxN image brightness
// block. A single manually placed AE light therefore cannot be directly
// compared to a luminous HDR region without accounting for that region's
// apparent image-plane area.
//
// For a uniformly bright circular disk of D pixels, the number of Nuke
// source blocks is approximately pi*(D/2)^2 / scan_step^2. This model is
// equivalent to using Nuke's optional Source Clustering to collapse that
// set of same-color HDR blocks to one centroid emitter: the optical math,
// RGB gain, pixel-splat energy and Fresnel terms are left unchanged.
//
// This is a MANUAL-only compressed area-source approximation, not the
// automatic source detector and not Nuke's literal unclustered multi-source
// distribution. Diameter zero means the original raw point-source mode.
#include <algorithm>
#include <cmath>

namespace fl {
// Opt-in experimental rendering policy: when the same optical sensor/FOV
// is rasterized at different resolutions, raw bilinear splatting conserves
// TOTAL energy, making fewer-pixel images brighter per pixel. Scale the
// manual emitter's energy by the raster area relative to 1920x1080.
// At FHD this is exactly 1, preserving all existing 1080p projects.
// Input coordinates must refer to the ACTUAL optical render sensor, not the
// source layer's full un-downsampled size.
inline float fhd_reference_emission_scale(int width,int height) noexcept {
 if(width<1||height<1||width>16384||height>16384)return 1.f;
 return float(double(width)*double(height)/(1920.0*1080.0));
}
inline float nuke_equivalent_manual_radiance(float source_radiance,
 float full_resolution_diameter_px,float scale_x,float scale_y,
 int scan_step,int max_sources) noexcept {
 if(!std::isfinite(source_radiance)||source_radiance<=0)return 0.f;
 if(!std::isfinite(full_resolution_diameter_px)||
    full_resolution_diameter_px<=0)return source_radiance;
 // Area is specified in FULL-RESOLUTION layer pixels and the Nuke source
 // scan step is also expressed in that coordinate space. AE adaptive
 // downsample may change rendering resolution, but must NOT change
 // the radiant power of a physical light. Recompute only its rasterization.
 (void)scale_x;
 (void)scale_y;
 const double radius=std::min(32768.,double(full_resolution_diameter_px)*.5);
 const double sample_area=3.1415926535897932384626433832795*
                          radius*radius;
 const double scan=std::clamp(scan_step,1,128);
 const double blocks=std::clamp(sample_area/(scan*scan),1.,
                                double(std::max(1,max_sources)));
 // Deliberately no empirical compensation, camera exposure or post-ghost
 // multiplier: this is only area integration of the Nuke input sampler.
 return float(std::min(1.e30,double(source_radiance)*blocks));
}
}
