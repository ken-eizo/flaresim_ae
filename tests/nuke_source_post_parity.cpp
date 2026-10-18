// Independent original-FlareSim Nuke algorithm oracle for CPU/Mac source
// grouping, pre-cap haze, box blur and Fraunhofer starburst stages.
#include "FlareLensNukeSources.hpp"
#include "FlareLensNukePreview.hpp"
#include "FlareLensOptics.hpp"
#include "starburst.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
static float luma(const BrightPixel& s){
 return .2126f*s.r+.7152f*s.g+.0722f*s.b;
}
static std::vector<BrightPixel> nuke_original(std::vector<BrightPixel> sources,
 int width,float tangent,int radius,int limit){
 if(radius>0&&sources.size()>1){
  const float unit=2.f*tangent/float(width),threshold=radius*unit;
  const float sq=threshold*threshold;
  std::sort(sources.begin(),sources.end(),
    [](const auto& a,const auto& b){return luma(a)>luma(b);});
  std::vector<bool> consumed(sources.size(),false);
  std::vector<BrightPixel> grouped;
  for(size_t i=0;i<sources.size();i++){
   if(consumed[i])continue;
   const auto& a=sources[i];float wt=luma(a);
   float x=a.angle_x*wt,y=a.angle_y*wt;
   float r=a.r,g=a.g,b=a.b;
   for(size_t j=i+1;j<sources.size();j++){
    if(consumed[j])continue;
    const auto& p=sources[j];
    const float dx=a.angle_x-p.angle_x,dy=a.angle_y-p.angle_y;
    if(dx*dx+dy*dy<=sq){
      const float w=luma(p);
      wt+=w;x+=p.angle_x*w;y+=p.angle_y*w;
      r+=p.r;g+=p.g;b+=p.b;consumed[j]=true;
    }
   }
   grouped.push_back({wt>0?x/wt:a.angle_x,wt>0?y/wt:a.angle_y,r,g,b});
  }
  sources.swap(grouped);
 }
 if(limit>0&&sources.size()>size_t(limit)){
  std::partial_sort(sources.begin(),sources.begin()+limit,sources.end(),
   [](const auto&a,const auto&b){return luma(a)>luma(b);});
  sources.resize(limit);
 }
 return sources;
}
static void nuke_blur(float* buf,int w,int h,int radius,int passes){
 if(radius<1||passes<1)return;
 std::vector<float> scratch(size_t(w)*h);
 std::vector<double> prefix(size_t(std::max(w,h))+1);
 for(int pass=0;pass<passes;pass++){
  for(int y=0;y<h;y++){
   prefix[0]=0;
   for(int x=0;x<w;x++)prefix[x+1]=prefix[x]+buf[size_t(y)*w+x];
   for(int x=0;x<w;x++){
    const int lo=std::max(0,x-radius),hi=std::min(w-1,x+radius);
    scratch[size_t(y)*w+x]=float((prefix[hi+1]-prefix[lo])/(hi-lo+1));
   }
  }
  for(int x=0;x<w;x++){
   prefix[0]=0;
   for(int y=0;y<h;y++)prefix[y+1]=prefix[y]+scratch[size_t(y)*w+x];
   for(int y=0;y<h;y++){
    const int lo=std::max(0,y-radius),hi=std::min(h-1,y+radius);
    buf[size_t(y)*w+x]=float((prefix[hi+1]-prefix[lo])/(hi-lo+1));
   }
  }
 }
}
int main(){
 const int w=96,h=64;
 constexpr float pi=3.14159265358979323846f;
 const float fovh=40.f,fovv=24.f;
 const float th=std::tan(fovh*pi/360.f),tv=std::tan(fovv*pi/360.f);
 std::vector<fl::Source> raw{
 {.11f,.21f,33,8,2},{.18f,.20f,17,6,4},
 {.5f,.5f,10,2,1},{.53f,.52f,8,3,1},
 {.8f,.7f,65,22,8},{.83f,.68f,6,5,4},
 {.17f,.8f,20,6,3},{.41f,.35f,32,6,2},
 {.71f,.21f,16,3,3},{.72f,.24f,10,4,2},
 };
 std::vector<BrightPixel> nuke;
 for(const auto& s:raw)nuke.push_back({
  std::atan((s.x*2.f-1.f)*th),std::atan((s.y*2.f-1.f)*tv),
  s.r,s.g,s.b});
 for(int radius:{0,5,13,27})for(int cap:{1,3,5,32}){
  const auto reference=nuke_original(nuke,w,th,radius,cap);
  const auto ae=fl::nuke_select_sources(raw,w,h,fovh,fovv,radius,cap);
  if(reference.size()!=ae.size())return 1;
  double largest=0;
  for(size_t i=0;i<ae.size();i++){
   const auto& s=ae[i];const auto& r=reference[i];
   const auto ax=std::atan((s.x*2.f-1.f)*th);
   const auto ay=std::atan((s.y*2.f-1.f)*tv);
   largest=std::max(largest,double(std::abs(ax-r.angle_x)));
   largest=std::max(largest,double(std::abs(ay-r.angle_y)));
   largest=std::max(largest,double(std::abs(s.r-r.r)));
   largest=std::max(largest,double(std::abs(s.g-r.g)));
   largest=std::max(largest,double(std::abs(s.b-r.b)));
  }
  if(largest>1.e-4){
   std::fprintf(stderr,"nuke cluster mismatch radius=%d cap=%d err=%g\n",
       radius,cap,largest);return 2;
  }
 }
 std::puts("NUKE_SOURCE_CLUSTER_CAP_PARITY_PASSED");
 // Haze is seeded from the ORIGINAL source blocks, before cluster+cap.
 const int step=4,p=3;
 // Haze inputs must represent Nuke's nominal source-block centers.
 std::vector<fl::Source> haze_sources=raw;
 for(size_t i=0;i<haze_sources.size();i++){
  const int bx=int((i*7+3)%(w/step)),by=int((i*5+2)%(h/step));
  haze_sources[i].x=float(bx*step+step/2)/w;
  haze_sources[i].y=float(by*step+step/2)/h;
 }
 std::vector<float> out;
 fl::render_haze(out,haze_sources,w,h,2.75f,.12f,p,step);
 std::vector<float> reference(size_t(w)*h*3,0.f);
 for(const auto& s:haze_sources){
  const int cx=int(s.x*w),cy=int(s.y*h);
  for(int y=std::max(0,cy-step/2);y<std::min(h,cy+(step+1)/2);y++)
   for(int x=std::max(0,cx-step/2);x<std::min(w,cx+(step+1)/2);x++){
    const size_t i=(size_t(y)*w+x)*3;
    reference[i]+=s.r;reference[i+1]+=s.g;reference[i+2]+=s.b;
   }
 }
 const int rad=int(std::round(.12f*std::hypot(float(w),float(h))));
 for(int c=0;c<3;c++){
  std::vector<float> one(size_t(w)*h);
  for(size_t i=0;i<one.size();i++)one[i]=reference[3*i+c];
  nuke_blur(one.data(),w,h,rad,p);
  for(size_t i=0;i<one.size();i++)reference[3*i+c]=one[i]*2.75f;
 }
 double err=0,energy=0;
 for(size_t i=0;i<reference.size();i++){
  err+=std::abs(double(out[i])-reference[i]);
  energy+=std::abs(double(reference[i]));
 }
 if(energy<1||err/energy>1e-6)return 3;
 std::puts("NUKE_HAZE_RAW_BLOCK_BLUR_PARITY_PASSED");
 // Original Nuke's final floor-division tile absorbs ALL remaining pixels.
 // A 127x91 image with step 4 has a 7x7 final block, not 4x4.
 {
  constexpr int rw=127,rh=91;
  std::vector<float> remainder;
  const std::vector<fl::Source> last{{122.f/rw,86.f/rh,5.f,3.f,2.f}};
  fl::render_haze(remainder,last,rw,rh,1.f,0.f,1,4);
  double total=0;
  for(int y=0;y<rh;y++)for(int x=0;x<rw;x++){
   const size_t o=(size_t(y)*rw+x)*3;
   const float expected=x>=120&&y>=84?5.f:0.f;
   if(std::abs(remainder[o]-expected)>1e-6f)return 5;
   total+=remainder[o];
  }
  if(total!=49.*5)return 6;
  std::puts("NUKE_HAZE_REMAINDER_BLOCK_7x7_PASSED");
 }
 // Original Nuke Fraunhofer starburst from same actual aperture, source
 // angles, FOV, and normalized light position, not a guessed RGB sprite.
 StarburstConfig cfg{};
 cfg.gain=14;cfg.scale=.19f;cfg.aperture_blades=6;
 cfg.aperture_rotation_deg=18;
 StarburstPSF psf;
 compute_starburst_psf(cfg,psf);
 std::vector<float> r(size_t(w)*h),g(r.size()),b(r.size());
 render_starburst(psf,cfg,nuke,th,tv,r.data(),g.data(),b.data(),
  w,h,w,h,0,0);
 std::vector<float> ae;
 fl::render_diffraction(ae,raw,w,h,6,18,14,.19f);
 double star_diff=0,star_energy=0;
 for(size_t i=0;i<r.size();i++){
  const float actual[]={ae[3*i],ae[3*i+1],ae[3*i+2]};
  const float expected[]={r[i],g[i],b[i]};
  for(int k=0;k<3;k++){
   star_diff+=std::abs(double(actual[k])-expected[k]);
   star_energy+=std::abs(double(expected[k]));
  }
 }
 std::printf("NUKE_STARBURST_REL_L1=%g\n",star_diff/star_energy);
 if(star_energy<1||star_diff/star_energy>.0001)return 4;
 std::puts("NUKE_STARBURST_PSF_PARITY_PASSED");
 // Reproduce pristine upstream source.rgb's independent angular inversion,
 // scan-step footprint and sequential overpainting for overlapping sources.
 // Source RGB remains HDR; original Nuke does not draw AE crosshairs or
 // blend the image plate beneath this auxiliary channel.
 {
  constexpr int fw=127,fh=91;
  const std::vector<fl::Source> chosen{
   {14.5f/fw,16.5f/fh,2.2f,1.1f,9.5f},
   {14.5f/fw,16.5f/fh,51.f,17.f,8.f}, // Later source overwrites previous
   {125.5f/fw,89.5f/fh,9.f,8.f,7.f},
   {-1.5f/fw,44.5f/fh,3.f,4.f,5.f},  // Off-frame light
   {.5f,.5f,10000.f,3200.f,240.f}
  };
  const float a=std::tan(fovh*pi/360.f);
  const float b=std::tan(fovv*pi/360.f);
  for(int step:{1,3,4,5,8,13}){
   std::vector<BrightPixel> angles;
   for(const auto& s:chosen)
    angles.push_back({std::atan((s.x-.5f)*2.f*a),
                      std::atan((s.y-.5f)*2.f*b),
                      s.r,s.g,s.b});
   std::vector<float> oracle(size_t(fw)*fh*3,0);
   for(const auto& src:angles){
    const int cx=int(std::tan(src.angle_x)/(2.f*a)*fw+fw*.5f);
    const int cy=int(std::tan(src.angle_y)/(2.f*b)*fh+fh*.5f);
    for(int y=std::max(0,cy-step/2);y<std::min(fh,cy-step/2+step);y++)
     for(int x=std::max(0,cx-step/2);x<std::min(fw,cx-step/2+step);x++){
      const auto i=(size_t(y)*fw+x)*3;
      oracle[i]=src.r;oracle[i+1]=src.g;oracle[i+2]=src.b;
     }
   }
   const auto result=fl::nuke_preview_rgb(chosen,fw,fh,step);
   if(result.size()!=oracle.size())return 7;
   double max_error=0;
   for(size_t i=0;i<result.size();i++)
    max_error=std::max(max_error,std::abs(double(result[i])-oracle[i]));
   if(max_error>1e-4){
    std::fprintf(stderr,"Nuke source.rgb mismatch step=%d err=%g\n",
                 step,max_error);return 8;
   }
  }
  std::puts("NUKE_SOURCE_RGB_PREVIEW_BLOCK_PARITY_PASSED");
 }
 return 0;
}
