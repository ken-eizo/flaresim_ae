#pragma once
// Fast CPU ghost tracer used by the threaded CPU renderer.
//
// trace_batch_scalar() repeats upstream trace_ghost_ray() expression for
// expression (same operands, same evaluation order, same branches), so each
// ray is bit-identical to trace.cpp. The only change is that the wavelength
// dependent refractive indices come from a per-frame table filled with the
// same Surface::ior_at() call instead of being recomputed at every surface.
//
// trace_batch_avx2() traces 8 pupil samples per instruction with the same
// operation order; the thin-film cos() is a vector polynomial, so coated
// surfaces may differ from std::cos by about one float ulp.
//
// The trace_group_*() variants trace several pairs that share the second
// bounce b at once: the path to b and back is computed once and each first
// bounce a branches off with its own trip to the sensor. Every ray is
// bit-identical to tracing its pair alone with the matching batch function.
#include "FlareLensOptics.hpp"
#include "lens.h"
#include "fresnel.h"
#include "vec3.h"
#include <cmath>
#include <cstdint>

namespace fl::cpu {
struct TraceInput {
 const Surface* surfaces=nullptr;
 int n=0;
 float sensor_z=0;
 int a=0,b=1;            // reflecting surfaces, a < b
 float lambda=550;
 const float* ior=nullptr; // ior[s] == surfaces[s].ior_at(lambda)
 Vec3f dir;              // normalized source direction
 float front_radius=0,start_z=0;
};
// Pairs (a[j], base.b) for j < count; base.a is ignored.
struct GroupInput {
 TraceInput base;
 const int* a=nullptr;   // first bounces, strictly decreasing, each < base.b
 int count=0;
};

namespace detail {
inline Vec3f fast_normalize(const Vec3f& v){
 float l2=v.x*v.x+v.y*v.y+v.z*v.z;
 float inv=1.0f/std::sqrt(l2);
 return {v.x*inv,v.y*inv,v.z*inv};
}
inline bool intersect(const Ray& ray,const Surface& surf,Vec3f& hit_pos,Vec3f& normal){
 if(std::abs(surf.radius)<1e-6f){
  if(std::abs(ray.dir.z)<1e-12f)return false;
  float t=(surf.z-ray.origin.z)/ray.dir.z;
  if(t<1e-6f)return false;
  hit_pos=ray.origin+ray.dir*t;
  float h2=hit_pos.x*hit_pos.x+hit_pos.y*hit_pos.y;
  if(h2>surf.semi_aperture*surf.semi_aperture)return false;
  normal=Vec3f(0,0,(ray.dir.z>0)?-1.0f:1.0f);
  return true;
 }
 float R=surf.radius;
 Vec3f center(0,0,surf.z+R);
 Vec3f oc=ray.origin-center;
 float a=dot(ray.dir,ray.dir);
 float b=2.0f*dot(oc,ray.dir);
 float c=dot(oc,oc)-R*R;
 float disc=b*b-4.0f*a*c;
 if(disc<0)return false;
 float sqrt_disc=std::sqrt(disc);
 float inv_2a=0.5f/a;
 float t1=(-b-sqrt_disc)*inv_2a;
 float t2=(-b+sqrt_disc)*inv_2a;
 float t;
 if(t1>1e-6f&&t2>1e-6f){
  float z1=ray.origin.z+t1*ray.dir.z;
  float z2=ray.origin.z+t2*ray.dir.z;
  t=(std::abs(z1-surf.z)<std::abs(z2-surf.z))?t1:t2;
 }else if(t1>1e-6f)t=t1;
 else if(t2>1e-6f)t=t2;
 else return false;
 hit_pos=ray.origin+ray.dir*t;
 float h2=hit_pos.x*hit_pos.x+hit_pos.y*hit_pos.y;
 if(h2>surf.semi_aperture*surf.semi_aperture)return false;
 normal=(hit_pos-center)/std::abs(R);
 if(dot(normal,ray.dir)>0)normal=-normal;
 return true;
}
inline bool refract(const Vec3f& dir,const Vec3f& normal,float n_ratio,Vec3f& out_dir){
 float cos_i=-dot(normal,dir);
 float sin2_t=n_ratio*n_ratio*(1.0f-cos_i*cos_i);
 if(sin2_t>=1.0f)return false;
 float cos_t=std::sqrt(1.0f-sin2_t);
 out_dir=fast_normalize(dir*n_ratio+normal*(n_ratio*cos_i-cos_t));
 return true;
}
inline Vec3f reflect(const Vec3f& dir,const Vec3f& normal){
 return fast_normalize(dir-normal*(2.0f*dot(dir,normal)));
}
// The last leg of trace_ghost_ray(): refract through surfaces from..n-1,
// then propagate to the sensor plane.
inline bool to_sensor(const TraceInput& in,Ray& ray,float& weight,float current_ior,
                      int from,Vec3f& position){
 const Surface* S=in.surfaces;
 const float* ior=in.ior;
 for(int s=from;s<in.n;++s){
  Vec3f hit,norm;
  if(!intersect(ray,S[s],hit,norm))return false;
  ray.origin=hit;
  float n1=current_ior,n2=ior[s];
  float cos_i=std::abs(dot(norm,ray.dir));
  float R=surface_reflectance(cos_i,n1,n2,S[s].coating,in.lambda);
  Vec3f new_dir;
  if(!refract(ray.dir,norm,n1/n2,new_dir))return false;
  ray.dir=new_dir;
  weight*=(1.0f-R);
  current_ior=n2;
 }
 if(std::abs(ray.dir.z)<1e-12f)return false;
 float t=(in.sensor_z-ray.origin.z)/ray.dir.z;
 if(t<0)return false;
 position=ray.origin+ray.dir*t;
 return true;
}
// trace_ghost_ray() with table lookups for Surface::ior_at(lambda).
inline bool trace(const TraceInput& in,Ray ray,Vec3f& position,float& weight){
 const Surface* S=in.surfaces;
 const float* ior=in.ior;
 weight=1.0f;
 float current_ior=1.0f;
 for(int s=0;s<=in.b;++s){
  Vec3f hit,norm;
  if(!intersect(ray,S[s],hit,norm))return false;
  ray.origin=hit;
  float n1=current_ior,n2=ior[s];
  float cos_i=std::abs(dot(norm,ray.dir));
  float R=surface_reflectance(cos_i,n1,n2,S[s].coating,in.lambda);
  if(s==in.b){
   ray.dir=reflect(ray.dir,norm);
   weight*=R;
  }else{
   Vec3f new_dir;
   if(!refract(ray.dir,norm,n1/n2,new_dir))return false;
   ray.dir=new_dir;
   weight*=(1.0f-R);
   current_ior=n2;
  }
 }
 for(int s=in.b-1;s>=in.a;--s){
  Vec3f hit,norm;
  if(!intersect(ray,S[s],hit,norm))return false;
  ray.origin=hit;
  float n1=current_ior;
  float n2=s<=0?1.0f:ior[s-1];
  float cos_i=std::abs(dot(norm,ray.dir));
  float R=surface_reflectance(cos_i,n1,n2,S[s].coating,in.lambda);
  if(s==in.a){
   ray.dir=reflect(ray.dir,norm);
   weight*=R;
   current_ior=ior[in.a];
  }else{
   Vec3f new_dir;
   if(!refract(ray.dir,norm,n1/n2,new_dir))return false;
   ray.dir=new_dir;
   weight*=(1.0f-R);
   current_ior=n2;
  }
 }
 return to_sensor(in,ray,weight,current_ior,in.a+1,position);
}
} // namespace detail

// Traces samples[0..count) for one (source, pair, wavelength).
// valid[k] is 1 when the ray reaches the sensor; x/y/w are then defined.
inline void trace_batch_scalar(const TraceInput& in,const PupilSample* samples,int count,
                               uint8_t* valid,float* x,float* y,float* w){
 for(int k=0;k<count;k++){
  Ray ray{};
  ray.origin={samples[k].u*in.front_radius,samples[k].v*in.front_radius,in.start_z};
  ray.dir=in.dir;
  Vec3f p;float weight=0;
  const bool ok=detail::trace(in,ray,p,weight);
  valid[k]=ok?1:0;
  x[k]=p.x;y[k]=p.y;w[k]=weight;
 }
}

// trace_batch_scalar() for every pair of a group. Pair j's results are at
// valid/x/y/w[j*stride+k].
inline void trace_group_scalar(const GroupInput& g,const PupilSample* samples,int count,
                               int stride,uint8_t* valid,float* x,float* y,float* w){
 const TraceInput& in=g.base;
 const Surface* S=in.surfaces;
 const float* ior=in.ior;
 for(int k=0;k<count;k++){
  for(int j=0;j<g.count;j++){
   const size_t o=size_t(j)*stride+k;
   valid[o]=0;x[o]=0;y[o]=0;w[o]=0;
  }
  Ray ray{};
  ray.origin={samples[k].u*in.front_radius,samples[k].v*in.front_radius,in.start_z};
  ray.dir=in.dir;
  float weight=1.0f,current_ior=1.0f;
  bool alive=true;
  for(int s=0;s<=in.b;++s){
   Vec3f hit,norm;
   if(!detail::intersect(ray,S[s],hit,norm)){alive=false;break;}
   ray.origin=hit;
   float n1=current_ior,n2=ior[s];
   float cos_i=std::abs(dot(norm,ray.dir));
   float R=surface_reflectance(cos_i,n1,n2,S[s].coating,in.lambda);
   if(s==in.b){
    ray.dir=detail::reflect(ray.dir,norm);
    weight*=R;
   }else{
    Vec3f new_dir;
    if(!detail::refract(ray.dir,norm,n1/n2,new_dir)){alive=false;break;}
    ray.dir=new_dir;
    weight*=(1.0f-R);
    current_ior=n2;
   }
  }
  if(!alive)continue;
  // Back towards the front: pair j reflects at a[j], the others refract.
  for(int s=in.b-1,j=0;j<g.count;--s){
   Vec3f hit,norm;
   if(!detail::intersect(ray,S[s],hit,norm))break;
   ray.origin=hit;
   float n1=current_ior;
   float n2=s<=0?1.0f:ior[s-1];
   float cos_i=std::abs(dot(norm,ray.dir));
   float R=surface_reflectance(cos_i,n1,n2,S[s].coating,in.lambda);
   if(s==g.a[j]){
    Ray branch=ray;
    branch.dir=detail::reflect(ray.dir,norm);
    float branch_weight=weight*R;
    Vec3f p;
    if(detail::to_sensor(in,branch,branch_weight,ior[s],s+1,p)){
     const size_t o=size_t(j)*stride+k;
     valid[o]=1;x[o]=p.x;y[o]=p.y;w[o]=branch_weight;
    }
    if(++j==g.count)break;
   }
   Vec3f new_dir;
   if(!detail::refract(ray.dir,norm,n1/n2,new_dir))break;
   ray.dir=new_dir;
   weight*=(1.0f-R);
   current_ior=n2;
  }
 }
}

#if defined(FLARELENS_HAVE_AVX2_TRACE)
bool avx2_supported() noexcept;
void trace_batch_avx2(const TraceInput& in,const PupilSample* samples,int count,
                      uint8_t* valid,float* x,float* y,float* w);
void trace_group_avx2(const GroupInput& g,const PupilSample* samples,int count,int stride,
                      uint8_t* valid,float* x,float* y,float* w);
#endif
} // namespace fl::cpu
