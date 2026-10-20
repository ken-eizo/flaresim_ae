// AVX2 version of fl::cpu::detail::trace(): 8 pupil samples per instruction.
// Every arithmetic step keeps the scalar operand order (no FMA, no
// reassociation), comparisons use the same NaN behaviour as the scalar
// branches, and lanes that the scalar code would have returned from are
// masked off. Only cos() in the thin-film term is a vector polynomial.
// The batch functions advance two batches of 8 together: each batch's surface
// events form one long dependency chain, and stepping two independent chains
// side by side lets the out-of-order core overlap them. Per lane the
// arithmetic is unchanged.
// Compiled with /arch:AVX2; callers must check avx2_supported() first.
#include "FlareLensCpuTrace.hpp"
#include <immintrin.h>
#include <cstring>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace fl::cpu {
namespace {
using V=__m256;
inline V set(float x){return _mm256_set1_ps(x);}
inline V add(V a,V b){return _mm256_add_ps(a,b);}
inline V sub(V a,V b){return _mm256_sub_ps(a,b);}
inline V mul(V a,V b){return _mm256_mul_ps(a,b);}
inline V div(V a,V b){return _mm256_div_ps(a,b);}
inline V vabs(V a){return _mm256_andnot_ps(set(-0.0f),a);}
inline V neg(V a){return _mm256_xor_ps(a,set(-0.0f));}
inline V lt(V a,V b){return _mm256_cmp_ps(a,b,_CMP_LT_OQ);}
inline V gt(V a,V b){return _mm256_cmp_ps(a,b,_CMP_GT_OQ);}
inline V ge(V a,V b){return _mm256_cmp_ps(a,b,_CMP_GE_OQ);}
inline V sel(V mask,V yes,V no){return _mm256_blendv_ps(no,yes,mask);}
inline V vand(V a,V b){return _mm256_and_ps(a,b);}
inline V vandnot(V a,V b){return _mm256_andnot_ps(a,b);} // ~a & b
inline V vor(V a,V b){return _mm256_or_ps(a,b);}
// Local helpers only: an inline function with external linkage compiled
// here with AVX2 encoding could be picked by the linker for SSE2 callers.
inline float abs_scalar(float x){return _mm_cvtss_f32(_mm_andnot_ps(_mm_set1_ps(-0.0f),_mm_set_ss(x)));}
// std::clamp(v,lo,hi): v<lo ? lo : (hi<v ? hi : v); NaN passes through.
inline V clamp01(V v){return sel(lt(v,set(0.f)),set(0.f),sel(lt(set(1.f),v),set(1.f),v));}

// Cephes cosf: |x|<8192, about 1 ulp from std::cos.
V vcos(V x){
 x=vabs(x);
 __m256i j=_mm256_cvttps_epi32(mul(x,set(1.27323954473516f)));
 j=_mm256_add_epi32(j,_mm256_set1_epi32(1));
 j=_mm256_and_si256(j,_mm256_set1_epi32(~1));
 V y=_mm256_cvtepi32_ps(j);
 j=_mm256_sub_epi32(j,_mm256_set1_epi32(2));
 const V sign=_mm256_castsi256_ps(_mm256_slli_epi32(
   _mm256_andnot_si256(j,_mm256_set1_epi32(4)),29));
 const V poly_sin=_mm256_castsi256_ps(_mm256_cmpeq_epi32(
   _mm256_and_si256(j,_mm256_set1_epi32(2)),_mm256_setzero_si256()));
 x=sub(x,mul(y,set(0.78515625f)));
 x=sub(x,mul(y,set(2.4187564849853515625e-4f)));
 x=sub(x,mul(y,set(3.77489497744594108e-8f)));
 const V z=mul(x,x);
 V c=add(mul(set(2.443315711809948e-5f),z),set(-1.388731625493765e-3f));
 c=add(mul(c,z),set(4.166664568298827e-2f));
 c=mul(mul(c,z),z);
 c=add(sub(c,mul(set(.5f),z)),set(1.f));
 V s=add(mul(set(-1.9515295891e-4f),z),set(8.3321608736e-3f));
 s=add(mul(s,z),set(-1.6666654611e-1f));
 s=add(mul(mul(s,z),x),x);
 return _mm256_xor_ps(sel(poly_sin,s,c),sign);
}

struct Lanes {V ox,oy,oz,dx,dy,dz,weight,alive;};

// fresnel_reflectance() with uniform n1, n2.
V fresnel(V cos_i,float n1,float n2){
 cos_i=vabs(cos_i);
 const float eta=n1/n2;
 const V sin2_t=mul(set(eta*eta),sub(set(1.0f),mul(cos_i,cos_i)));
 const V tir=ge(sin2_t,set(1.0f));
 const V cos_t=_mm256_sqrt_ps(sub(set(1.0f),sin2_t));
 const V a=mul(set(n1),cos_i),b=mul(set(n2),cos_t);
 const V rs=div(sub(a,b),add(a,b));
 const V c=mul(set(n2),cos_i),d=mul(set(n1),cos_t);
 const V rp=div(sub(c,d),add(c,d));
 const V r=mul(set(0.5f),add(mul(rs,rs),mul(rp,rp)));
 return sel(tir,set(1.0f),r);
}
// surface_reflectance() from fresnel.h, lane-parallel.
V reflectance(V cos_i,float n1,float n2,int layers,float lambda){
 if(layers<=0)return fresnel(cos_i,n1,n2);
 constexpr float coating_n=1.38f;
 constexpr float design_lambda=550.0f;
 const float d_nm=design_lambda/(4.0f*coating_n);
 const V plain=fresnel(cos_i,n1,n2);
 const float k1=n1/coating_n;
 const V sin2_c=mul(set(k1*k1),sub(set(1.0f),mul(cos_i,cos_i)));
 const V fall1=ge(sin2_c,set(1.0f));
 const V cos_c=_mm256_sqrt_ps(sub(set(1.0f),sin2_c));
 const float phase=2.0f*(float)M_PI*coating_n*d_nm;
 const V delta=div(mul(set(phase),cos_c),set(lambda));
 const V a=mul(set(n1),cos_i),b=mul(set(coating_n),cos_c);
 const V r01=div(sub(a,b),add(a,b));
 const float k2=coating_n/n2;
 const V sin2_2=mul(set(k2*k2),sub(set(1.0f),mul(cos_c,cos_c)));
 const V fall2=ge(sin2_2,set(1.0f));
 const V cos_2=_mm256_sqrt_ps(sub(set(1.0f),sin2_2));
 const V c=mul(set(coating_n),cos_c),d=mul(set(n2),cos_2);
 const V r12=div(sub(c,d),add(c,d));
 const V cos_2delta=vcos(mul(set(2.0f),delta));
 const V cross=mul(mul(mul(set(2.0f),r01),r12),cos_2delta);
 const V num=add(add(mul(r01,r01),mul(r12,r12)),cross);
 const V den=add(add(set(1.0f),mul(mul(mul(r01,r01),r12),r12)),cross);
 V R=clamp01(div(num,den));
 R=sel(vor(fall1,fall2),plain,R);
 for(int i=1;i<layers;++i)R=mul(R,set(0.25f));
 return clamp01(R);
}
// intersect_surface(): updates origin, returns the normal, clears lanes
// that miss the surface or its aperture.
void intersect(Lanes& L,const Surface& surf,V& nx,V& ny,V& nz){
 V t;
 if(abs_scalar(surf.radius)<1e-6f){
  L.alive=vandnot(lt(vabs(L.dz),set(1e-12f)),L.alive);
  t=div(sub(set(surf.z),L.oz),L.dz);
  L.alive=vandnot(lt(t,set(1e-6f)),L.alive);
  const V hx=add(L.ox,mul(L.dx,t)),hy=add(L.oy,mul(L.dy,t)),hz=add(L.oz,mul(L.dz,t));
  const V h2=add(mul(hx,hx),mul(hy,hy));
  L.alive=vandnot(gt(h2,set(surf.semi_aperture*surf.semi_aperture)),L.alive);
  nx=set(0.f);ny=set(0.f);
  nz=sel(gt(L.dz,set(0.f)),set(-1.0f),set(1.0f));
  L.ox=hx;L.oy=hy;L.oz=hz;
  return;
 }
 const float R=surf.radius;
 const float cz=surf.z+R;
 const V ocx=sub(L.ox,set(0.f)),ocy=sub(L.oy,set(0.f)),ocz=sub(L.oz,set(cz));
 const V a=add(add(mul(L.dx,L.dx),mul(L.dy,L.dy)),mul(L.dz,L.dz));
 const V b=mul(set(2.0f),add(add(mul(ocx,L.dx),mul(ocy,L.dy)),mul(ocz,L.dz)));
 const V c=sub(add(add(mul(ocx,ocx),mul(ocy,ocy)),mul(ocz,ocz)),set(R*R));
 const V disc=sub(mul(b,b),mul(mul(set(4.0f),a),c));
 L.alive=vandnot(lt(disc,set(0.f)),L.alive);
 const V sd=_mm256_sqrt_ps(disc);
 const V inv_2a=div(set(0.5f),a);
 const V nb=neg(b);
 const V t1=mul(sub(nb,sd),inv_2a);
 const V t2=mul(add(nb,sd),inv_2a);
 const V p1=gt(t1,set(1e-6f)),p2=gt(t2,set(1e-6f));
 const V z1=add(L.oz,mul(t1,L.dz)),z2=add(L.oz,mul(t2,L.dz));
 const V closer=lt(vabs(sub(z1,set(surf.z))),vabs(sub(z2,set(surf.z))));
 t=sel(vand(p1,p2),sel(closer,t1,t2),sel(p1,t1,t2));
 L.alive=vand(L.alive,vor(p1,p2));
 const V hx=add(L.ox,mul(L.dx,t)),hy=add(L.oy,mul(L.dy,t)),hz=add(L.oz,mul(L.dz,t));
 const V h2=add(mul(hx,hx),mul(hy,hy));
 L.alive=vandnot(gt(h2,set(surf.semi_aperture*surf.semi_aperture)),L.alive);
 const float inv=1.0f/abs_scalar(R);
 nx=mul(sub(hx,set(0.f)),set(inv));
 ny=mul(sub(hy,set(0.f)),set(inv));
 nz=mul(sub(hz,set(cz)),set(inv));
 const V flip=gt(add(add(mul(nx,L.dx),mul(ny,L.dy)),mul(nz,L.dz)),set(0.f));
 nx=sel(flip,neg(nx),nx);ny=sel(flip,neg(ny),ny);nz=sel(flip,neg(nz),nz);
 L.ox=hx;L.oy=hy;L.oz=hz;
}
void normalize(V& x,V& y,V& z){
 const V l2=add(add(mul(x,x),mul(y,y)),mul(z,z));
 const V inv=div(set(1.0f),_mm256_sqrt_ps(l2));
 x=mul(x,inv);y=mul(y,inv);z=mul(z,inv);
}
void reflect(Lanes& L,V nx,V ny,V nz){
 const V d2=mul(set(2.0f),add(add(mul(L.dx,nx),mul(L.dy,ny)),mul(L.dz,nz)));
 V x=sub(L.dx,mul(nx,d2)),y=sub(L.dy,mul(ny,d2)),z=sub(L.dz,mul(nz,d2));
 normalize(x,y,z);
 L.dx=x;L.dy=y;L.dz=z;
}
void refract(Lanes& L,V nx,V ny,V nz,float ratio){
 const V cos_i=neg(add(add(mul(nx,L.dx),mul(ny,L.dy)),mul(nz,L.dz)));
 const V sin2_t=mul(set(ratio*ratio),sub(set(1.0f),mul(cos_i,cos_i)));
 L.alive=vandnot(ge(sin2_t,set(1.0f)),L.alive);
 const V cos_t=_mm256_sqrt_ps(sub(set(1.0f),sin2_t));
 const V k=sub(mul(set(ratio),cos_i),cos_t);
 V x=add(mul(L.dx,set(ratio)),mul(nx,k));
 V y=add(mul(L.dy,set(ratio)),mul(ny,k));
 V z=add(mul(L.dz,set(ratio)),mul(nz,k));
 normalize(x,y,z);
 L.dx=x;L.dy=y;L.dz=z;
}
// One surface event: intersect, Fresnel weight, then reflect or refract.
bool event(Lanes& L,const Surface& surf,float n1,float n2,bool mirror,float lambda){
 V nx,ny,nz;
 intersect(L,surf,nx,ny,nz);
 if(_mm256_movemask_ps(L.alive)==0)return false;
 const V cos_i=vabs(add(add(mul(nx,L.dx),mul(ny,L.dy)),mul(nz,L.dz)));
 const V R=reflectance(cos_i,n1,n2,surf.coating,lambda);
 if(mirror){
  reflect(L,nx,ny,nz);
  L.weight=mul(L.weight,R);
 }else{
  refract(L,nx,ny,nz,n1/n2);
  L.weight=mul(L.weight,sub(set(1.0f),R));
 }
 return _mm256_movemask_ps(L.alive)!=0;
}
Lanes start_lanes(const TraceInput& in,const float* u,const float* v){
 Lanes L;
 L.ox=mul(_mm256_loadu_ps(u),set(in.front_radius));
 L.oy=mul(_mm256_loadu_ps(v),set(in.front_radius));
 L.oz=set(in.start_z);
 L.dx=set(in.dir.x);L.dy=set(in.dir.y);L.dz=set(in.dir.z);
 L.weight=set(1.0f);
 L.alive=_mm256_castsi256_ps(_mm256_set1_epi32(-1));
 return L;
}
// Forward 0..b, reflecting at b. Returns false once every lane is dead.
bool to_second_bounce(const TraceInput& in,Lanes& L,float& current){
 bool any=true;
 for(int s=0;any&&s<=in.b;++s){
  const float n2=in.ior[s];
  any=event(L,in.surfaces[s],current,n2,s==in.b,in.lambda);
  if(s!=in.b)current=n2;
 }
 return any;
}
bool alive(const Lanes& L){return _mm256_movemask_ps(L.alive)!=0;}
// Forward 0..b for two batches; a dead batch skips its events.
void to_second_bounce(const TraceInput& in,Lanes& A,bool& any_a,Lanes& B,bool& any_b,
                      float& current){
 for(int s=0;(any_a||any_b)&&s<=in.b;++s){
  const float n2=in.ior[s];
  if(any_a)any_a=event(A,in.surfaces[s],current,n2,s==in.b,in.lambda);
  if(any_b)any_b=event(B,in.surfaces[s],current,n2,s==in.b,in.lambda);
  if(s!=in.b)current=n2;
 }
}
// Propagates to the sensor plane and stores the lanes that reach it.
void store_sensor(const TraceInput& in,Lanes& L,bool any,
                  uint8_t* valid,float* x,float* y,float* w,int count){
 alignas(32) float px[8],py[8],pw[8];
 int mask=0;
 if(any){
  L.alive=vandnot(lt(vabs(L.dz),set(1e-12f)),L.alive);
  const V t=div(sub(set(in.sensor_z),L.oz),L.dz);
  L.alive=vandnot(lt(t,set(0.f)),L.alive);
  _mm256_store_ps(px,add(L.ox,mul(L.dx,t)));
  _mm256_store_ps(py,add(L.oy,mul(L.dy,t)));
  _mm256_store_ps(pw,L.weight);
  mask=_mm256_movemask_ps(L.alive);
 }
 for(int k=0;k<count;k++){
  valid[k]=uint8_t((mask>>k)&1);
  x[k]=mask?px[k]:0.f;y[k]=mask?py[k]:0.f;w[k]=mask?pw[k]:0.f;
 }
}
// The last leg: refract through surfaces from..n-1, then the sensor plane.
void to_sensor(const TraceInput& in,Lanes& L,bool any,float current,int from,
               uint8_t* valid,float* x,float* y,float* w,int count){
 for(int s=from;any&&s<in.n;++s){
  const float n2=in.ior[s];
  any=event(L,in.surfaces[s],current,n2,false,in.lambda);
  current=n2;
 }
 store_sensor(in,L,any,valid,x,y,w,count);
}
// to_sensor() for two batches; their results go to *_a and *_b.
void to_sensor(const TraceInput& in,Lanes& A,bool any_a,Lanes& B,bool any_b,
               float current,int from,
               uint8_t* valid_a,float* x_a,float* y_a,float* w_a,int count_a,
               uint8_t* valid_b,float* x_b,float* y_b,float* w_b,int count_b){
 for(int s=from;(any_a||any_b)&&s<in.n;++s){
  const float n2=in.ior[s];
  if(any_a)any_a=event(A,in.surfaces[s],current,n2,false,in.lambda);
  if(any_b)any_b=event(B,in.surfaces[s],current,n2,false,in.lambda);
  current=n2;
 }
 store_sensor(in,A,any_a,valid_a,x_a,y_a,w_a,count_a);
 store_sensor(in,B,any_b,valid_b,x_b,y_b,w_b,count_b);
}
void trace8(const TraceInput& in,const float* u,const float* v,
            uint8_t* valid,float* x,float* y,float* w,int count){
 Lanes L=start_lanes(in,u,v);
 float current=1.0f;
 bool any=to_second_bounce(in,L,current);
 for(int s=in.b-1;any&&s>=in.a;--s){
  const float n2=s<=0?1.0f:in.ior[s-1];
  any=event(L,in.surfaces[s],current,n2,s==in.a,in.lambda);
  current=s==in.a?in.ior[in.a]:n2;
 }
 to_sensor(in,L,any,current,in.a+1,valid,x,y,w,count);
}
// trace8() for every pair of a group: event() split into the shared
// intersection and Fresnel term, then reflect (pair j) or refract (the rest).
void trace8_group(const GroupInput& g,const float* u,const float* v,int stride,
                  uint8_t* valid,float* x,float* y,float* w,int count){
 const TraceInput& in=g.base;
 Lanes L=start_lanes(in,u,v);
 float current=1.0f;
 bool any=to_second_bounce(in,L,current);
 int j=0;
 for(int s=in.b-1;any&&j<g.count;--s){
  const Surface& surf=in.surfaces[s];
  const float n2=s<=0?1.0f:in.ior[s-1];
  V nx,ny,nz;
  intersect(L,surf,nx,ny,nz);
  if(_mm256_movemask_ps(L.alive)==0)break;
  const V cos_i=vabs(add(add(mul(nx,L.dx),mul(ny,L.dy)),mul(nz,L.dz)));
  const V R=reflectance(cos_i,current,n2,surf.coating,in.lambda);
  if(s==g.a[j]){
   Lanes B=L;
   reflect(B,nx,ny,nz);
   B.weight=mul(B.weight,R);
   const size_t o=size_t(j)*stride;
   to_sensor(in,B,true,in.ior[s],s+1,valid+o,x+o,y+o,w+o,count);
   if(++j==g.count)return;
  }
  refract(L,nx,ny,nz,current/n2);
  L.weight=mul(L.weight,sub(set(1.0f),R));
  current=n2;
  any=_mm256_movemask_ps(L.alive)!=0;
 }
 for(;j<g.count;++j){
  const size_t o=size_t(j)*stride;
  for(int k=0;k<count;k++){valid[o+k]=0;x[o+k]=0.f;y[o+k]=0.f;w[o+k]=0.f;}
 }
}
// trace8() for 9..16 samples: lanes 0-7 in batch A, 8-15 in batch B.
void trace16(const TraceInput& in,const float* u,const float* v,
             uint8_t* valid,float* x,float* y,float* w,int count){
 Lanes A=start_lanes(in,u,v),B=start_lanes(in,u+8,v+8);
 float current=1.0f;
 bool any_a=true,any_b=true;
 to_second_bounce(in,A,any_a,B,any_b,current);
 for(int s=in.b-1;(any_a||any_b)&&s>=in.a;--s){
  const float n2=s<=0?1.0f:in.ior[s-1];
  if(any_a)any_a=event(A,in.surfaces[s],current,n2,s==in.a,in.lambda);
  if(any_b)any_b=event(B,in.surfaces[s],current,n2,s==in.a,in.lambda);
  current=s==in.a?in.ior[in.a]:n2;
 }
 to_sensor(in,A,any_a,B,any_b,current,in.a+1,valid,x,y,w,8,
           valid+8,x+8,y+8,w+8,count-8);
}
// trace8_group() for 9..16 samples, as trace16().
void trace16_group(const GroupInput& g,const float* u,const float* v,int stride,
                   uint8_t* valid,float* x,float* y,float* w,int count){
 const TraceInput& in=g.base;
 Lanes A=start_lanes(in,u,v),B=start_lanes(in,u+8,v+8);
 float current=1.0f;
 bool any_a=true,any_b=true;
 to_second_bounce(in,A,any_a,B,any_b,current);
 const V zero=_mm256_setzero_ps();
 int j=0;
 for(int s=in.b-1;(any_a||any_b)&&j<g.count;--s){
  const Surface& surf=in.surfaces[s];
  const float n2=s<=0?1.0f:in.ior[s-1];
  V nxa=zero,nya=zero,nza=zero,ra=zero,nxb=zero,nyb=zero,nzb=zero,rb=zero;
  if(any_a){intersect(A,surf,nxa,nya,nza);any_a=alive(A);}
  if(any_b){intersect(B,surf,nxb,nyb,nzb);any_b=alive(B);}
  if(!any_a&&!any_b)break;
  if(any_a)ra=reflectance(vabs(add(add(mul(nxa,A.dx),mul(nya,A.dy)),mul(nza,A.dz))),
                          current,n2,surf.coating,in.lambda);
  if(any_b)rb=reflectance(vabs(add(add(mul(nxb,B.dx),mul(nyb,B.dy)),mul(nzb,B.dz))),
                          current,n2,surf.coating,in.lambda);
  if(s==g.a[j]){
   Lanes ba=A,bb=B;
   if(any_a){reflect(ba,nxa,nya,nza);ba.weight=mul(ba.weight,ra);}
   if(any_b){reflect(bb,nxb,nyb,nzb);bb.weight=mul(bb.weight,rb);}
   const size_t o=size_t(j)*stride;
   to_sensor(in,ba,any_a,bb,any_b,in.ior[s],s+1,valid+o,x+o,y+o,w+o,8,
             valid+o+8,x+o+8,y+o+8,w+o+8,count-8);
   if(++j==g.count)return;
  }
  if(any_a){
   refract(A,nxa,nya,nza,current/n2);
   A.weight=mul(A.weight,sub(set(1.0f),ra));
   any_a=alive(A);
  }
  if(any_b){
   refract(B,nxb,nyb,nzb,current/n2);
   B.weight=mul(B.weight,sub(set(1.0f),rb));
   any_b=alive(B);
  }
  current=n2;
 }
 for(;j<g.count;++j){
  const size_t o=size_t(j)*stride;
  for(int k=0;k<count;k++){valid[o+k]=0;x[o+k]=0.f;y[o+k]=0.f;w[o+k]=0.f;}
 }
}
// Loads samples [k,k+n) into u/v (padded with sample k) for trace8/16.
void load_samples(const PupilSample* samples,int k,int n,int width,float* u,float* v){
 for(int i=0;i<width;i++){
  const int src=k+(i<n?i:0);
  u[i]=samples[src].u;v[i]=samples[src].v;
 }
}
} // namespace

bool avx2_supported() noexcept {
#if defined(_MSC_VER)
 int info[4]{};
 __cpuid(info,0);
 if(info[0]<7)return false;
 __cpuid(info,1);
 const bool osxsave=(info[2]&(1<<27))!=0,avx=(info[2]&(1<<28))!=0;
 if(!osxsave||!avx)return false;
 if((_xgetbv(0)&6)!=6)return false; // XMM and YMM state saved by the OS
 __cpuidex(info,7,0);
 return (info[1]&(1<<5))!=0;          // AVX2
#else
 return __builtin_cpu_supports("avx2");
#endif
}

void trace_batch_avx2(const TraceInput& in,const PupilSample* samples,int count,
                      uint8_t* valid,float* x,float* y,float* w){
 alignas(32) float u[16],v[16];
 for(int k=0;k<count;){
  const int n=count-k<16?count-k:16;
  if(n>8){
   load_samples(samples,k,n,16,u,v);
   trace16(in,u,v,valid+k,x+k,y+k,w+k,n);
  }else{
   load_samples(samples,k,n,8,u,v);
   trace8(in,u,v,valid+k,x+k,y+k,w+k,n);
  }
  k+=n;
 }
}
void trace_group_avx2(const GroupInput& g,const PupilSample* samples,int count,int stride,
                      uint8_t* valid,float* x,float* y,float* w){
 alignas(32) float u[16],v[16];
 for(int k=0;k<count;){
  const int n=count-k<16?count-k:16;
  if(n>8){
   load_samples(samples,k,n,16,u,v);
   trace16_group(g,u,v,stride,valid+k,x+k,y+k,w+k,n);
  }else{
   load_samples(samples,k,n,8,u,v);
   trace8_group(g,u,v,stride,valid+k,x+k,y+k,w+k,n);
  }
  k+=n;
 }
}
} // namespace fl::cpu
