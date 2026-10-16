// Native Metal backend. No CUDA dependency; same optical equations and
// shared FlareLensGpuBridge data contract as the Windows renderer.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "FlareLensGpuBridge.hpp"
#include "FlareLensGpuDetection.hpp"
#include "FlareLensOptics.hpp"
#include "lens.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <queue>
#include <unordered_map>

static_assert(sizeof(Surface)==32,"Metal Surface POD layout mismatch");

@interface FlareSimMetalLocator : NSObject @end
@implementation FlareSimMetalLocator @end

namespace {
struct Pair {int a,b;float boost;};
struct Light {float ax,ay,r,g,b,dx,dy,dz;};
struct Sample {float u,v;};
struct Band {float lambda,r,g,b;};
struct Trace {
 uint32_t w,h,pitch,nsurfs,npairs,nsources,npupil,nbands;
 uint32_t offset,count;
 float sensor_z,front_radius,start_z,half_w,half_h,gain,ray_weight;
};
struct Image {uint32_t w,h,src_pitch,dst_pitch,mode;float gain;};
struct Blur {uint32_t w,h,radius;};
struct Haze {uint32_t w,h,pitch,count;float gain;};
struct Detect {uint32_t w,h,pitch,step,dw,dh;float threshold,cap;uint32_t gate;};
struct Seed {int32_t x,y;float b,g,r;};
struct SeedInfo {uint32_t w,h,footprint,count;};
static_assert(sizeof(Trace)==68 && sizeof(Light)==32 && sizeof(Detect)==36);

id<MTLComputePipelineState> pipeline(id<MTLDevice> device,NSString* name){
 // Pipeline states are immutable. Cache per native Metal device for AE MFR.
 static std::mutex mutex;
 static std::unordered_map<void*,id<MTLLibrary>> libraries;
 static std::unordered_map<std::string,id<MTLComputePipelineState>> states;
 std::lock_guard<std::mutex> lock(mutex);
 const void* key=(__bridge void*)device;
 const std::string name_key=std::to_string(uintptr_t(key))+":"+name.UTF8String;
 if(auto found=states.find(name_key);found!=states.end())return found->second;
 id<MTLLibrary> library=nil;
 if(auto found=libraries.find(const_cast<void*>(key));found!=libraries.end())
   library=found->second;
 else {
  NSBundle* bundle=[NSBundle bundleForClass:[FlareSimMetalLocator class]];
  NSString* path=[bundle pathForResource:@"FlareLensMetal" ofType:@"metallib"];
  // The standalone Metal GPU test loads the same CI-compiled metallib.
  if(!path&&getenv("FLARELENS_METAL_LIBRARY"))
    path=[NSString stringWithUTF8String:getenv("FLARELENS_METAL_LIBRARY")];
  if(!path)return nil;
  NSError* error=nil;
  library=[device newLibraryWithFile:path error:&error];
  if(!library)return nil;
  libraries[const_cast<void*>(key)]=library;
 }
 id<MTLFunction> function=[library newFunctionWithName:name];
 if(!function)return nil;
 NSError* error=nil;
 id<MTLComputePipelineState> state=
    [device newComputePipelineStateWithFunction:function error:&error];
 if(state)states.emplace(name_key,state);
 return state;
}
id<MTLBuffer> upload(id<MTLDevice> dev,const void* data,size_t bytes){
 if(!data||!bytes)return nil;
 return [dev newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
}
bool dispatch(id<MTLCommandBuffer> cmd,id<MTLDevice> device,NSString* name,
              NSUInteger count,
              id<MTLBuffer> b0,id<MTLBuffer> b1,id<MTLBuffer> b2,
              id<MTLBuffer> b3,id<MTLBuffer> b4,id<MTLBuffer> b5,
              const void* constants,size_t constants_bytes,
              NSUInteger o0=0,NSUInteger o1=0){
 if(!count)return true;
 auto state=pipeline(device,name);
 if(!state)return false;
 id<MTLComputeCommandEncoder> encoder=[cmd computeCommandEncoder];
 if(!encoder)return false;
 [encoder setComputePipelineState:state];
 const id<MTLBuffer> buffers[]={b0,b1,b2,b3,b4,b5};
 for(int i=0;i<6;i++)if(buffers[i])
   [encoder setBuffer:buffers[i] offset:i==0?o0:i==1?o1:0 atIndex:i];
 if(constants&&constants_bytes)
   [encoder setBytes:constants length:constants_bytes atIndex:6];
 const NSUInteger group=std::min<NSUInteger>(256,state.maxTotalThreadsPerThreadgroup);
 [encoder dispatchThreads:MTLSizeMake(count,1,1)
   threadsPerThreadgroup:MTLSizeMake(group,1,1)];
 [encoder endEncoding];
 return true;
}
// Constants must occupy the exact shader buffer index for each kernel.
bool encode(id<MTLCommandBuffer> cmd,id<MTLDevice> device,NSString* name,
 NSUInteger count,const std::vector<id<MTLBuffer>>& buffers,
 const void* constants,size_t bytes,NSUInteger constants_index,
 NSUInteger first_offset=0,NSUInteger second_offset=0){
 if(!count)return true;
 auto state=pipeline(device,name);
 if(!state)return false;
 id<MTLComputeCommandEncoder> enc=[cmd computeCommandEncoder];
 if(!enc)return false;
 [enc setComputePipelineState:state];
 for(NSUInteger i=0;i<buffers.size();i++)
  if(buffers[i])[enc setBuffer:buffers[i]
    offset:i==0?first_offset:i==1?second_offset:0 atIndex:i];
 if(constants&&bytes)[enc setBytes:constants length:bytes atIndex:constants_index];
 const NSUInteger group=std::min<NSUInteger>(256,state.maxTotalThreadsPerThreadgroup);
 [enc dispatchThreads:MTLSizeMake(count,1,1)
   threadsPerThreadgroup:MTLSizeMake(group,1,1)];
 [enc endEncoding];
 return true;
}
bool finish(id<MTLCommandBuffer> cmd){
 [cmd commit];[cmd waitUntilCompleted];
 return cmd.status==MTLCommandBufferStatusCompleted;
}
std::vector<float> haze_profile(int n,int center,int radius,int passes){
 std::vector<float> v(size_t(n),0.f),next(size_t(n),0.f);
 if(center>=0&&center<n)v[size_t(center)]=1.f;
 std::vector<double> prefix(size_t(n)+1);
 for(int pass=0;pass<passes;pass++){
  prefix[0]=0;
  for(int x=0;x<n;x++)prefix[size_t(x)+1]=prefix[size_t(x)]+v[size_t(x)];
  for(int x=0;x<n;x++){
   const int lo=std::max(0,x-radius),hi=std::min(n-1,x+radius);
   next[size_t(x)]=float((prefix[size_t(hi)+1]-prefix[size_t(lo)])/(hi-lo+1));
  }
  v.swap(next);
 }
 return v;
}
struct Candidate {float score;int index;fl::Source source;};
struct Better {
 bool operator()(const Candidate&a,const Candidate&b)const{
  return a.score>b.score||(a.score==b.score&&a.index<b.index);
 }
};
float luma(const fl::Source& s){return .2126f*s.r+.7152f*s.g+.0722f*s.b;}
} // namespace

namespace fl::gpu {
size_t metal_scratch_bytes(const LensSystem& lens,int count,int grid,int w,int h,
 int mode,bool,bool,bool) noexcept {
 if(count<1||count>2048||grid<2||grid>2048||w<1||h<1||
    w>16384||h>16384||lens.surfaces.size()<2||lens.surfaces.size()>64||
    mode<1||mode>5)return 0;
 if(mode==3)return 16;
 const size_t bytes=size_t(w)*size_t(h)*16;
 if(bytes>size_t(1024)*1024*1024)return 0;
 return bytes*2;
}
bool metal_zero(void* buffer,size_t bytes,void* command_queue){
 id<MTLBuffer> dst=(__bridge id<MTLBuffer>)buffer;
 id<MTLCommandQueue> queue=(__bridge id<MTLCommandQueue>)command_queue;
 if(!dst||!queue||bytes>dst.length)return false;
 id<MTLCommandBuffer> cmd=[queue commandBuffer];
 id<MTLBlitCommandEncoder> blit=[cmd blitCommandEncoder];
 [blit fillBuffer:dst range:NSMakeRange(0,bytes) value:0];
 [blit endEncoding];return finish(cmd);
}
bool metal_copy_region(void* source,void* destination,void* command_queue,
 size_t src_pitch,size_t dst_pitch,int left,int top,int right,int bottom,
 int origin_x,int origin_y,int output_width,int output_height,bool padded){
 id<MTLBuffer> src=(__bridge id<MTLBuffer>)source;
 id<MTLBuffer> dst=(__bridge id<MTLBuffer>)destination;
 id<MTLCommandQueue> queue=(__bridge id<MTLCommandQueue>)command_queue;
 if(!src||!dst||!queue||right<=left||bottom<=top)return false;
 const size_t row=size_t(right-left)*16;
 const size_t dst_row=size_t(output_width)*16;
 if(dst_pitch<dst_row||src_pitch<size_t(right)*16)return false;
 if(size_t(output_height)*dst_pitch>dst.length)return false;
 if(size_t(bottom)*src_pitch>src.length)return false;
 id<MTLCommandBuffer> cmd=[queue commandBuffer];
 id<MTLBlitCommandEncoder> blit=[cmd blitCommandEncoder];
 if(padded)[blit fillBuffer:dst range:NSMakeRange(0,size_t(output_height)*dst_pitch) value:0];
 for(int y=top;y<bottom;y++){
  const size_t src_offset=size_t(y)*src_pitch+size_t(left)*16;
  const size_t dst_offset=size_t(y-origin_y)*dst_pitch+size_t(left-origin_x)*16;
  if(dst_offset+row>dst.length){[blit endEncoding];return false;}
  [blit copyFromBuffer:src sourceOffset:src_offset
      toBuffer:dst destinationOffset:dst_offset size:row];
 }
 [blit endEncoding];return finish(cmd);
}
bool detect_sources_metal(const void* input,void* scratch,size_t scratch_size,
 void* queue_raw,const DetectionSettings& s,std::vector<fl::Source>& result){
 result.clear();
 if(!input)return true;
 id<MTLBuffer> src=(__bridge id<MTLBuffer>)input;
 id<MTLBuffer> gpu=(__bridge id<MTLBuffer>)scratch;
 id<MTLCommandQueue> queue=(__bridge id<MTLCommandQueue>)queue_raw;
 if(!src||!gpu||!queue)return false;
 const int step=std::clamp(s.scan_step,1,32),w=s.width,h=s.height;
 if(w<=0||h<=0||w>16384||h>16384||s.input_pitch_bytes<w*16)return false;
 const int dw=std::max(1,w/step),dh=std::max(1,h/step);
 const size_t bytes=size_t(dw)*dh*16;
 if(bytes>scratch_size||bytes>gpu.length)return false;
 auto dev=queue.device;
 id<MTLBuffer> readback=[dev newBufferWithLength:bytes
                               options:MTLResourceStorageModeShared];
 if(!readback)return false;
 Detect d{uint32_t(w),uint32_t(h),uint32_t(s.input_pitch_bytes/16),
          uint32_t(step),uint32_t(dw),uint32_t(dh),
          s.source_threshold,s.source_cap,uint32_t(s.gate_alpha)};
 id<MTLCommandBuffer> cmd=[queue commandBuffer];
 if(!encode(cmd,dev,@"fl_detect_tiles",size_t(dw)*dh,{src,gpu},&d,sizeof(d),2))
   return false;
 id<MTLBlitCommandEncoder> blit=[cmd blitCommandEncoder];
 [blit copyFromBuffer:gpu sourceOffset:0 toBuffer:readback destinationOffset:0 size:bytes];
 [blit endEncoding];
 if(!finish(cmd))return false;
 const auto* blocks=static_cast<const simd_float4*>(readback.contents);
 const int max_sources=std::clamp(s.max_sources,1,2048);
 const int limit=s.cluster_radius>0?std::min(8192,max_sources*4):max_sources;
 std::priority_queue<Candidate,std::vector<Candidate>,Better> best;
 for(size_t i=0;i<size_t(dw)*dh;i++){
  const auto v=blocks[i];
  if(v.w<0||!std::isfinite(v.w))continue;
  const int bx=int(i%dw),by=int(i/dw),x=bx*step,y=by*step;
  const float cx=.5f*(x+std::min(w,x+step));
  const float cy=.5f*(y+std::min(h,y+step));
  Candidate candidate{v.w,int(i),{cx/float(w),cy/float(h),
      v.x*s.tint_r,v.y*s.tint_g,v.z*s.tint_b}};
  if(int(best.size())<limit)best.push(candidate);
  else if(Better{}(candidate,best.top())){best.pop();best.push(candidate);}
 }
 std::vector<Candidate> ranked;
 while(!best.empty()){ranked.push_back(best.top());best.pop();}
 std::sort(ranked.begin(),ranked.end(),Better{});
 for(const auto& item:ranked)result.push_back(item.source);
 if(s.cluster_radius>0&&result.size()>1){
  const float radius2=float(s.cluster_radius)*s.cluster_radius;
  std::vector<fl::Source> clustered;
  std::vector<uint8_t> consumed(result.size(),0);
  for(size_t i=0;i<result.size();i++){
   if(consumed[i])continue;
   auto src=result[i];
   float weight=std::max(0.f,luma(src));
   float cx=src.x*weight,cy=src.y*weight;
   for(size_t j=i+1;j<result.size();j++){
    if(consumed[j])continue;
    const float dx=(result[j].x-result[i].x)*w;
    const float dy=(result[j].y-result[i].y)*h;
    if(dx*dx+dy*dy>radius2)continue;
    const float wt=std::max(0.f,luma(result[j]));
    weight+=wt;cx+=result[j].x*wt;cy+=result[j].y*wt;
    src.r+=result[j].r;src.g+=result[j].g;src.b+=result[j].b;
    consumed[j]=1;
   }
   if(weight>0){src.x=cx/weight;src.y=cy/weight;}
   clustered.push_back(src);
  }
  result.swap(clustered);
  if(result.size()>size_t(max_sources)){
   std::stable_sort(result.begin(),result.end(),
      [](const auto&a,const auto&b){return luma(a)>luma(b);});
   result.resize(max_sources);
  }
 }
 return true;
}
bool metal_compose(const LensSystem& lens,const std::vector<Source>& sources,
 const Settings& settings,const View& v,void* scratch,size_t scratch_size){
 id<MTLCommandQueue> queue=(__bridge id<MTLCommandQueue>)v.command_stream;
 id<MTLBuffer> src=(__bridge id<MTLBuffer>)v.input;
 id<MTLBuffer> dst=(__bridge id<MTLBuffer>)v.output;
 id<MTLBuffer> temp=(__bridge id<MTLBuffer>)scratch;
 if(!queue||!dst||v.width<1||v.height<1||v.width>16384||v.height>16384||
    v.output_pitch_bytes<v.width*16||v.output_pitch_bytes%16||
    (v.output_mode==1&&(!src||v.input_pitch_bytes<v.width*16))||
    v.output_mode<1||v.output_mode>5)return false;
 const int w=v.width,h=v.height;
 const size_t pixels=size_t(w)*h,bytes=pixels*16;
 if(size_t(v.output_pitch_bytes)*h>dst.length)return false;
 if(src&&size_t(v.input_pitch_bytes)*h>src.length)return false;
 auto dev=queue.device;
 std::vector<Light> lights;
 lights.reserve(sources.size());
 const float fov=std::clamp(settings.fov_h_deg,1.f,150.f)*.0174532925199433f;
 const float sx=lens.focal_length*std::tan(fov*.5f);
 const float sy=settings.fov_v_deg>0?
  lens.focal_length*std::tan(std::clamp(settings.fov_v_deg,1.f,150.f)*.00872664625997f):
  sx*h/float(w);
 if(!(sx>0&&sy>0)&&v.output_mode!=3)return false;
 for(const auto& source:sources){
  if(v.output_mode==3){lights.push_back({source.x,source.y,source.r,source.g,source.b,0,0,0});continue;}
  const float ax=std::atan((source.x*2-1)*std::tan(fov*.5f));
  const float ay=std::atan((source.y*2-1)*sy/lens.focal_length);
  const float x=std::tan(ax),y=std::tan(ay);
  const float inv=1.f/std::sqrt(x*x+y*y+1.f);
  lights.push_back({ax,ay,source.r,source.g,source.b,x*inv,y*inv,inv});
 }
 auto light_gpu=upload(dev,lights.data(),lights.size()*sizeof(Light));
 if(!lights.empty()&&!light_gpu)return false;
 Image image{uint32_t(w),uint32_t(h),uint32_t(v.input_pitch_bytes/16),
             uint32_t(v.output_pitch_bytes/16),uint32_t(v.output_mode),1.f};
 id<MTLCommandBuffer> cmd=[queue commandBuffer];
 if(v.output_mode==3){
  // The shader computes dimmed input and cross markers in one GPU pass.
  uint32_t count=uint32_t(lights.size());
  auto state=pipeline(dev,@"fl_preview");
  if(!state||!src)return false;
  id<MTLComputeCommandEncoder> enc=[cmd computeCommandEncoder];
  [enc setComputePipelineState:state];
  [enc setBuffer:src offset:0 atIndex:0];
  [enc setBuffer:dst offset:0 atIndex:1];
  if(light_gpu)[enc setBuffer:light_gpu offset:0 atIndex:2];
  [enc setBytes:&image length:sizeof(image) atIndex:3];
  [enc setBytes:&count length:sizeof(count) atIndex:4];
  [enc dispatchThreads:MTLSizeMake(pixels,1,1)
      threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(256,state.maxTotalThreadsPerThreadgroup),1,1)];
  [enc endEncoding];
  return finish(cmd);
 }
 if(!encode(cmd,dev,@"fl_plate",pixels,{src,dst},&image,sizeof(image),2))
  return false;
 if(sources.empty()||((settings.gain<=0||v.output_mode==5)&&
    (v.haze_gain<=0||v.output_mode==4)))return finish(cmd);
 if(!temp||scratch_size<bytes*2||temp.length<bytes*2)return false;
 id<MTLBlitCommandEncoder> clear=[cmd blitCommandEncoder];
 [clear fillBuffer:temp range:NSMakeRange(0,bytes*2) value:0];
 [clear endEncoding];
 const bool ghosts=settings.gain>0&&v.output_mode!=5;
 const bool haze=(v.output_mode==1||v.output_mode==2||v.output_mode==5)&&v.haze_gain>0;
 if(ghosts){
  const auto pairs_storage=fl::cached_pairs(lens,sx,sy);
  std::vector<Pair> pairs;
  const int count=std::min<int>(int(pairs_storage->pairs.size()),settings.max_pairs);
  for(int i=0;i<count;i++){
   const auto& pair=pairs_storage->pairs[i];
   const bool selected=pair.surf_a==settings.pair_surface_a&&
                       pair.surf_b==settings.pair_surface_b;
   if((settings.pair_mode==1&&!selected)||
      (settings.pair_mode==2&&selected))continue;
   pairs.push_back({pair.surf_a,pair.surf_b,pairs_storage->boosts[i]});
  }
  const auto pupil_storage=fl::cached_pupil(settings);
  const auto& pupil=*pupil_storage;
  const auto spectrum=fl::make_spectrum(settings);
  std::vector<Sample> samples;
  for(const auto& s:pupil)samples.push_back({s.u,s.v});
  std::vector<Band> bands;
  for(const auto& s:spectrum)bands.push_back({s.lambda,s.r,s.g,s.b});
  if(!pairs.empty()&&!samples.empty()&&!bands.empty()){
   auto surface_gpu=upload(dev,lens.surfaces.data(),lens.surfaces.size()*sizeof(Surface));
   auto pair_gpu=upload(dev,pairs.data(),pairs.size()*sizeof(Pair));
   auto pupil_gpu=upload(dev,samples.data(),samples.size()*sizeof(Sample));
   auto band_gpu=upload(dev,bands.data(),bands.size()*sizeof(Band));
   if(!surface_gpu||!pair_gpu||!pupil_gpu||!band_gpu)return false;
   const uint64_t total=uint64_t(pairs.size())*lights.size()*samples.size()*bands.size();
   if(total>UINT32_MAX)return false;
   Trace trace{uint32_t(w),uint32_t(h),uint32_t(w),
    uint32_t(lens.surfaces.size()),uint32_t(pairs.size()),uint32_t(lights.size()),
    uint32_t(samples.size()),uint32_t(bands.size()),0,0,
    lens.sensor_z,lens.surfaces.front().semi_aperture,
    lens.surfaces.front().z-20.f,sx,sy,settings.gain,1.f/float(samples.size())};
   for(uint64_t offset=0;offset<total;offset+=65536){
    if(v.check_abort&&v.check_abort(v.abort_context))return false;
    trace.offset=uint32_t(offset);
    trace.count=uint32_t(std::min<uint64_t>(65536,total-offset));
    if(!encode(cmd,dev,@"fl_trace",trace.count,
       {surface_gpu,pair_gpu,light_gpu,pupil_gpu,band_gpu,temp},
       &trace,sizeof(trace),6))return false;
   }
  }
  const int radius=int(std::round(std::max(0.f,v.ghost_blur)*
                                   std::hypot(float(w),float(h))));
  const int passes=std::clamp(v.ghost_blur_passes,0,6);
  if(radius>0&&passes>0){
   const Blur blur{uint32_t(w),uint32_t(h),uint32_t(std::min(radius,std::max(w,h)))};
   for(int i=0;i<passes;i++){
    const bool wide=radius>32;
    const NSUInteger nx=wide?size_t(h)*((w+127)/128):pixels;
    const NSUInteger ny=wide?size_t(w)*((h+127)/128):pixels;
    if(!encode(cmd,dev,wide?@"fl_blur_x_wide":@"fl_blur_x",nx,
       {temp,temp},&blur,sizeof(blur),2,0,bytes))return false;
    if(!encode(cmd,dev,wide?@"fl_blur_y_wide":@"fl_blur_y",ny,
       {temp,temp},&blur,sizeof(blur),2,bytes,0))return false;
   }
  }
  if(!encode(cmd,dev,@"fl_add",pixels,{temp,dst},
      &image,sizeof(image),2))return false;
 }
 if(haze){
  const int radius=int(std::round(std::clamp(v.haze_radius,0.f,1.f)*
                                  std::hypot(float(w),float(h))));
  const int passes=std::clamp(v.haze_passes,1,6);
  const bool dense=sources.size()>8||v.haze_footprint>1;
  if(dense){
   std::vector<Seed> seeds;
   for(const auto& s:sources)
    seeds.push_back({int(s.x*w),int(s.y*h),s.b,s.g,s.r});
   auto seed_gpu=upload(dev,seeds.data(),seeds.size()*sizeof(Seed));
   if(!seed_gpu)return false;
   id<MTLBlitCommandEncoder> blit=[cmd blitCommandEncoder];
   [blit fillBuffer:temp range:NSMakeRange(0,bytes*2) value:0];
   [blit endEncoding];
   const SeedInfo si{uint32_t(w),uint32_t(h),
     uint32_t(std::clamp(v.haze_footprint,1,32)),uint32_t(seeds.size())};
   if(!encode(cmd,dev,@"fl_seed_haze",seeds.size(),{seed_gpu,temp},
      &si,sizeof(si),2))return false;
   if(radius>0&&passes>0){
    const Blur blur{uint32_t(w),uint32_t(h),uint32_t(std::min(radius,std::max(w,h)))};
    for(int i=0;i<passes;i++){
     const bool wide=radius>32;
     const NSUInteger nx=wide?size_t(h)*((w+127)/128):pixels;
     const NSUInteger ny=wide?size_t(w)*((h+127)/128):pixels;
     if(!encode(cmd,dev,wide?@"fl_blur_x_wide":@"fl_blur_x",nx,
       {temp,temp},&blur,sizeof(blur),2,0,bytes))return false;
     if(!encode(cmd,dev,wide?@"fl_blur_y_wide":@"fl_blur_y",ny,
       {temp,temp},&blur,sizeof(blur),2,bytes,0))return false;
    }
   }
   image.gain=v.haze_gain;
   if(!encode(cmd,dev,@"fl_add",pixels,{temp,dst},&image,sizeof(image),2))
    return false;
  }else{
   std::vector<float> hx,hy;
   for(const auto& s:sources){
    auto x=haze_profile(w,int(s.x*w),radius,passes);
    auto y=haze_profile(h,int(s.y*h),radius,passes);
    hx.insert(hx.end(),x.begin(),x.end());
    hy.insert(hy.end(),y.begin(),y.end());
   }
   auto x_gpu=upload(dev,hx.data(),hx.size()*sizeof(float));
   auto y_gpu=upload(dev,hy.data(),hy.size()*sizeof(float));
   const Haze hz{uint32_t(w),uint32_t(h),uint32_t(v.output_pitch_bytes/16),
                 uint32_t(sources.size()),v.haze_gain};
   if(!x_gpu||!y_gpu||!encode(cmd,dev,@"fl_analytic_haze",pixels,
      {light_gpu,x_gpu,y_gpu,dst},&hz,sizeof(hz),4))return false;
  }
 }
 if(v.output_mode==1&&!encode(cmd,dev,@"fl_alpha",pixels,
   {src,dst},&image,sizeof(image),2))return false;
 return finish(cmd);
}
} // namespace fl::gpu
