#include "FlareLensGpuDetection.hpp"
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
 std::vector<fl::Source>& result){
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
 const int candidate_limit=s.cluster_radius>0?
    std::min(8192,max_sources*4):max_sources;
 std::priority_queue<Candidate,std::vector<Candidate>,Better> best;
 for(size_t i=0;i<tile_count;i++){
  const float4 v=blocks[i];
  if(v.w<0||!std::isfinite(v.w))continue;
  const int bx=int(i%size_t(dw)),by=int(i/size_t(dw));
  const int x=bx*step,y=by*step;
  const float cx=.5f*(x+std::min(w,x+step));
  const float cy=.5f*(y+std::min(h,y+step));
  Candidate candidate{v.w,int(i),{cx/float(w),cy/float(h),
   v.x*s.tint_r,v.y*s.tint_g,v.z*s.tint_b}};
  if(int(best.size())<candidate_limit)best.push(candidate);
  else if(Better{}(candidate,best.top())){best.pop();best.push(candidate);}
 }
 std::vector<Candidate> ranked;
 ranked.reserve(best.size());
 while(!best.empty()){ranked.push_back(best.top());best.pop();}
 std::sort(ranked.begin(),ranked.end(),Better{});
 result.reserve(ranked.size());
 for(const auto& candidate:ranked)result.push_back(candidate.source);
 if(s.cluster_radius>0&&result.size()>1){
  const float r2=float(s.cluster_radius)*s.cluster_radius;
  std::vector<fl::Source> clustered;
  std::vector<uint8_t> consumed(result.size(),0);
  clustered.reserve(result.size());
  for(size_t i=0;i<result.size();i++){
   if(consumed[i])continue;
   auto source=result[i];
   float weight=std::max(0.f,source_luma(source));
   float cx=source.x*weight,cy=source.y*weight;
   for(size_t j=i+1;j<result.size();j++){
    if(consumed[j])continue;
    const float dx=(result[j].x-result[i].x)*w;
    const float dy=(result[j].y-result[i].y)*h;
    if(dx*dx+dy*dy>r2)continue;
    const float wj=std::max(0.f,source_luma(result[j]));
    weight+=wj;
    cx+=result[j].x*wj;cy+=result[j].y*wj;
    source.r+=result[j].r;
    source.g+=result[j].g;
    source.b+=result[j].b;
    consumed[j]=1;
   }
   if(weight>0){source.x=cx/weight;source.y=cy/weight;}
   clustered.push_back(source);
  }
  result.swap(clustered);
  if(result.size()>size_t(max_sources)){
   std::stable_sort(result.begin(),result.end(),
    [](const auto& a,const auto& b){return source_luma(a)>source_luma(b);});
   result.resize(max_sources);
  }
 }
 return true;
}
}
