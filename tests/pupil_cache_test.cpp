#include "FlareLensOptics.hpp"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
#include <array>

static bool same(const std::vector<fl::PupilSample>& x,
                 const std::vector<fl::PupilSample>& y){
 if(x.size()!=y.size())return false;
 for(size_t i=0;i<x.size();i++){
  if(std::memcmp(&x[i],&y[i],sizeof(fl::PupilSample))!=0)return false;
 }
 return true;
}
int main(){
 fl::Settings s{};
 s.grid=128;s.aperture_blades=6;s.aperture_rotation_deg=11.25f;
 s.pupil_jitter=2;s.jitter_seed=123;
 const auto original=fl::make_pupil(s);
 if(original.size()<10000)return 1;
 for(int t=0;t<10;t++){
  const auto cached=fl::make_pupil(s);
  if(!same(original,cached))return 2;
 }
 // Halton geometry cannot depend on the seed.
 s.jitter_seed=998;
 if(!same(original,fl::make_pupil(s)))return 3;
 s.pupil_jitter=1;s.jitter_seed=1;
 const auto stratified=fl::make_pupil(s);
 s.jitter_seed=2;
 const auto changed=fl::make_pupil(s);
 if(same(stratified,changed))return 4;
 // Ultra-final 512^2 pupil sampling must stay deterministic and cacheable,
 // without overflow of k/n or altering the Nuke normalization.
 s.grid=512;s.pupil_jitter=2;s.aperture_blades=6;
 const auto ultra=fl::make_pupil(s);
 if(ultra.size()<150000||ultra.size()>size_t(512)*512)return 6;
 if(!same(ultra,fl::make_pupil(s)))return 7;
 // High-density disk sampling is deterministic, equal-area and keeps
 // every sample instead of discarding ~21.5% outside a square's circle.
 s.grid=768;s.pupil_jitter=3;s.aperture_blades=0;
 const auto golden=fl::make_pupil(s);
 if(golden.size()!=size_t(768)*768)return 8;
 for(const auto& p:golden){
  if(!std::isfinite(p.u)||!std::isfinite(p.v)||
     p.u*p.u+p.v*p.v>1.00001f)return 9;
 }
 if(!same(golden,fl::make_pupil(s)))return 10;
 std::atomic<int> failures{0};
 constexpr int threads=8;
 std::array<std::thread,threads> jobs;
 for(int t=0;t<threads;t++)jobs[t]=std::thread([&,t]{
  auto local=s;
  for(int i=0;i<25;i++){
   local.pupil_jitter=(i%3);
   local.jitter_seed=(i%5);
   local.grid=(i%2)?64:128;
   local.aperture_rotation_deg=float((t%2)?11.25:0);
   const auto first=fl::make_pupil(local);
   const auto second=fl::make_pupil(local);
   if(first.empty()||!same(first,second))failures.fetch_add(1);
  }
 });
 for(auto& t:jobs)t.join();
 std::cout<<"AE_MFR_PUPIL_CACHE threads="<<threads
          <<" seed_variants=5 baseline_samples="<<original.size()
          <<" failures="<<failures.load()<<"\n";
 return failures.load()?5:0;
}
