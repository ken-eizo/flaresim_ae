#include "FlareLensCpuParallel.hpp"
#include <filesystem>
#include <iostream>
#include <cmath>
#include <atomic>
int main(){
 const auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/
  "upstream/lenses/lens_files/AI_Nikkor_50mm_F1.2S.lens";
 LensSystem lens;
 if(!lens.load(path.string().c_str()))return 1;
 fl::Settings settings;
 settings.grid=10;settings.max_pairs=12;settings.gain=1000;
 settings.spectral_samples=3;settings.pupil_jitter=2;
 std::vector<fl::Source> sources;
 for(int i=0;i<12;i++)sources.push_back({
  (float(i%4)+.5f)/4.f,(float(i/4)+.5f)/3.f,25.f,30.f,35.f});
 const auto old=fl::render_cpu(lens,sources,settings,160,90);
 const auto fast=fl::render_sources_parallel(lens,sources,settings,160,90,{},3);
 if(old.rgb.size()!=fast.rgb.size()||old.rgb.empty())return 2;
 double error=0,reference=0;
 for(size_t i=0;i<old.rgb.size();i++){
  if(!std::isfinite(fast.rgb[i]))return 3;
  error+=std::abs(double(old.rgb[i])-fast.rgb[i]);
  reference+=std::abs(double(old.rgb[i]));
 }
 std::cout<<"PARALLEL_CPU_NUKE_SOURCE_EQUIVALENCE_L1="
  <<error/std::max(reference,1e-10)
  <<" sources="<<sources.size()<<"\n";
 if(error/std::max(reference,1e-10)>1e-4)return 4;
 std::atomic<int> cancel{42};
 const auto should_abort=[](void* ptr)->int{
  return static_cast<std::atomic<int>*>(ptr)->load();
 };
 const auto interrupted=fl::render_sources_parallel(
  lens,sources,settings,160,90,{should_abort,&cancel},3);
 if(interrupted.abort_error!=42)return 5;
 return 0;
}
