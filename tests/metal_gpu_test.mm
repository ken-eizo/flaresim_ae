#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "FlareLensMetal.hpp"
#include "FlareLensCore.hpp"
#include "FlareLensOptics.hpp"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>
int main(){
 @autoreleasepool {
  id<MTLDevice> dev=MTLCreateSystemDefaultDevice();
  if(!dev){std::cerr<<"Metal GPU unavailable\n";return 77;}
  auto queue=[dev newCommandQueue];
  if(!queue)return 2;
  LensSystem lens;
  const auto path=std::filesystem::path(FLARELENS_TEST_SOURCE_DIR)/
    "upstream/lenses/lens_files/AI_Nikkor_50mm_F1.2S.lens";
  if(!lens.load(path.string().c_str()))return 3;
  constexpr int w=128,h=72;
  const size_t n=size_t(w)*h,bytes=n*16;
  std::vector<float> bg(n*4);
  for(size_t i=0;i<n;i++){
   bg[i*4]=.04f;bg[i*4+1]=.08f;bg[i*4+2]=.15f;
   bg[i*4+3]=i%3==0?0.f:1.f;
  }
  auto in=[dev newBufferWithBytes:bg.data() length:bytes
      options:MTLResourceStorageModeShared];
  auto out=[dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
  if(!in||!out)return 4;
  const std::vector<fl::Source> sources{{.56f,.44f,1.8f,.8f,.6f}};
  fl::Settings cfg;
  cfg.gain=2000;cfg.max_pairs=16;cfg.grid=12;cfg.spectral_samples=3;
  const size_t scratch_bytes=fl::gpu::metal_scratch_bytes(
    lens,1,cfg.grid,w,h,1,false,false,false);
  auto scratch=[dev newBufferWithLength:scratch_bytes
      options:MTLResourceStorageModePrivate];
  if(!scratch)return 5;
  fl::gpu::View view{(__bridge void*)in,(__bridge void*)out,w,h,
      w*16,w*16};
  view.command_stream=(__bridge void*)queue;
  view.ghost_blur=0.f;
  view.haze_gain=0.f;
  const auto reference=fl::render_cpu(lens,sources,cfg,w,h);
  if(reference.rgb.size()!=n*3)return 6;
  for(int mode:{1,2,4}){
   view.output_mode=mode;
   if(!fl::gpu::metal_compose(lens,sources,cfg,view,
       (__bridge void*)scratch,scratch_bytes)){
    std::cerr<<"Metal compose failed mode="<<mode<<"\n";
    return 10+mode;
   }
   const auto* result=static_cast<const float*>(out.contents);
   double diff=0,energy=0;
   for(size_t i=0;i<n;i++){
    for(int c=0;c<3;c++){
     const float actual=result[i*4+2-c]-(mode==1?bg[i*4+2-c]:0.f);
     const float expected=reference.rgb[i*3+c];
     if(!std::isfinite(actual))return 20+mode;
     diff+=std::abs(double(actual)-expected);
     energy+=std::abs(double(expected));
    }
   }
   const double relative=diff/std::max(1e-9,energy);
   std::cout<<"METAL_CPU_PARITY mode="<<mode<<" relL1="<<relative
            <<" reference_energy="<<energy<<"\n";
   if(energy<1e-7||relative>.08)return 30+mode;
  }
  // Exercise nonzero separable ghost blur against the shared CPU baseline.
  view.output_mode=2;
  view.ghost_blur=.025f;
  view.ghost_blur_passes=2;
  if(!fl::gpu::metal_compose(lens,sources,cfg,view,
      (__bridge void*)scratch,scratch_bytes))return 46;
  auto blurred=reference.rgb;
  const int radius=int(std::round(view.ghost_blur*
                                    std::hypot(float(w),float(h))));
  fl::box_blur_rgb(blurred,w,h,radius,view.ghost_blur_passes);
  const auto* blur_result=static_cast<const float*>(out.contents);
  double blur_diff=0,blur_energy=0;
  for(size_t i=0;i<n;i++)for(int c=0;c<3;c++){
   blur_diff+=std::abs(double(blur_result[i*4+2-c])-blurred[i*3+c]);
   blur_energy+=std::abs(double(blurred[i*3+c]));
  }
  if(blur_energy<1e-8||blur_diff/blur_energy>.04)return 47;
  std::cout<<"METAL_GHOST_BLUR_CPU_PARITY relL1="
           <<blur_diff/blur_energy<<"\n";
  view.ghost_blur=0.f;
  view.output_mode=3;
  if(!fl::gpu::metal_compose(lens,sources,cfg,view,
       (__bridge void*)scratch,scratch_bytes))return 40;
  const auto* preview=static_cast<const float*>(out.contents);
  const int cx=int(sources[0].x*w),cy=int(sources[0].y*h);
  const size_t marker=(size_t(cy)*w+cx)*4;
  if(preview[marker]<1.f||preview[marker+1]<1.f||
     preview[marker+2]<1.f)return 41;
  view.output_mode=5;
  view.haze_gain=8.f;
  view.haze_radius=.1f;
  view.haze_passes=3;
  if(!fl::gpu::metal_compose(lens,sources,cfg,view,
       (__bridge void*)scratch,scratch_bytes))return 42;
  const auto* haze=static_cast<const float*>(out.contents);
  double haze_energy=0;
  for(size_t i=0;i<n;i++)haze_energy+=haze[i*4]+haze[i*4+1]+haze[i*4+2];
  if(haze_energy<=0||!std::isfinite(haze_energy))return 43;
  std::vector<fl::Source> dense;
  for(int i=0;i<12;i++)
   dense.push_back({.2f+.05f*i,.3f+.02f*i,1.f,.8f,.6f});
  view.haze_footprint=4;
  if(!fl::gpu::metal_compose(lens,dense,cfg,view,
       (__bridge void*)scratch,scratch_bytes))return 48;
  const auto* dense_result=static_cast<const float*>(out.contents);
  double dense_energy=0;
  for(size_t i=0;i<n;i++)
   dense_energy+=dense_result[i*4]+dense_result[i*4+1]+dense_result[i*4+2];
  if(!std::isfinite(dense_energy)||dense_energy<=0)return 49;
  std::cout<<"METAL_DENSE_HAZE_GPU_PASSED energy="<<dense_energy<<"\n";
  view.haze_footprint=1;
  std::vector<float> bright(n*4,0.f);
  for(int y=20;y<24;y++)for(int x=40;x<44;x++){
   const size_t i=(size_t(y)*w+x)*4;
   bright[i]=12.f;bright[i+1]=15.f;bright[i+2]=20.f;bright[i+3]=1.f;
  }
  memcpy(in.contents,bright.data(),bytes);
  fl::gpu::DetectionSettings detect{};
  detect.width=w;detect.height=h;detect.input_pitch_bytes=w*16;
  detect.scan_step=4;detect.source_threshold=2.f;detect.max_sources=8;
  const size_t detect_bytes=fl::gpu::metal_detection_scratch_bytes(w,h,4);
  auto detect_scratch=[dev newBufferWithLength:detect_bytes
      options:MTLResourceStorageModePrivate];
  std::vector<fl::Source> found;
  if(!fl::gpu::detect_sources_metal((__bridge void*)in,
      (__bridge void*)detect_scratch,detect_bytes,
      (__bridge void*)queue,detect,found))return 44;
  if(found.empty()||std::abs(found[0].x-42.f/w)>.02f||
     std::abs(found[0].y-22.f/h)>.03f)return 45;
  std::cout<<"METAL_GPU_RAYTRACE_BLUR_HAZE_PREVIEW_DETECTION_PASSED\n";
  // Fail closed before a GPU dispatch when AE supplies a short or otherwise
  // invalid host-owned buffer, rather than reading out of bounds in Metal.
  auto short_input=[dev newBufferWithLength:16
      options:MTLResourceStorageModeShared];
  if(!short_input)return 50;
  view.input=(__bridge void*)short_input;
  view.output_mode=1;
  view.haze_gain=0.f;
  if(fl::gpu::metal_compose(lens,sources,cfg,view,
      (__bridge void*)scratch,scratch_bytes))return 51;
  view.input=(__bridge void*)in;
  view.output_pitch_bytes=w*16-1;
  if(fl::gpu::metal_compose(lens,sources,cfg,view,
      (__bridge void*)scratch,scratch_bytes))return 52;
  view.output_pitch_bytes=w*16;
  if(fl::gpu::metal_zero((__bridge void*)short_input,32,
      (__bridge void*)queue))return 53;
  if(fl::gpu::metal_zero((__bridge void*)short_input,3,
      (__bridge void*)queue))return 54;
  // Detector validates full input allocation and stride before encoding.
  if(fl::gpu::detect_sources_metal((__bridge void*)short_input,
      (__bridge void*)detect_scratch,detect_bytes,
      (__bridge void*)queue,detect,found))return 55;
  detect.input_pitch_bytes=w*16+1;
  if(fl::gpu::detect_sources_metal((__bridge void*)in,
      (__bridge void*)detect_scratch,detect_bytes,
      (__bridge void*)queue,detect,found))return 56;
  detect.input_pitch_bytes=w*16;

  // Expand an optical tile beyond the sensor: destination padding must be
  // transparent and valid pixels must map back to their origin exactly.
  constexpr int sw=4,sh=3,ow=6,oh=5;
  std::vector<float> source_tile(size_t(sw)*sh*4,0.f);
  for(int y=0;y<sh;y++)for(int x=0;x<sw;x++)
    source_tile[(size_t(y)*sw+x)*4]=float(1+y*sw+x);
  auto region_src=[dev newBufferWithBytes:source_tile.data()
      length:source_tile.size()*sizeof(float)
      options:MTLResourceStorageModeShared];
  auto region_dst=[dev newBufferWithLength:size_t(ow)*oh*16
      options:MTLResourceStorageModeShared];
  if(!region_src||!region_dst)return 57;
  if(!fl::gpu::metal_copy_region((__bridge void*)region_src,
      (__bridge void*)region_dst,(__bridge void*)queue,
      sw*16,ow*16,0,0,sw,sh,-1,-1,ow,oh,true))return 58;
  auto region=static_cast<const float*>(region_dst.contents);
  for(int y=0;y<oh;y++)for(int x=0;x<ow;x++){
    const float expected=(x>=1&&x<=sw&&y>=1&&y<=sh)
        ?float((y-1)*sw+x):0.f;
    if(std::abs(region[(size_t(y)*ow+x)*4]-expected)>1e-6f)
      return 59;
  }
  if(fl::gpu::metal_copy_region((__bridge void*)region_src,
      (__bridge void*)region_dst,(__bridge void*)queue,
      sw*16,ow*16,0,0,sw,sh,0,0,2,2,false))return 60;
  if(fl::gpu::metal_copy_region((__bridge void*)region_src,
      (__bridge void*)region_dst,(__bridge void*)queue,
      sw*16,ow*16,0,0,sw,sh,1,1,ow,oh,false))return 61;

  // A multi-GPU Intel host may change its active Metal device. Never use
  // resource objects from one GPU on a command queue from another.
  for(id<MTLDevice> other in MTLCopyAllDevices()){
    if(other==dev)continue;
    auto other_queue=[other newCommandQueue];
    if(!other_queue)continue;
    if(fl::gpu::metal_zero((__bridge void*)in,16,
        (__bridge void*)other_queue))return 62;
    view.command_stream=(__bridge void*)other_queue;
    if(fl::gpu::metal_compose(lens,sources,cfg,view,
        (__bridge void*)scratch,scratch_bytes))return 63;
    view.command_stream=(__bridge void*)queue;
    break;
  }
  std::cout<<"METAL_RESOURCE_BOUNDS_AND_DEVICE_ISOLATION_PASSED\n";
  return 0;
 }
}
