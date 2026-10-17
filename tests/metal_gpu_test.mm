#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "FlareLensMetal.hpp"
#include "FlareLensCore.hpp"
#include "FlareLensOptics.hpp"
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <thread>
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
  // The shared fixture later turns ghost blur on. The real AE adapter
  // computes scratch for each parameter snapshot; reserve the maximum here
  // and test the minimal per-mode sizes separately below.
  const size_t scratch_bytes=fl::gpu::metal_scratch_bytes(
    lens,1,cfg.grid,w,h,1,true,false,false);
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
  // AE may not provide any input GPU world for a blank solid. Flare-only,
  // ghost-only and haze-only must not dereference an unbound Metal buffer.
  view.input=nullptr;
  view.haze_gain=0.f;
  view.ghost_blur=0.f;
  for(int mode:{2,4,5}){
    view.output_mode=mode;
    if(!fl::gpu::metal_compose(lens,sources,cfg,view,
         (__bridge void*)scratch,scratch_bytes))return 65+mode;
    const float* blank=static_cast<const float*>(out.contents);
    for(size_t i=0;i<n*4;i++)
      if(!std::isfinite(blank[i]))return 70+mode;
    if(mode==2||mode==4){
      double energy=0;
      for(size_t i=0;i<n;i++)
        energy+=std::abs(blank[4*i])+std::abs(blank[4*i+1])+
                std::abs(blank[4*i+2]);
      if(energy<1e-7)return 75+mode;
    }
  }
  view.output_mode=3;
  if(fl::gpu::metal_compose(lens,sources,cfg,view,
        (__bridge void*)scratch,scratch_bytes))return 79;
  view.input=(__bridge void*)in;
  std::cout<<"METAL_BLANK_INPUT_SOURCE_WORLD_PASSED\n";
  // Exact GPU scratch requirements: preserve output while removing one
  // entire RGBA32F frame from common unblurred/analytic render modes.
  const size_t one_image=bytes;
  const size_t single_ghost=fl::gpu::metal_scratch_bytes(
    lens,1,cfg.grid,w,h,2,false,false,false);
  if(single_ghost!=one_image)return 80;
  const size_t blurred_ghost=fl::gpu::metal_scratch_bytes(
    lens,1,cfg.grid,w,h,2,true,false,false);
  if(blurred_ghost!=2*one_image)return 81;
  const size_t analytic_haze=fl::gpu::metal_scratch_bytes(
    lens,1,cfg.grid,w,h,5,false,false,true);
  if(analytic_haze!=16)return 82;
  const size_t dense_haze=fl::gpu::metal_scratch_bytes(
    lens,12,cfg.grid,w,h,5,false,true,false);
  if(dense_haze!=2*one_image)return 83;
  auto one_image_buffer=[dev newBufferWithLength:single_ghost
      options:MTLResourceStorageModePrivate];
  auto tiny_buffer=[dev newBufferWithLength:analytic_haze
      options:MTLResourceStorageModePrivate];
  if(!one_image_buffer||!tiny_buffer)return 84;
  view.input=nullptr;
  view.output_mode=2;
  view.haze_gain=0.f;
  view.ghost_blur=0.f;
  if(!fl::gpu::metal_compose(lens,sources,cfg,view,
      (__bridge void*)one_image_buffer,single_ghost))return 85;
  auto ghost_only=static_cast<const float*>(out.contents);
  double ghost_energy=0;
  for(size_t i=0;i<n;i++)
    ghost_energy+=std::abs(ghost_only[i*4])+
                 std::abs(ghost_only[i*4+1])+
                 std::abs(ghost_only[i*4+2]);
  if(!(ghost_energy>1e-7))return 86;
  view.output_mode=5;
  view.haze_gain=8.f;
  view.haze_radius=.1f;
  view.haze_passes=3;
  view.haze_footprint=1;
  if(!fl::gpu::metal_compose(lens,sources,cfg,view,
      (__bridge void*)tiny_buffer,analytic_haze))return 87;
  const float* haze_only=static_cast<const float*>(out.contents);
  double haze_only_energy=0;
  for(size_t i=0;i<n;i++)
    haze_only_energy+=std::abs(haze_only[i*4])+
                      std::abs(haze_only[i*4+1])+
                      std::abs(haze_only[i*4+2]);
  if(!(haze_only_energy>1e-7))return 88;
  view.input=(__bridge void*)in;
  view.haze_gain=0.f;
  std::cout<<"METAL_MINIMAL_SCRATCH_ALLOCATIONS_PASSED\n";
  // Reported black result: same manual source must remain compositable when
  // the layer supplies a fully transparent BGRA32F frame. Test the GPU
  // composition path, not merely the standalone ghost-only buffer.
  std::memset(in.contents,0,bytes);
  view.input=(__bridge void*)in;
  view.output_mode=1;
  view.haze_gain=0.f;
  view.ghost_blur=0.f;
  if(!fl::gpu::metal_compose(lens,sources,cfg,view,
       (__bridge void*)one_image_buffer,single_ghost))return 89;
  const float* composite=static_cast<const float*>(out.contents);
  double visible_rgb=0,visible_alpha=0;
  size_t orphan_rgb=0;
  for(size_t i=0;i<n;i++){
    const float b=composite[4*i],g=composite[4*i+1],
                r=composite[4*i+2],a=composite[4*i+3];
    if(!std::isfinite(b)||!std::isfinite(g)||!std::isfinite(r)||
       !std::isfinite(a)||a<0.f||a>1.f)return 90;
    const float rgb=std::max({b,g,r});
    if(rgb>1.f/255.f&&a<=0.f)orphan_rgb++;
    if(rgb>1.f/255.f&&a>0.f){
      visible_rgb+=rgb;visible_alpha+=a;
    }
  }
  if(visible_rgb<=0.001||visible_alpha<=0.001||orphan_rgb!=0)return 91;
  std::cout<<"METAL_TRANSPARENT_COMPOSITE_NOT_BLACK_PASSED"
           <<" rgb="<<visible_rgb<<" alpha="<<visible_alpha<<"\n";

  // Simulate independent AE MFR callbacks on one host-owned Metal device.
  // Each callback owns its queue, buffers and scratch; immutable pipelines
  // are intentionally shared through the backend's device-keyed cache.
  std::atomic<int> mfr_failures{0};
  std::vector<std::thread> workers;
  for(int worker=0;worker<4;worker++){
    workers.emplace_back([&,worker]{
      @autoreleasepool {
        auto local_queue=[dev newCommandQueue];
        auto local_src=[dev newBufferWithBytes:bg.data() length:bytes
            options:MTLResourceStorageModeShared];
        auto local_dst=[dev newBufferWithLength:bytes
            options:MTLResourceStorageModeShared];
        auto local_scratch=[dev newBufferWithLength:scratch_bytes
            options:MTLResourceStorageModePrivate];
        if(!local_queue||!local_src||!local_dst||!local_scratch){
          mfr_failures.fetch_add(1);return;
        }
        fl::gpu::View local_view{
          (__bridge void*)local_src,(__bridge void*)local_dst,w,h,w*16,w*16};
        local_view.command_stream=(__bridge void*)local_queue;
        local_view.output_mode=2;
        for(int iteration=0;iteration<3;iteration++){
          if(!fl::gpu::metal_compose(lens,sources,cfg,local_view,
              (__bridge void*)local_scratch,scratch_bytes)){
            mfr_failures.fetch_add(1);return;
          }
          const auto* data=static_cast<const float*>(local_dst.contents);
          double deviation=0;
          for(size_t i=0;i<n;i++)for(int c=0;c<3;c++){
            const float value=data[i*4+2-c];
            if(!std::isfinite(value)){
              mfr_failures.fetch_add(1);return;
            }
            deviation+=std::abs(double(value)-reference.rgb[i*3+c]);
          }
          double total=0;
          for(const float v:reference.rgb)total+=std::abs(double(v));
          if(total<1e-8||deviation/total>.08){
            mfr_failures.fetch_add(1);return;
          }
        }
      }
    });
  }
  for(auto& worker:workers)worker.join();
  if(mfr_failures.load()!=0)return 64;
  std::cout<<"METAL_4_THREAD_12_FRAME_MFR_PARITY_PASSED\n";
  return 0;
 }
}
