#include "FlareLensOptics.hpp"
#include "../upstream/src/fresnel.h"
#include <complex>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
int main(){
 for(int n:{3,5,7,9,11}){
  fl::Settings s;s.spectral_samples=n;
  const auto spec=fl::make_spectrum(s);
  if(int(spec.size())!=n)return 1;
  float r=0,g=0,b=0;
  for(const auto& x:spec){
   if(!(x.lambda>=400&&x.lambda<=700)&&n!=3)return 2;
   r+=x.r;g+=x.g;b+=x.b;
  }
  if(std::fabs(r-1)>1e-5||std::fabs(g-1)>1e-5||std::fabs(b-1)>1e-5)return 3;
 }
 fl::Settings s;s.grid=32;s.aperture_blades=5;s.aperture_rotation_deg=17;
 for(int mode:{0,1,2}){
  s.pupil_jitter=mode;s.jitter_seed=321;
  const auto a=fl::make_pupil(s),b=fl::make_pupil(s);
  if(a.empty()||a.size()>1024||a.size()!=b.size())return 4;
  for(size_t i=0;i<a.size();i++){
   if(a[i].u!=b[i].u||a[i].v!=b[i].v)return 5;
   if(a[i].u*a[i].u+a[i].v*a[i].v>1.000001f)return 6;
  }
 }
 // Independent complex-amplitude thin-film transfer-function check.
 // s and p amplitudes diverge at oblique incidence. Normal incidence
 // alone cannot detect an incorrectly polarized-only model.
 const float film_n=1.38f,glass_n=1.517f,film_d=550.f/(4.f*film_n);
 for(const float angle:{0.f,35.f,55.f,75.f}){
  for(const float wavelength:{450.f,550.f,650.f}){
   const float ci=std::cos(angle*3.14159265358979323846f/180.f);
   const float si2=1.f-ci*ci;
   const float cc=std::sqrt(1.f-si2/(film_n*film_n));
   const float cs=std::sqrt(1.f-si2/(glass_n*glass_n));
   const auto rs01=(ci-film_n*cc)/(ci+film_n*cc);
   const auto rs12=(film_n*cc-glass_n*cs)/(film_n*cc+glass_n*cs);
   const auto rp01=(film_n*ci-cc)/(film_n*ci+cc);
   const auto rp12=(glass_n*cc-film_n*cs)/(glass_n*cc+film_n*cs);
   const auto phase=std::polar(1.f,
     4.f*3.14159265358979323846f*film_n*film_d*cc/wavelength);
   const auto intensity=[&](float a,float b){
    return std::norm((a+b*phase)/(1.f+a*b*phase));
   };
   const float reference=.5f*(intensity(rs01,rs12)+intensity(rp01,rp12));
   const float actual=coating_reflectance(ci,1.f,glass_n,film_n,film_d,wavelength);
   if(!std::isfinite(actual)||std::fabs(actual-reference)>3e-6f){
    std::cerr<<"thin film s/p parity mismatch angle="<<angle
             <<" wavelength="<<wavelength<<" actual="<<actual
             <<" expected="<<reference<<"\\n";return 15;
   }
  }
 }
 const int w=96,h=64;
 const std::vector<fl::Source> lights{{.49f,.5f,1,1,1}};
 std::vector<float> haze;
 fl::render_haze(haze,lights,w,h,12,.15f,3);
 if(haze.size()!=size_t(w)*h*3)return 7;
 float sum=0;
 for(auto x:haze){if(!std::isfinite(x)||x<0)return 8;sum+=x;}
 if(!(sum>0))return 9;
 std::vector<float> star;
 fl::render_diffraction(star,lights,w,h,6,18,300,.2f);
 if(star.size()!=size_t(w)*h*3)return 10;
 float ss=0;
 for(auto x:star){if(!std::isfinite(x)||x<0)return 11;ss+=x;}
 if(!(ss>0))return 12;
 std::vector<float> blur(size_t(w)*h*3,0);
 blur[(size_t(h/2)*w+w/2)*3]=1;
 fl::box_blur_rgb(blur,w,h,4,3);
 double bs=0;for(float v:blur){if(v<0||!std::isfinite(v))return 13;bs+=v;}
 if(std::abs(bs-1)>1e-5)return 14;
 std::cout<<"spectral/pupil/Haze/Starburst/blur deterministic numerical checks passed\n";
 return 0;
}
