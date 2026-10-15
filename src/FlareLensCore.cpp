#include "FlareLensCore.hpp"
#include "FlareLensOptics.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <deque>
#include <cstring>
#include <memory>
#include <utility>

namespace fl {
namespace {
uint64_t optical_key(const LensSystem& lens,float sx,float sy){
 uint64_t h=1469598103934665603ULL;
 const auto mix=[&](uint32_t x){h^=x;h*=1099511628211ULL;};
 const auto mix_float=[&](float x){uint32_t bits=0;std::memcpy(&bits,&x,sizeof(bits));mix(bits);};
 mix(uint32_t(lens.surfaces.size()));
 mix_float(lens.focal_length);mix_float(lens.sensor_z);
 mix_float(sx);mix_float(sy);
 for(const auto& s:lens.surfaces){
  mix_float(s.radius);mix_float(s.z);mix_float(s.thickness);
  mix_float(s.ior);mix_float(s.abbe_v);mix_float(s.semi_aperture);
  mix(uint32_t(s.coating));mix(uint32_t(s.is_stop));
 }
 return h;
}
}
std::shared_ptr<const PairSelection> cached_pairs(
 const LensSystem& lens,float sensor_half_w,float sensor_half_h){
 using Entry=std::pair<uint64_t,std::shared_ptr<const PairSelection>>;
 static std::mutex mutex;
 static std::deque<Entry> cache;
 const uint64_t key=optical_key(lens,sensor_half_w,sensor_half_h);
 {
  std::lock_guard<std::mutex> lock(mutex);
  for(const auto& [candidate,pairs]:cache)if(candidate==key)return pairs;
 }
 // Heavy ray/spread estimates run *outside* the global lock.
 auto created=std::make_shared<PairSelection>();
 GhostConfig cfg{};
 filter_ghost_pairs(lens,sensor_half_w,sensor_half_h,cfg,
                    created->pairs,created->boosts);
 {
  std::lock_guard<std::mutex> lock(mutex);
  for(const auto& [candidate,pairs]:cache)if(candidate==key)return pairs;
  if(cache.size()>=24)cache.pop_front();
  cache.emplace_back(key,created);
 }
 return created;
}

Image render_cpu(const LensSystem& lens,const std::vector<Source>& sources,
                 const Settings& s,int w,int h,RenderControl control){
 Image out{w,h,{}};
 if(w<1||h<1||w>16384||h>16384||lens.num_surfaces()<2||
    sources.empty()||lens.focal_length<=0||s.max_pairs<=0)return out;
 if(size_t(w)*h>size_t(16384)*16384)return out;
 if(!s.debug_ray_density&&!(s.gain>0))return out;
 const float fov_h=std::clamp(s.fov_h_deg,1.f,150.f)*.0174532925199433f;
 const float sx=lens.focal_length*std::tan(fov_h*.5f);
 const float sy=s.fov_v_deg>0?
    lens.focal_length*std::tan(std::clamp(s.fov_v_deg,1.f,150.f)*.00872664625997f):
    sx*h/float(w);
 if(!(sx>0&&sy>0)||!std::isfinite(sx+sy))return out;
 GhostConfig cfg{};
 cfg.ray_grid=std::clamp(s.grid,2,2048);
 cfg.gain=std::max(0.f,s.gain);
 cfg.aperture_blades=s.aperture_blades;
 cfg.aperture_rotation_deg=s.aperture_rotation_deg;
 cfg.spectral_samples=s.spectral_samples;
 cfg.pupil_jitter=s.pupil_jitter;
 cfg.pupil_jitter_seed=s.jitter_seed;
 const auto selection=cached_pairs(lens,sx,sy);
 const auto& pairs=selection->pairs;
 const auto& boosts=selection->boosts;
 const int np=std::min<int>(int(pairs.size()),s.max_pairs);
 if(np<1)return out;
 const auto pupil=make_pupil(s);
 const auto spectrum=make_spectrum(s);
 if(pupil.empty()||spectrum.empty())return out;
 // Exact upstream ghost_cuda.cu scatter semantics: full-frame 2x2 bilinear
 // pixel splat, with ray_weight=1/n_pupil. No intermediate map, kernel,
 // resampling or compensating exposure.
 out.rgb.assign(size_t(w)*h*3,0);
 const float normalization=1.f/float(pupil.size());
 const float tan_h=std::tan(fov_h*.5f);
 for(const auto& light:sources){
  if(control.check_abort&&(out.abort_error=control.check_abort(control.context)))return out;
  const float ax=std::atan((light.x*2.f-1.f)*tan_h);
  const float ay=std::atan((light.y*2.f-1.f)*sy/lens.focal_length);
  if(!std::isfinite(ax+ay))continue;
  const Vec3f direction=Vec3f(std::tan(ax),std::tan(ay),1.f).normalized();
  const float source_rgb[3]={light.r,light.g,light.b};
  for(int p=0;p<np;p++){
   if(control.check_abort&&(out.abort_error=control.check_abort(control.context)))return out;
   const bool selected=pairs[p].surf_a==s.pair_surface_a&&
                       pairs[p].surf_b==s.pair_surface_b;
   if((s.pair_mode==1&&!selected)||(s.pair_mode==2&&selected))continue;
   out.diagnostics.active_pairs++;
   int sample_counter=0;
   for(const auto& sample:pupil){
    if((++sample_counter%128)==0&&control.check_abort&&
       (out.abort_error=control.check_abort(control.context)))return out;
    Ray ray{};
    ray.origin={sample.u*lens.surfaces.front().semi_aperture,
                sample.v*lens.surfaces.front().semi_aperture,
                lens.surfaces.front().z-20.f};
    ray.dir=direction;
    for(const auto& spec:spectrum){
     ++out.diagnostics.traced_rays;
     const auto res=trace_ghost_ray(ray,lens,pairs[p].surf_a,pairs[p].surf_b,spec.lambda);
     if(!res.valid||!std::isfinite(res.position.x)||!std::isfinite(res.position.y))continue;
     ++out.diagnostics.valid_rays;
     const float px=(res.position.x/(2*sx)+.5f)*w;
     const float py=(res.position.y/(2*sy)+.5f)*h;
     if(!std::isfinite(px)||!std::isfinite(py)||px<-2||py<-2||
        px>=w+2||py>=h+2)continue;
     ++out.diagnostics.sensor_hits;
     const int ix=int(std::floor(px-.5f)),iy=int(std::floor(py-.5f));
     const float fx=px-.5f-ix,fy=py-.5f-iy;
     // Diagnostic pass is intentionally bright at 8bpc; the actual ghost
     // radiance path below retains the unmodified Nuke weighting.
     const float base=s.debug_ray_density?1.f:(res.weight*cfg.gain*boosts[p]*normalization);
     if(!std::isfinite(base)||base<=0)continue;
     const float energy[3]={
       s.debug_ray_density?base:source_rgb[0]*spec.r*base,
       s.debug_ray_density?base:source_rgb[1]*spec.g*base,
       s.debug_ray_density?base:source_rgb[2]*spec.b*base
     };
     for(int dy=0;dy<2;dy++){
      const int y=iy+dy;
      if(y<0||y>=h)continue;
      const float wy=dy?fy:1.f-fy;
      for(int dx=0;dx<2;dx++){
       const int x=ix+dx;
       if(x<0||x>=w)continue;
       const float wt=wy*(dx?fx:1.f-fx);
       const size_t o=(size_t(y)*w+x)*3;
       for(int c=0;c<3;c++){
        const float v=energy[c]*wt;
        if(energy[c]>1e-14f&&std::isfinite(v))out.rgb[o+c]+=v;
       }
      }
     }
    }
   }
  }
 }
 // Exact upstream full-resolution scatter; diagnostics do not modify pixels.
 for(int y=0;y<h;y++){
  if((y&63)==0&&control.check_abort&&
     (out.abort_error=control.check_abort(control.context)))return out;
  for(int x=0;x<w;x++){
   const size_t o=(size_t(y)*w+x)*3;
   const float l=.2126f*out.rgb[o]+.7152f*out.rgb[o+1]+.0722f*out.rgb[o+2];
   if(l>0&&std::isfinite(l)){
    ++out.diagnostics.screen_samples;
    out.diagnostics.max_luminance=std::max(out.diagnostics.max_luminance,l);
    out.diagnostics.sum_luminance+=l;
   }
  }
 }
 return out;
}
} // namespace fl
