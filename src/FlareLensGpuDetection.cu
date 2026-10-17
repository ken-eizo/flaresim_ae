#include "FlareLensGpuDetection.hpp"
#include "FlareLensNukeSources.hpp"
#include <cuda_runtime.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <vector>

// Each thread reduces exactly one Nuke-style tile. The last floor-divided
// tile absorbs remainders; the nominal tile-centre is retained. BGRA128 GPU
// input stays device-local; only 16 bytes per source TILE go back to CPU.
namespace {
__global__ void reduce_bright_tiles(const float4* input,int pitch_pixels,
 int w,int h,int step,int dw,int dh,float threshold,float cap,bool gate,
 float4* tiles){
 const int tid=int(blockIdx.x)*256+int(threadIdx.x);
 if(tid>=dw*dh)return;
 const int by=tid/dw,bx=tid%dw;
 const int left=bx*step,top=by*step;
 const int right=bx+1==dw?w:min(w,left+step);
 const int bottom=by+1==dh?h:min(h,top+step);
 double red=0,green=0,blue=0,alpha=0;
 int n=0;
 for(int y=top;y<bottom;y++){
  for(int x=left;x<right;x++){
   const float4 c=input[size_t(y)*pitch_pixels+x];
   if(!isfinite(c.x)||!isfinite(c.y)||!isfinite(c.z))continue;
   // Nuke's host input is R,G,B; host-managed CUDA worlds are B,G,R,A.
   red+=double(c.z);green+=double(c.y);blue+=double(c.x);
   const float a=isfinite(c.w)?fminf(1.f,fmaxf(0.f,c.w)):0.f;
   alpha+=gate?double(a):1.0;
   ++n;
  }
 }
 float4 out=make_float4(0,0,0,-1);
 if(n){
  float r=float(red/n),g=float(green/n),b=float(blue/n);
  float score=.2126f*r+.7152f*g+.0722f*b;
  if(isfinite(score)){
   if(cap>0&&score>cap){
    const float mul=cap/score;
    r*=mul;g*=mul;b*=mul;score=cap;
   }
   const float mask=float(alpha/n);
   r*=mask;g*=mask;b*=mask;score*=mask;
   if(isfinite(score)&&score>=threshold)
    out=make_float4(r,g,b,score);
  }
 }
 tiles[tid]=out;
}
struct Candidate{
 float score=0;
 int index=0;
 fl::Source source{};
};
struct Better{
 bool operator()(const Candidate& a,const Candidate& b)const{
  return a.score>b.score||(a.score==b.score&&a.index<b.index);
 }
};
float source_luma(const fl::Source& source){
 return .2126f*source.r+.7152f*source.g+.0722f*source.b;
}
}

namespace fl::gpu {
size_t detection_scratch_bytes(int width,int height,int step) noexcept{
 if(width<=0||height<=0||width>16384||height>16384||
    step<1||step>32)return 0;
 const size_t dw=std::max(1,width/step),dh=std::max(1,height/step);
 const size_t bytes=dw*dh*sizeof(float4);
 // 128MB of tile metadata is an excessive allocation for this host. Such
 // requests must fall back to the CPU scanner without a silent crop.
 return bytes<=size_t(128)*1024*1024?bytes:0;
}
bool detect_sources_cuda(const void* input,void* device_scratch,
 size_t scratch_size,void* host_cuda_stream,const DetectionSettings& s,
 std::vector<fl::Source>& result,
 std::vector<fl::Source>* raw_sources){
 result.clear();
 if(!input)return true;
 const int step=std::clamp(s.scan_step,1,32);
 const int w=s.width,h=s.height;
 const size_t bytes=detection_scratch_bytes(w,h,step);
 if(!bytes||!device_scratch||scratch_size<bytes||s.input_pitch_bytes<w*16||
    s.input_pitch_bytes%16!=0)return false;
 const int dw=std::max(1,w/step),dh=std::max(1,h/step);
 const size_t tile_count=size_t(dw)*dh;
 auto stream=reinterpret_cast<cudaStream_t>(host_cuda_stream);
 const dim3 block(256),grid((unsigned(tile_count)+255)/256);
 reduce_bright_tiles<<<grid,block,0,stream>>>(
   static_cast<const float4*>(input),s.input_pitch_bytes/16,
   w,h,step,dw,dh,s.source_threshold,s.source_cap,s.gate_alpha,
   static_cast<float4*>(device_scratch));
 if(cudaPeekAtLastError()!=cudaSuccess)return false;
 std::vector<float4> blocks(tile_count);
 if(cudaMemcpyAsync(blocks.data(),device_scratch,bytes,
                    cudaMemcpyDeviceToHost,stream)!=cudaSuccess)return false;
 // The bounded metadata transfer is the sole detector sync. CUDA tracing
 // immediately follows on this SAME host stream. No RGBA image readback.
 if(cudaStreamSynchronize(stream)!=cudaSuccess)return false;
 const int max_sources=std::clamp(s.max_sources,1,2048);
 std::vector<fl::Source> raw;
 raw.reserve(std::min<size_t>(tile_count,4096));
 for(size_t i=0;i<tile_count;i++){
  const float4 v=blocks[i];
  if(v.w<0||!std::isfinite(v.w))continue;
  const int bx=int(i%size_t(dw)),by=int(i/size_t(dw));
  const int x=bx*step,y=by*step;
  const float cx=.5f*(x+std::min(w,x+step));
  const float cy=.5f*(y+std::min(h,y+step));
  raw.push_back({cx/float(w),cy/float(h),
    v.x*s.tint_r,v.y*s.tint_g,v.z*s.tint_b});
 }
 if(raw_sources)*raw_sources=raw;
 result=fl::nuke_select_sources(std::move(raw),w,h,
   s.fov_h_deg,s.fov_v_deg,s.cluster_radius,max_sources);
 return true;
}
}
