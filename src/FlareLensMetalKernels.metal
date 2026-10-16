// Metal optical backend for FlareSimAE. The ray physics is generated from
// FlareLensGpu.cu, preserving Fresnel, dispersion, coating and 2x2 splat.
#include "FlareLensMetalPhysics.metal"
struct FlPair { int surf_a,surf_b; float area_boost; };
struct FlSource { float angle_x,angle_y,r,g,b,dir_x,dir_y,dir_z; };
struct FlSample { float u,v; };
struct FlBand { float lambda,rw,gw,bw; };
struct FlTrace {
 uint width,height,pitch,surface_count;
 uint pair_count,source_count,pupil_count,band_count;
 uint sample_offset,work_count;
 float sensor_z,front_radius,start_z,half_w,half_h,gain,ray_weight;
};
struct FlImage { uint width,height,input_pitch,output_pitch,mode; float gain; };
struct FlBlur {uint width,height,radius;};
struct FlHaze {uint width,height,pitch,source_count;float gain;};
struct FlDetect {uint width,height,pitch,step,dw,dh;float threshold,cap;uint gate;};
struct FlSeed {int x,y;float b,g,r;};
struct FlSeedInfo {uint width,height,footprint,count;};
inline void fl_atomic_add(device atomic_uint* p,float amount){
 if(!(amount>1.e-14f)||!isfinite(amount))return;
 uint old=atomic_load_explicit(p,memory_order_relaxed);
 while(!atomic_compare_exchange_weak_explicit(
    p,&old,as_type<uint>(as_type<float>(old)+amount),
    memory_order_relaxed,memory_order_relaxed)){}
}
kernel void fl_trace(
 device const Surface* surfs [[buffer(0)]],
 device const FlPair* pairs [[buffer(1)]],
 device const FlSource* sources [[buffer(2)]],
 device const FlSample* pupil [[buffer(3)]],
 device const FlBand* bands [[buffer(4)]],
 device atomic_uint* output [[buffer(5)]],
 constant FlTrace& a [[buffer(6)]],
 uint tid [[thread_position_in_grid]]){
 if(tid>=a.work_count)return;
 uint n=a.sample_offset+tid;
 const uint q=n%a.band_count;n/=a.band_count;
 const uint pi_sample=n%a.pupil_count;n/=a.pupil_count;
 const uint si=n%a.source_count;
 const uint pair_index=n/a.source_count;
 if(pair_index>=a.pair_count)return;
 const FlPair pair=pairs[pair_index];
 if(pair.surf_a<0||pair.surf_b<=pair.surf_a||
    uint(pair.surf_b)>=a.surface_count)return;
 const FlSource src=sources[si];
 const FlSample sample=pupil[pi_sample];
 const DRay ray={dv(sample.u*a.front_radius,
                    sample.v*a.front_radius,a.start_z),
                 dv(src.dir_x,src.dir_y,src.dir_z)};
 const FlBand sp=bands[q];
 const DTraceResult hit=d_trace_ghost_ray(ray,surfs,int(a.surface_count),
                      a.sensor_z,pair.surf_a,pair.surf_b,sp.lambda);
 if(!hit.valid||!isfinite(hit.position.x)||!isfinite(hit.position.y))return;
 const float base=hit.weight*a.ray_weight*a.gain*pair.area_boost;
 if(base<1.e-14f)return;
 float px=(hit.position.x/(2.f*a.half_w)+.5f)*float(a.width);
 float py=(hit.position.y/(2.f*a.half_h)+.5f)*float(a.height);
 if(!isfinite(px)||!isfinite(py))return;
 px=clamp(px,-2.e9f,2.e9f);py=clamp(py,-2.e9f,2.e9f);
 const int x=int(floor(px-.5f)),y=int(floor(py-.5f));
 const float fx=px-.5f-float(x),fy=py-.5f-float(y);
 const float3 energy=float3(src.b*sp.bw,src.g*sp.gw,src.r*sp.rw)*base;
 for(int dy=0;dy<2;dy++){
  const int yy=y+dy;if(yy<0||yy>=int(a.height))continue;
  for(int dx=0;dx<2;dx++){
   const int xx=x+dx;if(xx<0||xx>=int(a.width))continue;
   const float wt=(dy?fy:1.f-fy)*(dx?fx:1.f-fx);
   const uint at=(uint(yy)*a.pitch+uint(xx))*4;
   fl_atomic_add(output+at,energy.x*wt);
   fl_atomic_add(output+at+1,energy.y*wt);
   fl_atomic_add(output+at+2,energy.z*wt);
  }
 }
}
kernel void fl_plate(
 device const float4* input [[buffer(0)]],
 device float4* output [[buffer(1)]],
 constant FlImage& a [[buffer(2)]],
 uint tid [[thread_position_in_grid]]){
 if(tid>=a.width*a.height)return;
 const uint x=tid%a.width,y=tid/a.width;
 const float4 in=input[y*a.input_pitch+x];
 output[y*a.output_pitch+x]=a.mode==1?in:float4(0,0,0,1);
}
// Source preview is O(pixels + sources*121), NOT O(pixels*sources).
kernel void fl_preview_base(
 device const float4* input [[buffer(0)]],
 device float4* output [[buffer(1)]],
 constant FlImage& a [[buffer(2)]],
 uint tid [[thread_position_in_grid]]){
 if(tid>=a.width*a.height)return;
 const uint x=tid%a.width,y=tid/a.width;
 float4 v=input[y*a.input_pitch+x]*.12f;
 v.w=1.f;
 output[y*a.output_pitch+x]=v;
}
kernel void fl_preview_mark(
 device const float4* input [[buffer(0)]],
 device atomic_uint* output [[buffer(1)]],
 device const FlSource* sources [[buffer(2)]],
 constant FlImage& a [[buffer(3)]],
 uint tid [[thread_position_in_grid]]){
 if(tid>=a.mode*121u)return;
 const uint index=tid/121u,p=tid%121u;
 const int dx=int(p%11u)-5,dy=int(p/11u)-5;
 const int r2=dx*dx+dy*dy;
 if(!((r2>=9&&r2<=25)||(dx==0&&r2<=25)||
      (dy==0&&r2<=25)))return;
 const int x=int(floor(sources[index].angle_x*a.width))+dx;
 const int y=int(floor(sources[index].angle_y*a.height))+dy;
 if(x<0||y<0||x>=int(a.width)||y>=int(a.height))return;
 const uint src_pos=uint(y)*a.input_pitch+uint(x);
 const uint dst_pos=(uint(y)*a.output_pitch+uint(x))*4;
 const float4 in=input[src_pos];
 atomic_store_explicit(output+dst_pos,
   as_type<uint>(in.x*.12f+1.f),memory_order_relaxed);
 atomic_store_explicit(output+dst_pos+1,
   as_type<uint>(in.y*.12f+1.f),memory_order_relaxed);
 atomic_store_explicit(output+dst_pos+2,
   as_type<uint>(in.z*.12f+1.f),memory_order_relaxed);
}
kernel void fl_add(
 device const float4* photon [[buffer(0)]],
 device float4* output [[buffer(1)]],
 constant FlImage& a [[buffer(2)]],
 uint tid [[thread_position_in_grid]]){
 if(tid>=a.width*a.height)return;
 const uint x=tid%a.width,y=tid/a.width;
 float4 v=output[y*a.output_pitch+x];
 v.xyz+=photon[tid].xyz*a.gain;
 output[y*a.output_pitch+x]=v;
}
kernel void fl_alpha(
 device const float4* input [[buffer(0)]],
 device float4* output [[buffer(1)]],
 constant FlImage& a [[buffer(2)]],
 uint tid [[thread_position_in_grid]]){
 if(tid>=a.width*a.height)return;
 const uint x=tid%a.width,y=tid/a.width;
 const float4 in=input[y*a.input_pitch+x];
 float4 out=output[y*a.output_pitch+x];
 const float3 difference=max(float3(0),out.xyz-in.xyz);
 const float coverage=clamp(max(difference.x,max(difference.y,difference.z)),0.f,1.f);
 const float alpha=isfinite(in.w)?clamp(in.w,0.f,1.f):0.f;
 out.w=alpha+(1.f-alpha)*coverage;
 output[y*a.output_pitch+x]=out;
}
kernel void fl_blur_x(
 device const float4* input [[buffer(0)]],
 device float4* output [[buffer(1)]],
 constant FlBlur& a [[buffer(2)]],
 uint tid [[thread_position_in_grid]]){
 if(tid>=a.width*a.height)return;
 const int x=int(tid%a.width),y=int(tid/a.width);
 const int first=max(0,x-int(a.radius)),last=min(int(a.width)-1,x+int(a.radius));
 float3 sum=0.f;
 for(int xx=first;xx<=last;xx++)sum+=input[uint(y)*a.width+uint(xx)].xyz;
 output[tid]=float4(sum/float(last-first+1),0);
}
kernel void fl_blur_y(
 device const float4* input [[buffer(0)]],
 device float4* output [[buffer(1)]],
 constant FlBlur& a [[buffer(2)]],
 uint tid [[thread_position_in_grid]]){
 if(tid>=a.width*a.height)return;
 const int x=int(tid%a.width),y=int(tid/a.width);
 const int first=max(0,y-int(a.radius)),last=min(int(a.height)-1,y+int(a.radius));
 float3 sum=0.f;
 for(int yy=first;yy<=last;yy++)sum+=input[uint(yy)*a.width+uint(x)].xyz;
 output[tid]=float4(sum/float(last-first+1),0);
}
kernel void fl_blur_x_wide(
 device const float4* input [[buffer(0)]],
 device float4* output [[buffer(1)]],
 constant FlBlur& a [[buffer(2)]],
 uint tid [[thread_position_in_grid]]){
 const uint segments=(a.width+127)/128;
 const uint y=tid/segments,begin=(tid%segments)*128;
 if(y>=a.height)return;
 const int radius=int(a.radius),end=int(min(begin+128,a.width));
 int first=max(0,int(begin)-radius),last=min(int(a.width)-1,int(begin)+radius);
 float3 sum=0.f;
 for(int xx=first;xx<=last;xx++)sum+=input[y*a.width+uint(xx)].xyz;
 for(int x=int(begin);x<end;x++){
  const int denom=min(int(a.width)-1,x+radius)-max(0,x-radius)+1;
  output[y*a.width+uint(x)]=float4(sum/float(denom),0);
  const int remove=x-radius,add=x+radius+1;
  if(remove>=0)sum-=input[y*a.width+uint(remove)].xyz;
  if(add<int(a.width))sum+=input[y*a.width+uint(add)].xyz;
 }
}
kernel void fl_blur_y_wide(
 device const float4* input [[buffer(0)]],
 device float4* output [[buffer(1)]],
 constant FlBlur& a [[buffer(2)]],
 uint tid [[thread_position_in_grid]]){
 const uint segments=(a.height+127)/128;
 const uint x=tid/segments,begin=(tid%segments)*128;
 if(x>=a.width)return;
 const int radius=int(a.radius),end=int(min(begin+128,a.height));
 int first=max(0,int(begin)-radius),last=min(int(a.height)-1,int(begin)+radius);
 float3 sum=0.f;
 for(int yy=first;yy<=last;yy++)sum+=input[uint(yy)*a.width+x].xyz;
 for(int y=int(begin);y<end;y++){
  const int denom=min(int(a.height)-1,y+radius)-max(0,y-radius)+1;
  output[uint(y)*a.width+x]=float4(sum/float(denom),0);
  const int remove=y-radius,add=y+radius+1;
  if(remove>=0)sum-=input[uint(remove)*a.width+x].xyz;
  if(add<int(a.height))sum+=input[uint(add)*a.width+x].xyz;
 }
}
kernel void fl_seed_haze(
 device const FlSeed* seeds [[buffer(0)]],
 device atomic_uint* photon [[buffer(1)]],
 constant FlSeedInfo& a [[buffer(2)]],
 uint tid [[thread_position_in_grid]]){
 if(tid>=a.count)return;
 const FlSeed s=seeds[tid];
 if(s.x<0||s.y<0||s.x>=int(a.width)||s.y>=int(a.height))return;
 const int f=int(clamp(a.footprint,1u,32u));
 for(int y=max(0,s.y-f/2);y<min(int(a.height),s.y+(f+1)/2);y++)
 for(int x=max(0,s.x-f/2);x<min(int(a.width),s.x+(f+1)/2);x++){
  const uint i=(uint(y)*a.width+uint(x))*4;
  fl_atomic_add(photon+i,s.b);
  fl_atomic_add(photon+i+1,s.g);
  fl_atomic_add(photon+i+2,s.r);
 }
}
kernel void fl_analytic_haze(
 device const FlSource* sources [[buffer(0)]],
 device const float* weights_x [[buffer(1)]],
 device const float* weights_y [[buffer(2)]],
 device float4* output [[buffer(3)]],
 constant FlHaze& a [[buffer(4)]],
 uint tid [[thread_position_in_grid]]){
 if(tid>=a.width*a.height)return;
 const uint x=tid%a.width,y=tid/a.width;
 float3 sum=0;
 for(uint s=0;s<a.source_count;s++){
  const float weight=weights_x[s*a.width+x]*weights_y[s*a.height+y]*a.gain;
  sum+=float3(sources[s].b,sources[s].g,sources[s].r)*weight;
 }
 float4 out=output[y*a.pitch+x];out.xyz+=sum;
 output[y*a.pitch+x]=out;
}
kernel void fl_detect_tiles(
 device const float4* input [[buffer(0)]],
 device float4* tiles [[buffer(1)]],
 constant FlDetect& a [[buffer(2)]],
 uint tid [[thread_position_in_grid]]){
 if(tid>=a.dw*a.dh)return;
 const uint bx=tid%a.dw,by=tid/a.dw;
 const uint left=bx*a.step,top=by*a.step;
 const uint right=bx+1==a.dw?a.width:min(a.width,left+a.step);
 const uint bottom=by+1==a.dh?a.height:min(a.height,top+a.step);
 float4 accum=0;
 uint n=0;
 for(uint y=top;y<bottom;y++)for(uint x=left;x<right;x++){
  const float4 v=input[y*a.pitch+x];
  if(!all(isfinite(v.xyz)))continue;
  accum+=float4(v.z,v.y,v.x,a.gate?clamp(v.w,0.f,1.f):1.f);
  n++;
 }
 float4 result=float4(0,0,0,-1);
 if(n){
  const float3 rgb=accum.xyz/float(n);
  float score=dot(rgb,float3(.2126f,.7152f,.0722f));
  float3 value=rgb;
  if(a.cap>0&&score>a.cap){value*=a.cap/score;score=a.cap;}
  const float mask=accum.w/float(n);
  value*=mask;score*=mask;
  if(isfinite(score)&&score>=a.threshold)
   result=float4(value,score);
 }
 tiles[tid]=result;
}
