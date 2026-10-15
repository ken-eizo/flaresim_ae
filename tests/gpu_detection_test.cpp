// Actual device-resident BGRA128 -> block summaries -> sorted HDR Nuke
// sources. The CPU oracle does not call the GPU implementation.
#include "FlareLensGpuDetection.hpp"
#include <cuda_runtime.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <queue>
#include <vector>

struct Candidate{float score;int index;fl::Source source;};
struct Better{
 bool operator()(const Candidate& x,const Candidate& y)const{
  return x.score>y.score||(x.score==y.score&&x.index<y.index);
 }
};
std::vector<fl::Source> oracle(const std::vector<float4>& image,
 const fl::gpu::DetectionSettings& c){
 const int w=c.width,h=c.height,st=c.scan_step,dw=std::max(1,w/st),dh=std::max(1,h/st);
 std::priority_queue<Candidate,std::vector<Candidate>,Better> best;
 for(int by=0;by<dh;by++)for(int bx=0;bx<dw;bx++){
  const int left=bx*st,top=by*st;
  const int right=bx==dw-1?w:std::min(w,left+st);
  const int bottom=by==dh-1?h:std::min(h,top+st);
  double red=0,green=0,blue=0,alpha=0;int n=0;
  for(int y=top;y<bottom;y++)for(int x=left;x<right;x++){
   const auto& p=image[size_t(y)*w+x];
   if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z))continue;
   red+=p.z;green+=p.y;blue+=p.x;
   alpha+=c.gate_alpha?std::clamp(double(p.w),0.,1.):1.;
   n++;
  }
  if(!n)continue;
  float r=float(red/n),g=float(green/n),b=float(blue/n);
  float score=.2126f*r+.7152f*g+.0722f*b;
  if(c.source_cap>0&&score>c.source_cap){
   const float mul=c.source_cap/score;
   r*=mul;g*=mul;b*=mul;score=c.source_cap;
  }
  const float mask=float(alpha/n);
  r*=mask;g*=mask;b*=mask;score*=mask;
  if(score<c.source_threshold||!std::isfinite(score))continue;
  const float cx=.5f*(left+std::min(w,left+st));
  const float cy=.5f*(top+std::min(h,top+st));
  Candidate cand{score,by*dw+bx,
    {cx/w,cy/h,r*c.tint_r,g*c.tint_g,b*c.tint_b}};
  if(int(best.size())<c.max_sources)best.push(cand);
  else if(Better{}(cand,best.top())){best.pop();best.push(cand);}
 }
 std::vector<Candidate> ranked;
 while(!best.empty()){ranked.push_back(best.top());best.pop();}
 std::sort(ranked.begin(),ranked.end(),Better{});
 std::vector<fl::Source> result;
 for(const auto& item:ranked)result.push_back(item.source);
 return result;
}
int main(){
 int count=0;
 if(cudaGetDeviceCount(&count)!=cudaSuccess||count==0)return 77;
 cudaStream_t stream{};
 if(cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking)!=cudaSuccess)return 1;
 for(int run=0;run<5;run++){
  const int w=run==4?1920:127,h=run==4?1080:91;
  const int step=run==3?3:4;
  std::vector<float4> image(size_t(w)*h,make_float4(0,0,0,1));
  const int dw=std::max(1,w/step),dh=std::max(1,h/step);
  for(int by=0;by<dh;by++)for(int bx=0;bx<dw;bx++){
   if(run!=4 && (by*17+bx*11)%9)continue;
   const float r=float((1+bx%13)*3),g=float((1+by%11)*3),b=float((bx+by)%5+1);
   const float alpha=(run==1||run==3)?float((bx+by)%4)/3.f:1.f;
   const int end_x=bx==dw-1?w:std::min(w,(bx+1)*step);
   const int end_y=by==dh-1?h:std::min(h,(by+1)*step);
   for(int y=by*step;y<end_y;y++)
    for(int x=bx*step;x<end_x;x++)
      image[size_t(y)*w+x]=make_float4(b,g,r,alpha);
  }
  fl::gpu::DetectionSettings settings{};
  settings.width=w;settings.height=h;
  settings.scan_step=step;settings.max_sources=run==4?512:128;
  settings.source_threshold=run==2?22.f:2.f;
  settings.source_cap=run==2?35.f:0.f;
  settings.gate_alpha=(run==1||run==3);
  settings.tint_r=.8f;settings.tint_g=.6f;settings.tint_b=1.f;
  const auto reference=oracle(image,settings);
  void* device_input=nullptr,*device_scratch=nullptr;
  size_t pitch=0;
  if(cudaMallocPitch(&device_input,&pitch,w*16,h)!=cudaSuccess)return 2;
  const size_t size=fl::gpu::detection_scratch_bytes(w,h,step);
  if(!size||cudaMalloc(&device_scratch,size+128)!=cudaSuccess)return 3;
  if(cudaMemcpy2DAsync(device_input,pitch,image.data(),w*16,w*16,h,
                       cudaMemcpyHostToDevice,stream)!=cudaSuccess)return 4;
  if(cudaMemsetAsync(device_scratch,0xA5,size+128,stream)!=cudaSuccess)return 5;
  settings.input_pitch_bytes=int(pitch);
  std::vector<fl::Source> detected;
  const auto start=std::chrono::steady_clock::now();
  if(!fl::gpu::detect_sources_cuda(device_input,device_scratch,size,
          reinterpret_cast<void*>(stream),settings,detected))return 6;
  const auto millis=std::chrono::duration<double,std::milli>(
     std::chrono::steady_clock::now()-start).count();
  if(detected.size()!=reference.size()){
   std::cerr<<"source count error "<<run<<" "<<detected.size()
            <<" vs "<<reference.size()<<"\n";return 7;
  }
  float err=0;
  for(size_t i=0;i<reference.size();i++){
   const auto& a=detected[i],&b=reference[i];
   err=std::max(err,std::abs(a.x-b.x));
   err=std::max(err,std::abs(a.y-b.y));
   err=std::max(err,std::abs(a.r-b.r));
   err=std::max(err,std::abs(a.g-b.g));
   err=std::max(err,std::abs(a.b-b.b));
  }
  std::array<unsigned char,128> guard{};
  if(cudaMemcpy(guard.data(),static_cast<unsigned char*>(device_scratch)+size,
      128,cudaMemcpyDeviceToHost)!=cudaSuccess)return 8;
  for(const auto v:guard)if(v!=0xA5)return 9;
  std::cout<<"GPU_SOURCE_NUKE_BLOCK_PARITY case="<<run<<" input="
    <<w<<"x"<<h<<" candidates="<<size/16<<" sources="<<detected.size()
    <<" max_abs_error="<<err<<" detector_ms="<<millis
    <<" transfer_MB="<<double(size)/1048576<<"\n";
  if(err>1.e-4f)return 10;
  cudaFree(device_scratch);cudaFree(device_input);
 }
 cudaStreamDestroy(stream);
 std::cout<<"ALL_GPU_SOURCE_DETECTION_PARITY_PASS\n";
 return 0;
}
