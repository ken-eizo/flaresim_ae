#include "FlareLensOptics.hpp"
#include "FlareLensNukeHaze.hpp"
#include "starburst.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <deque>
#include <memory>
#include <mutex>
#include <cstdint>
#include <numeric>
#include <vector>
namespace fl {
namespace {
constexpr float pi=3.14159265358979323846f;
uint32_t wang(uint32_t x){
 x=(x^61u)^(x>>16); x*=9u; x^=x>>4;
 x*=0x27d4eb2du;x^=x>>15;return x;
}
float halton2(uint32_t x){
 x=(x<<16)|(x>>16);
 x=((x&0x00ff00ffu)<<8)|((x&0xff00ff00u)>>8);
 x=((x&0x0f0f0f0fu)<<4)|((x&0xf0f0f0f0u)>>4);
 x=((x&0x33333333u)<<2)|((x&0xccccccccu)>>2);
 x=((x&0x55555555u)<<1)|((x&0xaaaaaaaau)>>1);
 return float(x)*(1.f/4294967296.f);
}
float halton3(uint32_t x){
 float r=0,f=1.f/3.f;
 while(x){r+=(x%3u)*f;x/=3;f/=3;}
 return r;
}
} // namespace
std::shared_ptr<const std::vector<PupilSample>> cached_pupil(const Settings& s){
 const int n=std::clamp(s.grid,2,2048);
 const int blades=std::clamp(s.aperture_blades,0,16);
 const float rotate=std::isfinite(s.aperture_rotation_deg)?s.aperture_rotation_deg*pi/180.f:0;
 const float sector=blades>=3?2*pi/blades:1;
 const float apothem=blades>=3?std::cos(pi/blades):1;
 const uint32_t seed=uint32_t(s.jitter_seed)*1000003u;
 // CPU optical setup is deterministic. Avoid regenerating 16K polygon-clip
 // and Halton samples on each frame when only the source intensity/position
 // or composition footage changes. Share immutable samples under a bounded
 // MFR-safe LRU; no CUDA pointer survives across AE render calls.
 using Key=std::array<uint32_t,5>;
 using Samples=std::vector<PupilSample>;
 const uint32_t method=uint32_t(std::clamp(s.pupil_jitter,0,3));
 const Key key={uint32_t(n),uint32_t(blades),
                blades>=3?std::bit_cast<uint32_t>(rotate):0u,
                method,method==1?seed:0u};
 using Entry=std::pair<Key,std::shared_ptr<const Samples>>;
 static std::mutex cache_mutex;
 static std::deque<Entry> cache;
 std::shared_ptr<const Samples> existing;
 {
  std::lock_guard<std::mutex> lock(cache_mutex);
  for(const auto& entry:cache)if(entry.first==key){
   existing=entry.second;break;
  }
 }
 if(existing)return existing;
 std::vector<PupilSample> result;
 result.reserve(n*n);
 for(int k=0;k<n*n;k++){
  const int x=k%n,y=k/n;
  float u,v;
  if(s.pupil_jitter==3){
   // Equal-area Vogel spiral. Every point is inside the entrance pupil,
   // eliminating square-sampling rejection and reducing low-grid spokes.
   const float radius=std::sqrt((float(k)+.5f)/float(n*n));
   const float a=float(k)*2.39996322972865332f;
   u=radius*std::cos(a);v=radius*std::sin(a);
  }else if(s.pupil_jitter==2){
   u=halton2(uint32_t(k))*2.f-1.f;
   v=halton3(uint32_t(k))*2.f-1.f;
  }else{
   const float jx=s.pupil_jitter==1?float(wang(uint32_t(k)+seed))*(1.f/4294967296.f):.5f;
   const float jy=s.pupil_jitter==1?float(wang(uint32_t(k+n*n)+seed))*(1.f/4294967296.f):.5f;
   u=(x+jx)*2.f/n-1;v=(y+jy)*2.f/n-1;
  }
  const float r2=u*u+v*v;
  if(r2>1)continue;
  if(blades>=3){
   float t=std::fmod(std::atan2(v,u)-rotate,sector);
   if(t<0)t+=sector;
   if(std::sqrt(r2)*std::cos(t-sector*.5f)>apothem)continue;
  }
  result.push_back({u,v});
 }
 // Large final-quality sets are consumed once per render. Move them
 // directly to the caller without building a second temporary copy.
 if(n>512)return std::make_shared<const Samples>(std::move(result));
 auto created=std::make_shared<const Samples>(std::move(result));
 {
  std::lock_guard<std::mutex> lock(cache_mutex);
  for(const auto& entry:cache)if(entry.first==key)return entry.second;
  if(cache.size()>=24)cache.pop_front();
  cache.emplace_back(key,created);
 }
 return created;
}
std::vector<PupilSample> make_pupil(const Settings& s){
 return *cached_pupil(s);
}
std::vector<SpectralSample> make_spectrum(const Settings& s){
 int n=s.spectral_samples;
 if(n!=3&&n!=5&&n!=7&&n!=9&&n!=11)n=3;
 std::vector<SpectralSample> out(n);
 if(n==3){
  out[0]={650,1,0,0};out[1]={550,0,1,0};out[2]={450,0,0,1};
  return out;
 }
 double sum_r=0,sum_g=0,sum_b=0;
 for(int i=0;i<n;i++){
  const float l=400.f+300.f*i/(n-1);
  const float r=(l-600)/70.f,b=(l-450)/30.f,g=(l-545)/55.f,bb=(l-445)/45.f;
  auto gauss=[](float v){return std::exp(-.5f*v*v);};
  out[i]={l,.63f*gauss(r)+.22f*gauss(b),gauss(g),gauss(bb)};
  sum_r+=out[i].r;sum_g+=out[i].g;sum_b+=out[i].b;
 }
 for(auto& x:out){x.r/=float(sum_r);x.g/=float(sum_g);x.b/=float(sum_b);}
 return out;
}
void box_blur_rgb(std::vector<float>& rgb,int w,int h,int radius,int passes){
 if(w<=0||h<=0||radius<=0||passes<=0||rgb.size()!=size_t(w)*h*3)return;
 radius=std::min(radius,std::max(w,h));passes=std::min(passes,6);
 std::vector<float> scratch(rgb.size());
 std::vector<double> sums(size_t(std::max(w,h))+1);
 for(int pass=0;pass<passes;pass++){
  // Horizontal, interleaved RGB, prefix sums.
  for(int y=0;y<h;y++)for(int c=0;c<3;c++){
   sums[0]=0;
   for(int x=0;x<w;x++)sums[x+1]=sums[x]+rgb[(size_t(y)*w+x)*3+c];
   for(int x=0;x<w;x++){
    int lo=std::max(0,x-radius),hi=std::min(w-1,x+radius);
    scratch[(size_t(y)*w+x)*3+c]=float((sums[hi+1]-sums[lo])/(hi-lo+1));
   }
  }
  // Vertical; no repeated full-frame allocations.
  for(int x=0;x<w;x++)for(int c=0;c<3;c++){
   sums[0]=0;
   for(int y=0;y<h;y++)sums[y+1]=sums[y]+scratch[(size_t(y)*w+x)*3+c];
   for(int y=0;y<h;y++){
    const int lo=std::max(0,y-radius),hi=std::min(h-1,y+radius);
    rgb[(size_t(y)*w+x)*3+c]=float((sums[hi+1]-sums[lo])/(hi-lo+1));
   }
  }
 }
}
void render_haze(std::vector<float>& rgb,const std::vector<Source>& sources,int w,int h,
                 float gain,float radius,int passes,int source_footprint){
 if(gain<=0||sources.empty()||w<1||h<1)return;
 rgb.assign(size_t(w)*h*3,0.f);
 const int footprint=std::clamp(source_footprint,1,32);
 for(const auto& s:sources){
  const auto rect=nuke_haze_rect(s,w,h,footprint);
  for(int y=rect.y0;y<rect.y1;y++)
   for(int x=rect.x0;x<rect.x1;x++){
    const size_t idx=(size_t(y)*w+x)*3;
    rgb[idx]+=s.r;rgb[idx+1]+=s.g;rgb[idx+2]+=s.b;
   }
 }
 const int rad=int(std::round(std::clamp(radius,0.f,1.f)*std::hypot(float(w),float(h))));
 box_blur_rgb(rgb,w,h,rad,std::max(1,passes));
 for(float& v:rgb)v*=gain;
}
void render_diffraction(std::vector<float>& rgb,const std::vector<Source>& sources,
                        int w,int h,int blades,float rotation,float gain,float scale){
 if(gain<=0||sources.empty()||w<1||h<1)return;
 StarburstConfig config;
 config.gain=gain;config.scale=std::clamp(scale,.001f,1.f);
 config.aperture_blades=std::clamp(blades,0,16);
 config.aperture_rotation_deg=rotation;
 // Immutable bounded PSF cache: 8 configurations at 512^2 float samples (~8 MiB).
 // Shader/render threads never mutate the published PSFs.
 struct Key {int blades;uint32_t angle;};
 static std::mutex mutex;
 static std::deque<std::pair<Key,std::shared_ptr<const StarburstPSF>>> cached;
 const Key key{config.aperture_blades,std::bit_cast<uint32_t>(config.aperture_rotation_deg)};
 std::shared_ptr<const StarburstPSF> psf;
 {
  std::lock_guard<std::mutex> lock(mutex);
  for(auto it=cached.begin();it!=cached.end();++it){
   if(it->first.blades==key.blades&&it->first.angle==key.angle){
    psf=it->second;break;
   }
  }
 }
 if(!psf){
  auto created=std::make_shared<StarburstPSF>();
  compute_starburst_psf(config,*created,512);
  std::lock_guard<std::mutex> lock(mutex);
  for(const auto& entry:cached){
   if(entry.first.blades==key.blades&&entry.first.angle==key.angle){
    psf=entry.second;break;
   }
  }
  if(!psf){
   psf=created;
   if(cached.size()>=8)cached.pop_front();
   cached.emplace_back(key,psf);
  }
 }
 std::vector<BrightPixel> bp;
 bp.reserve(sources.size());
 for(const auto& s:sources){
  bp.push_back({std::atan((s.x*2.f-1.f)),std::atan((s.y*2.f-1.f)*h/float(w)),s.r,s.g,s.b});
 }
 const size_t n=size_t(w)*h;
 std::vector<float> r(n,0),g(n,0),b(n,0);
 render_starburst(*psf,config,bp,1.f,float(h)/w,
                  r.data(),g.data(),b.data(),w,h,w,h,0,0);
 rgb.assign(n*3,0.f);
 for(size_t i=0;i<n;i++){rgb[i*3]=r[i];rgb[i*3+1]=g[i];rgb[i*3+2]=b[i];}
}
} // namespace fl
