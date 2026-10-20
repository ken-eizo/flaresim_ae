#include "FlareLensOptics.hpp"
#include "FlareLensThreadPool.hpp"
#include "starburst.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
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
 // CPU optical setup is deterministic. Avoid regenerating polygon-clip,
 // Halton and Vogel samples on each frame when only the source intensity,
 // position or footage changes. Share immutable samples under a bounded
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
 // Samples are independent: evaluate all candidates on the worker pool,
 // then keep the accepted ones in index order (same list as a serial loop).
 const size_t total=size_t(n)*n;
 std::vector<PupilSample> candidate(total);
 std::vector<uint8_t> keep(total,0);
 constexpr size_t kBlock=size_t(1)<<14;
 parallel_for(int((total+kBlock-1)/kBlock),[&](int block){
  const size_t begin=size_t(block)*kBlock,end=std::min(total,begin+kBlock);
  for(size_t i=begin;i<end;i++){
   const int k=int(i);
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
   candidate[i]={u,v};keep[i]=1;
  }
 });
 std::vector<PupilSample> result;
 result.reserve(total);
 for(size_t i=0;i<total;i++)if(keep[i])result.push_back(candidate[i]);
 result.shrink_to_fit();
 auto created=std::make_shared<const Samples>(std::move(result));
 {
  // Up to 24 configurations and 256 MiB: a 2048 grid alone is ~32 MiB.
  constexpr size_t kBytes=size_t(256)<<20;
  std::lock_guard<std::mutex> lock(cache_mutex);
  for(const auto& entry:cache)if(entry.first==key)return entry.second;
  cache.emplace_back(key,created);
  size_t bytes=0;
  for(const auto& entry:cache)bytes+=entry.second->size()*sizeof(PupilSample);
  while(cache.size()>1&&(cache.size()>24||bytes>kBytes)){
   bytes-=cache.front().second->size()*sizeof(PupilSample);
   cache.pop_front();
  }
 }
 return created;
}
namespace {
uint32_t spread_bits(uint32_t x){
 x&=0x0000ffffu;
 x=(x|(x<<8))&0x00ff00ffu;
 x=(x|(x<<4))&0x0f0f0f0fu;
 x=(x|(x<<2))&0x33333333u;
 x=(x|(x<<1))&0x55555555u;
 return x;
}
} // namespace
std::shared_ptr<const std::vector<PupilSample>> coherent_pupil(
  const std::shared_ptr<const std::vector<PupilSample>>& pupil){
 using Samples=std::vector<PupilSample>;
 using Entry=std::pair<std::shared_ptr<const Samples>,std::shared_ptr<const Samples>>;
 static std::mutex mutex;
 static std::deque<Entry> cache;
 if(!pupil)return pupil;
 {
  std::lock_guard<std::mutex> lock(mutex);
  for(const auto& entry:cache)if(entry.first==pupil)return entry.second;
 }
 // Stable two-pass radix sort on a 10+10 bit Morton code: ties keep the
 // original sample order, so the result is deterministic.
 const auto& source=*pupil;
 const size_t n=source.size();
 std::vector<uint32_t> code(n),order(n),next(n);
 for(size_t i=0;i<n;i++){
  const auto cell=[](float t){
   return uint32_t(std::clamp((t+1.f)*512.f,0.f,1023.f));
  };
  code[i]=spread_bits(cell(source[i].u))|(spread_bits(cell(source[i].v))<<1);
  order[i]=uint32_t(i);
 }
 for(int pass=0;pass<2;pass++){
  const int shift=pass*10;
  std::vector<uint32_t> count(1025,0);
  for(size_t i=0;i<n;i++)count[((code[order[i]]>>shift)&1023u)+1]++;
  for(int b=0;b<1024;b++)count[b+1]+=count[b];
  for(size_t i=0;i<n;i++)next[count[(code[order[i]]>>shift)&1023u]++]=order[i];
  order.swap(next);
 }
 auto ordered=std::make_shared<Samples>();
 ordered->reserve(n);
 for(const uint32_t i:order)ordered->push_back(source[i]);
 std::shared_ptr<const Samples> result=std::move(ordered);
 std::lock_guard<std::mutex> lock(mutex);
 for(const auto& entry:cache)if(entry.first==pupil)return entry.second;
 cache.emplace_back(pupil,result);
 size_t bytes=0;
 for(const auto& entry:cache)bytes+=entry.second->size()*sizeof(PupilSample);
 while(cache.size()>1&&(cache.size()>8||bytes>(size_t(256)<<20))){
  bytes-=cache.front().second->size()*sizeof(PupilSample);
  cache.pop_front();
 }
 return result;
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
void box_blur_rgb(std::vector<float>& rgb,int w,int h,int radius,int passes,
                  float fraction){
 if(!(fraction>0.f&&fraction<1.f))fraction=0.f;
 if(w<=0||h<=0||radius<0||(radius==0&&fraction==0.f)||passes<=0||
    rgb.size()!=size_t(w)*h*3)return;
 radius=std::min(radius,std::max(w,h));passes=std::min(passes,6);
 const double tail=fraction;
 // Row and column box filters commute, so all horizontal passes run first,
 // each row kept in cache across passes, then all vertical passes per column
 // block: two full-frame sweeps instead of two per pass. Every pass keeps the
 // serial double prefix sums and divisions; only the H/V order differs, which
 // changes results by float rounding (~1e-7 relative) at most.
 std::vector<float> scratch=acquire_buffer(rgb.size());
 float* const data=rgb.data();
 float* const tmp=scratch.data();
 // 1/n for every window size; a double product differs from the double
 // quotient by far less than the float rounding applied right after.
 std::vector<double> inverse(size_t(2)*radius+2,0.);
 for(size_t n=1;n<inverse.size();n++)inverse[n]=1./double(n);
 parallel_for(h,[&](int y){
  thread_local std::vector<double> sums;
  thread_local std::vector<float> ping,pong;
  sums.resize((size_t(w)+1)*3);
  ping.assign(data+size_t(y)*w*3,data+size_t(y+1)*w*3);
  pong.resize(size_t(w)*3);
  for(int pass=0;pass<passes;pass++){
   // Three interleaved running sums, one per channel.
   sums[0]=sums[1]=sums[2]=0;
   for(size_t i=0;i<size_t(w)*3;i++)sums[i+3]=sums[i]+ping[i];
   for(int x=0;x<w;x++){
    const int lo=std::max(0,x-radius),hi=std::min(w-1,x+radius);
    const double* a=sums.data()+size_t(lo)*3;
    const double* b=sums.data()+size_t(hi+1)*3;
    float* out=pong.data()+size_t(x)*3;
    if(tail==0.){
     const double k=inverse[size_t(hi-lo+1)];
     out[0]=float((b[0]-a[0])*k);out[1]=float((b[1]-a[1])*k);out[2]=float((b[2]-a[2])*k);
     continue;
    }
    double s[3]={b[0]-a[0],b[1]-a[1],b[2]-a[2]},weight=double(hi-lo+1);
    for(const int t:{x-radius-1,x+radius+1}){
     if(t<0||t>=w)continue;
     for(int c=0;c<3;c++)s[c]+=tail*ping[size_t(t)*3+c];
     weight+=tail;
    }
    for(int c=0;c<3;c++)out[c]=float(s[c]/weight);
   }
   ping.swap(pong);
  }
  std::copy(ping.begin(),ping.end(),tmp+size_t(y)*w*3);
 });
 constexpr int kColumns=16; // 16 pixels = 48 contiguous floats per row
 const int column_blocks=(w+kColumns-1)/kColumns;
 parallel_for(column_blocks,[&](int block){
  const int x0=block*kColumns,lanes=std::min(kColumns,w-x0)*3;
  thread_local std::vector<double> sums;
  thread_local std::vector<float> ping,pong;
  sums.resize((size_t(h)+1)*lanes);
  ping.resize(size_t(h)*lanes);pong.resize(size_t(h)*lanes);
  for(int y=0;y<h;y++){
   const float* in=tmp+(size_t(y)*w+x0)*3;
   std::copy(in,in+lanes,ping.data()+size_t(y)*lanes);
  }
  for(int pass=0;pass<passes;pass++){
   for(int j=0;j<lanes;j++)sums[j]=0;
   for(int y=0;y<h;y++){
    const float* in=ping.data()+size_t(y)*lanes;
    const double* prev=sums.data()+size_t(y)*lanes;
    double* next=sums.data()+size_t(y+1)*lanes;
    for(int j=0;j<lanes;j++)next[j]=prev[j]+in[j];
   }
   for(int y=0;y<h;y++){
    const int lo=std::max(0,y-radius),hi=std::min(h-1,y+radius);
    const double* lo_sum=sums.data()+size_t(lo)*lanes;
    const double* hi_sum=sums.data()+size_t(hi+1)*lanes;
    float* out=pong.data()+size_t(y)*lanes;
    if(tail==0.){
     const double k=inverse[size_t(hi-lo+1)];
     for(int j=0;j<lanes;j++)out[j]=float((hi_sum[j]-lo_sum[j])*k);
     continue;
    }
    const float* up=y-radius-1>=0?ping.data()+size_t(y-radius-1)*lanes:nullptr;
    const float* down=y+radius+1<h?ping.data()+size_t(y+radius+1)*lanes:nullptr;
    const double weight=double(hi-lo+1)+(up?tail:0.)+(down?tail:0.);
    for(int j=0;j<lanes;j++){
     double s=hi_sum[j]-lo_sum[j];
     if(up)s+=tail*up[j];
     if(down)s+=tail*down[j];
     out[j]=float(s/weight);
    }
   }
   ping.swap(pong);
  }
  for(int y=0;y<h;y++){
   const float* in=ping.data()+size_t(y)*lanes;
   std::copy(in,in+lanes,data+(size_t(y)*w+x0)*3);
  }
 });
 release_buffer(std::move(scratch));
}
std::vector<float> box_downscale_rgb(const std::vector<float>& rgb,int w,int h,int fx,int fy){
 if(fx<1||fy<1||w<fx||h<fy||rgb.size()!=size_t(w)*h*3)return {};
 const int sw=w/fx,sh=h/fy;
 std::vector<float> out=acquire_buffer(size_t(sw)*sh*3);
 const float norm=1.f/float(fx*fy);
 const float* src=rgb.data();
 float* dst=out.data();
 parallel_for(sh,[&](int y){
  float* row=dst+size_t(y)*sw*3;
  for(int i=0;i<sw*3;i++)row[i]=0.f;
  for(int yy=0;yy<fy;yy++){
   const float* in=src+size_t(y*fy+yy)*w*3;
   for(int x=0;x<sw;x++){
    const float* p=in+size_t(x*fx)*3;
    float r=0.f,g=0.f,b=0.f;
    for(int xx=0;xx<fx;xx++){r+=p[xx*3];g+=p[xx*3+1];b+=p[xx*3+2];}
    row[x*3]+=r;row[x*3+1]+=g;row[x*3+2]+=b;
   }
  }
  for(int i=0;i<sw*3;i++)row[i]*=norm;
 });
 return out;
}
void render_haze(std::vector<float>& rgb,const std::vector<Source>& sources,int w,int h,
                 float gain,float radius,int passes,int source_footprint,
                 float resolution_scale){
 if(gain<=0||sources.empty()||w<1||h<1){rgb.clear();return;}
 if(rgb.size()!=size_t(w)*h*3)rgb.assign(size_t(w)*h*3,0.f);
 else parallel_zero(rgb);
 const int footprint=std::clamp(source_footprint,1,32);
 for(const auto& s:sources){
  const int cx=int(s.x*w),cy=int(s.y*h);
  for(int y=std::max(0,cy-footprint/2);y<std::min(h,cy+(footprint+1)/2);y++)
   for(int x=std::max(0,cx-footprint/2);x<std::min(w,cx+(footprint+1)/2);x++){
    const size_t idx=(size_t(y)*w+x)*3;
    rgb[idx]+=s.r;rgb[idx+1]+=s.g;rgb[idx+2]+=s.b;
   }
 }
 const BlurRadius rad=frame_blur_radius(std::clamp(radius,0.f,1.f),w,h,resolution_scale,false);
 box_blur_rgb(rgb,w,h,rad.whole,std::max(1,passes),rad.fraction);
 float* const data=rgb.data();
 parallel_for(h,[&](int y){
  float* row=data+size_t(y)*w*3;
  for(int i=0;i<w*3;i++)row[i]*=gain;
 });
}
namespace {
// upstream starburst.cpp fft1d(), unchanged: in-place radix-2, forward.
void psf_fft1d(std::complex<float>* data,int n){
 for(int i=1,j=0;i<n;++i){
  int bit=n>>1;
  for(;j&bit;bit>>=1)j^=bit;
  j^=bit;
  if(i<j)std::swap(data[i],data[j]);
 }
 for(int len=2;len<=n;len<<=1){
  float ang=-2.0f*(float)M_PI/len;
  std::complex<float> wlen(std::cos(ang),std::sin(ang));
  for(int i=0;i<n;i+=len){
   std::complex<float> w(1.0f,0.0f);
   for(int j=0;j<len/2;++j){
    std::complex<float> u=data[i+j];
    std::complex<float> v=data[i+j+len/2]*w;
    data[i+j]=u+v;
    data[i+j+len/2]=u-v;
    w*=wlen;
   }
  }
 }
}
// upstream compute_starburst_psf() with every row, column and pixel loop on
// the worker pool. Each value goes through the same operations in the same
// order, so the PSF is bit-identical; only the wall time changes (a new
// aperture rotation recomputed this 512^2 FFT on one thread every frame).
void compute_psf_parallel(const StarburstConfig& cfg,StarburstPSF& psf_out,int fft_size){
 const int n=fft_size;
 psf_out.N=n;
 psf_out.data.resize((size_t)n*n);
 const float rot_rad=cfg.aperture_rotation_deg*(float)M_PI/180.0f;
 const bool polygonal=(cfg.aperture_blades>=3);
 const float apothem=polygonal?std::cos((float)M_PI/cfg.aperture_blades):1.0f;
 const float sector_ang=polygonal?(2.0f*(float)M_PI/cfg.aperture_blades):1.0f;
 std::vector<std::complex<float>> grid((size_t)n*n);
 parallel_for(n,[&](int r){
  for(int c=0;c<n;++c){
   float u=((c+0.5f)/n)*2.0f-1.0f;
   float v=((r+0.5f)/n)*2.0f-1.0f;
   float r2=u*u+v*v;
   float value=0.0f;
   if(r2<=1.0f){
    value=1.0f;
    if(polygonal){
     float angle=std::atan2(v,u)-rot_rad;
     float sector=std::fmod(angle,sector_ang);
     if(sector<0.0f)sector+=sector_ang;
     if(std::sqrt(r2)*std::cos(sector-sector_ang*0.5f)>apothem)value=0.0f;
    }
   }
   grid[(size_t)r*n+c]={value,0.0f};
  }
  psf_fft1d(grid.data()+(size_t)r*n,n);
 });
 parallel_for(n,[&](int c){
  std::vector<std::complex<float>> col(n);
  for(int r=0;r<n;++r)col[r]=grid[(size_t)r*n+c];
  psf_fft1d(col.data(),n);
  for(int r=0;r<n;++r)grid[(size_t)r*n+c]=col[r];
 });
 std::vector<float> row_peak(n,0.0f);
 parallel_for(n,[&](int r){
  const int rs=(r+n/2)%n;
  float peak=0.0f;
  for(int c=0;c<n;++c){
   const auto& z=grid[(size_t)rs*n+(c+n/2)%n];
   float val=z.real()*z.real()+z.imag()*z.imag();
   psf_out.data[(size_t)r*n+c]=val;
   if(val>peak)peak=val;
  }
  row_peak[r]=peak;
 });
 float peak=0.0f;
 for(const float value:row_peak)if(value>peak)peak=value;
 if(peak>0.0f){
  const float inv=1.0f/peak;
  parallel_for(n,[&](int r){
   for(int c=0;c<n;++c)psf_out.data[(size_t)r*n+c]*=inv;
  });
 }
}
} // namespace
std::shared_ptr<const StarburstPSF> cached_starburst_psf(int blades,float rotation){
 StarburstConfig config;
 config.aperture_blades=std::clamp(blades,0,16);
 config.aperture_rotation_deg=rotation;
 // Immutable bounded PSF cache: 8 configurations at 512^2 float samples (~8 MiB).
 // Shader/render threads never mutate the published PSFs.
 struct Key {int blades;uint32_t angle;};
 static std::mutex mutex;
 static std::deque<std::pair<Key,std::shared_ptr<const StarburstPSF>>> cached;
 const Key key{config.aperture_blades,std::bit_cast<uint32_t>(config.aperture_rotation_deg)};
 {
  std::lock_guard<std::mutex> lock(mutex);
  for(const auto& entry:cached){
   if(entry.first.blades==key.blades&&entry.first.angle==key.angle)return entry.second;
  }
 }
 auto created=std::make_shared<StarburstPSF>();
 compute_psf_parallel(config,*created,512);
 std::lock_guard<std::mutex> lock(mutex);
 for(const auto& entry:cached){
  if(entry.first.blades==key.blades&&entry.first.angle==key.angle)return entry.second;
 }
 if(cached.size()>=8)cached.pop_front();
 cached.emplace_back(key,created);
 return created;
}
// Geometry of upstream render_starburst() for render_diffraction()'s call
// (tan_half_h=1, tan_half_v=h/w, format == buffer), shared by CPU and CUDA.
bool starburst_setup(const std::vector<Source>& sources,int w,int h,int blades,
                     float rotation,float gain,float scale,StarburstSetup& out){
 out=StarburstSetup{};
 if(gain<=0||sources.empty()||w<1||h<1)return false;
 StarburstConfig cfg;
 cfg.gain=gain;cfg.scale=std::clamp(scale,.001f,1.f);
 out.psf=cached_starburst_psf(blades,rotation);
 if(!out.psf||out.psf->empty())return false;
 const int buf_w=w,buf_h=h,fmt_w=w,fmt_h=h;
 const float tan_half_h=1.f,tan_half_v=float(h)/w;
 out.N=out.psf->N;
 out.half_N=out.N*0.5f;
 const float diag=std::sqrt((float)buf_w*buf_w+(float)buf_h*buf_h);
 out.r_ref=cfg.scale*diag;
 const float kRefLam=550.0f;
 for(int c=0;c<3;c++)out.scale_ch[c]=cfg.wavelengths[c]/kRefLam;
 out.gain=cfg.gain;
 const float fmt_cx=0+fmt_w*0.5f;
 const float fmt_cy=0+fmt_h*0.5f;
 for(const auto& s:sources){
  const BrightPixel src{std::atan((s.x*2.f-1.f)),std::atan((s.y*2.f-1.f)*h/float(w)),s.r,s.g,s.b};
  StarPlacement star;
  star.px=fmt_cx+(std::tan(src.angle_x)/(2.0f*tan_half_h))*fmt_w;
  star.py=fmt_cy+(std::tan(src.angle_y)/(2.0f*tan_half_v))*fmt_h;
  const float r_max=out.r_ref*out.scale_ch[0];
  star.x0=std::max(0,(int)std::floor(star.px-r_max));
  star.x1=std::min(buf_w,(int)std::ceil(star.px+r_max)+1);
  star.y0=std::max(0,(int)std::floor(star.py-r_max));
  star.y1=std::min(buf_h,(int)std::ceil(star.py+r_max)+1);
  if(star.x0>=star.x1||star.y0>=star.y1)continue;
  star.rgb[0]=src.r;star.rgb[1]=src.g;star.rgb[2]=src.b;
  out.stars.push_back(star);
 }
 return !out.stars.empty();
}
void render_diffraction(std::vector<float>& rgb,const std::vector<Source>& sources,
                        int w,int h,int blades,float rotation,float gain,float scale,
                        int supersample){
 StarburstSetup setup;
 if(!starburst_setup(sources,w,h,blades,rotation,gain,scale,setup)){
  // Upstream renders nothing visible; keep the (all-zero) image when the
  // effect is enabled so callers still composite a defined buffer.
  if(gain<=0||sources.empty()||w<1||h<1){rgb.clear();return;}
  if(rgb.size()!=size_t(w)*h*3)rgb.assign(size_t(w)*h*3,0.f);
  else parallel_zero(rgb);
  return;
 }
 if(rgb.size()!=size_t(w)*h*3)rgb.assign(size_t(w)*h*3,0.f);
 else parallel_zero(rgb);
 // upstream render_starburst() arithmetic. Output rows are split across the
 // pool and every row visits the sources in their original order, so each
 // pixel sees the same additions in the same order as the serial upstream
 // loop, with one dispatch for any number of sources.
 const int N=setup.N;
 const float half_N=setup.half_N;
 const float* p=setup.psf->data.data();
 float* const out=rgb.data();
 if(supersample>1){
  // Average the full-resolution pixels this pixel covers: sub-pixel
  // positions k/supersample, as integer pixel positions are at Full.
  const int sub=std::min(supersample,32);
  const float step=1.f/float(sub),norm=1.f/float(sub*sub);
  const auto psf_at=[&](float dj_f,float di_f){
   const int dj=(int)dj_f,di=(int)di_f;
   const float fj=dj_f-dj,fi=di_f-di;
   return p[(size_t)dj*N+di]*(1.0f-fi)*(1.0f-fj)
         +p[(size_t)dj*N+di+1]*fi*(1.0f-fj)
         +p[(size_t)(dj+1)*N+di]*(1.0f-fi)*fj
         +p[(size_t)(dj+1)*N+di+1]*fi*fj;
  };
  parallel_for(h,[&](int oy){
   for(const StarPlacement& star:setup.stars){
    if(oy<star.y0||oy>=star.y1)continue;
    for(int ch=0;ch<3;++ch){
     const float inv_r=half_N/(setup.r_ref*setup.scale_ch[ch]);
     const float src_v=star.rgb[ch]*setup.gain*norm;
     float* o=out+size_t(oy)*w*3+ch;
     for(int sy=0;sy<sub;sy++){
      const float dj_f=(float(oy)+float(sy)*step-star.py)*inv_r+half_N;
      if(dj_f<0.0f||dj_f>=(float)(N-1))continue;
      for(int ox=star.x0;ox<star.x1;++ox){
       float sum=0.f;
       for(int sx=0;sx<sub;sx++){
        const float di_f=(float(ox)+float(sx)*step-star.px)*inv_r+half_N;
        if(di_f<0.0f||di_f>=(float)(N-1))continue;
        sum+=psf_at(dj_f,di_f);
       }
       o[size_t(ox)*3]+=src_v*sum;
      }
     }
    }
   }
  });
  return;
 }
 parallel_for(h,[&](int oy){
  for(const StarPlacement& star:setup.stars){
   if(oy<star.y0||oy>=star.y1)continue;
   for(int ch=0;ch<3;++ch){
    const float inv_r=half_N/(setup.r_ref*setup.scale_ch[ch]);
    const float src_v=star.rgb[ch]*setup.gain;
    const float dj_f=(oy-star.py)*inv_r+half_N;
    if(dj_f<0.0f||dj_f>=(float)(N-1))continue;
    const int dj=(int)dj_f;
    const float fj=dj_f-dj;
    float* o=out+size_t(oy)*w*3+ch;
    for(int ox=star.x0;ox<star.x1;++ox){
     const float di_f=(ox-star.px)*inv_r+half_N;
     if(di_f<0.0f||di_f>=(float)(N-1))continue;
     const int di=(int)di_f;
     const float fi=di_f-di;
     float val=p[(size_t)dj*N+di]*(1.0f-fi)*(1.0f-fj)
              +p[(size_t)dj*N+di+1]*fi*(1.0f-fj)
              +p[(size_t)(dj+1)*N+di]*(1.0f-fi)*fj
              +p[(size_t)(dj+1)*N+di+1]*fi*fj;
     o[size_t(ox)*3]+=src_v*val;
    }
   }
  }
 });
}
} // namespace fl
