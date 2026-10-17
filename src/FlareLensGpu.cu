// ============================================================================
// ghost_cuda.cu 窶・GPU ghost rendering: device math, scatter kernel, launcher
//
// Design notes
// 笏笏笏笏笏笏笏笏笏笏笏笏笏
// 窶｢ The device-side math is self-contained (no STL) to avoid portability
//   issues with std::clamp / std::sqrt in device code across toolchain
//   versions.  It mirrors the logic in fresnel.h, vec3.h and trace.cpp
//   exactly 窶・keep them in sync if those files change.
//
// 窶｢ Surface (from lens.h) is plain POD and is uploaded to device as-is via
//   cudaMemcpy.  Only the flat array is uploaded; LensSystem (which owns a
//   std::vector) stays on the CPU.
//
// 窶｢ Thread layout:
//     gridDim  = (n_pairs * n_sources, grid_blocks, 1)
//     blockDim = (BLOCK_SIZE, 1, 1)
//   Each thread handles one entrance-pupil sample for one (pair, source) and
//   traces 3 wavelengths (R, G, B) sequentially.  AtomicAdd scatters results.
// ============================================================================

#include "ghost_cuda.h"
#include "lens.h"       // Surface (POD) 窶・uploaded to device
#include "ghost.h"      // BrightPixel, GhostPair, GhostConfig (host side)

#include <cuda_runtime.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>


// ===========================================================================
// GpuBufferCache 窶・implementation of the persistent buffer cache.
// ===========================================================================

void GpuBufferCache::release()
{
    cudaFree(d_surfs);  d_surfs = nullptr;  surfs_bytes = 0;
    cudaFree(d_pairs);  d_pairs = nullptr;  pairs_bytes = 0;
    cudaFree(d_src);    d_src   = nullptr;  src_bytes   = 0;
    cudaFree(d_grid);   d_grid  = nullptr;  grid_bytes  = 0;
    cudaFree(d_spec);   d_spec  = nullptr;  spec_bytes  = 0;
    cudaFree(d_out_r);  d_out_r = nullptr;
    cudaFree(d_out_g);  d_out_g = nullptr;
    cudaFree(d_out_b);  d_out_b = nullptr;
    out_floats = 0;
}

// ===========================================================================
// Device-side math 窶・no STL, no std:: calls
// ===========================================================================

// ---- Minimal 3-vector ----

struct DVec3 { float x, y, z; };

__device__ __forceinline__ DVec3 dv(float x, float y, float z)
    { return {x, y, z}; }
__device__ __forceinline__ DVec3 dv_add(DVec3 a, DVec3 b)
    { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
__device__ __forceinline__ DVec3 dv_sub(DVec3 a, DVec3 b)
    { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
__device__ __forceinline__ DVec3 dv_scale(DVec3 a, float s)
    { return {a.x*s, a.y*s, a.z*s}; }
__device__ __forceinline__ DVec3 dv_neg(DVec3 a)
    { return {-a.x, -a.y, -a.z}; }
__device__ __forceinline__ float dv_dot(DVec3 a, DVec3 b)
    { return a.x*b.x + a.y*b.y + a.z*b.z; }
__device__ __forceinline__ DVec3 dv_normalize(DVec3 v)
{
    float inv = rsqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
    return {v.x*inv, v.y*inv, v.z*inv};
}

struct DRay { DVec3 origin; DVec3 dir; };

// ---- Dispersion (Cauchy via Abbe number) ----

__device__ __forceinline__
float d_dispersion_ior(float n_d, float V_d, float lambda_nm)
{
    if (V_d < 0.1f || n_d <= 1.0001f) return n_d;

    const float lF = 486.13f, lC = 656.27f, ld = 587.56f;
    float dn       = (n_d - 1.0f) / V_d;
    float inv_lF2  = 1.0f / (lF * lF);
    float inv_lC2  = 1.0f / (lC * lC);
    float inv_ld2  = 1.0f / (ld * ld);
    float B        = dn / (inv_lF2 - inv_lC2);
    float A        = n_d - B * inv_ld2;
    return A + B / (lambda_nm * lambda_nm);
}

__device__ __forceinline__
float d_ior_at(const Surface& s, float lambda_nm)
{
    return d_dispersion_ior(s.ior, s.abbe_v, lambda_nm);
}

__device__ __forceinline__
float d_ior_before(const Surface* surfs, int idx, float lambda_nm)
{
    return (idx <= 0) ? 1.0f : d_ior_at(surfs[idx - 1], lambda_nm);
}

// ---- Fresnel reflectance ----

__device__ __forceinline__
float d_fresnel_reflectance(float cos_i, float n1, float n2)
{
    cos_i        = fabsf(cos_i);
    float eta    = n1 / n2;
    float sin2_t = eta * eta * (1.0f - cos_i * cos_i);
    if (sin2_t >= 1.0f) return 1.0f;  // TIR
    float cos_t  = sqrtf(1.0f - sin2_t);
    float rs     = (n1*cos_i - n2*cos_t) / (n1*cos_i + n2*cos_t);
    float rp     = (n2*cos_i - n1*cos_t) / (n2*cos_i + n1*cos_t);
    return 0.5f * (rs*rs + rp*rp);
}

// Single-layer MgF2 AR coating (mirrors coating_reflectance in fresnel.h)
__device__ __forceinline__
float d_coating_reflectance(float cos_i, float n1, float n2,
                             float coating_n, float d_nm, float lambda_nm)
{
    float sin2_c = (n1/coating_n) * (n1/coating_n) * (1.0f - cos_i*cos_i);
    if (sin2_c >= 1.0f) return d_fresnel_reflectance(cos_i, n1, n2);
    float cos_c  = sqrtf(1.0f - sin2_c);

    const float PI = 3.14159265358979323846f;
    float delta  = 2.0f * PI * coating_n * d_nm * cos_c / lambda_nm;
    float r01    = (n1*cos_i - coating_n*cos_c) / (n1*cos_i + coating_n*cos_c);

    float sin2_2 = (coating_n/n2) * (coating_n/n2) * (1.0f - cos_c*cos_c);
    if (sin2_2 >= 1.0f) return d_fresnel_reflectance(cos_i, n1, n2);
    float cos_2  = sqrtf(1.0f - sin2_2);
    float r12    = (coating_n*cos_c - n2*cos_2) / (coating_n*cos_c + n2*cos_2);

    float cos_2d = cosf(2.0f * delta);
    float num    = r01*r01 + r12*r12 + 2.0f*r01*r12*cos_2d;
    float den    = 1.0f   + r01*r01*r12*r12 + 2.0f*r01*r12*cos_2d;
    float R      = num / den;
    return fminf(fmaxf(R, 0.0f), 1.0f);
}

__device__ __forceinline__
float d_surface_reflectance(float cos_i, float n1, float n2,
                             int coating_layers, float lambda_nm)
{
    if (coating_layers <= 0)
        return d_fresnel_reflectance(cos_i, n1, n2);

    // MgF2 quarter-wave at 550 nm
    const float mgf2_n        = 1.38f;
    const float design_lambda = 550.0f;
    float qw_thick            = design_lambda / (4.0f * mgf2_n);  // ~99.6 nm
    float R = d_coating_reflectance(cos_i, n1, n2, mgf2_n, qw_thick, lambda_nm);
    for (int i = 1; i < coating_layers; ++i) R *= 0.25f;
    return fminf(fmaxf(R, 0.0f), 1.0f);
}

// ---- Ray窶都urface intersection ----

__device__ __forceinline__
bool d_intersect_surface(const DRay& ray, const Surface& surf,
                          DVec3& hit, DVec3& norm)
{
    if (fabsf(surf.radius) < 1e-6f)
    {
        // Flat surface: ray窶菟lane at z = surf.z
        if (fabsf(ray.dir.z) < 1e-12f) return false;
        float t = (surf.z - ray.origin.z) / ray.dir.z;
        if (!(t > 1e-6f)) return false;  // catches t<=0, NaN, and Inf

        hit = dv(ray.origin.x + ray.dir.x*t,
                 ray.origin.y + ray.dir.y*t,
                 ray.origin.z + ray.dir.z*t);

        if (!(hit.x*hit.x + hit.y*hit.y <= surf.semi_aperture*surf.semi_aperture))
            return false;

        norm = dv(0.0f, 0.0f, (ray.dir.z > 0) ? -1.0f : 1.0f);
        return true;
    }

    // Spherical surface 窶・sphere centre at (0, 0, surf.z + surf.radius)
    float R   = surf.radius;
    DVec3 ctr = dv(0.0f, 0.0f, surf.z + R);
    DVec3 oc  = dv_sub(ray.origin, ctr);

    float a    = dv_dot(ray.dir, ray.dir);
    float b    = 2.0f * dv_dot(oc, ray.dir);
    float c    = dv_dot(oc, oc) - R*R;
    float disc = b*b - 4.0f*a*c;
    if (disc < 0.0f) return false;

    float sd    = sqrtf(disc);
    float inv2a = 0.5f / a;
    float t1    = (-b - sd) * inv2a;
    float t2    = (-b + sd) * inv2a;

    float t;
    if (t1 > 1e-6f && t2 > 1e-6f)
    {
        float z1 = ray.origin.z + t1*ray.dir.z;
        float z2 = ray.origin.z + t2*ray.dir.z;
        t = (fabsf(z1 - surf.z) < fabsf(z2 - surf.z)) ? t1 : t2;
    }
    else if (t1 > 1e-6f) t = t1;
    else if (t2 > 1e-6f) t = t2;
    else return false;

    hit = dv(ray.origin.x + ray.dir.x*t,
             ray.origin.y + ray.dir.y*t,
             ray.origin.z + ray.dir.z*t);

    // !(... <= ...) instead of (... >) catches NaN hit points.
    if (!(hit.x*hit.x + hit.y*hit.y <= surf.semi_aperture*surf.semi_aperture))
        return false;

    // Normal from sphere centre; ensure it opposes the ray
    float invR = 1.0f / fabsf(R);
    norm = dv((hit.x - ctr.x)*invR,
              (hit.y - ctr.y)*invR,
              (hit.z - ctr.z)*invR);
    if (dv_dot(norm, ray.dir) > 0.0f) norm = dv_neg(norm);
    return true;
}

// ---- Refraction / reflection ----

__device__ __forceinline__
bool d_refract(const DVec3& dir, const DVec3& norm,
               float n_ratio, DVec3& out)
{
    float cos_i  = -dv_dot(norm, dir);
    float sin2_t = n_ratio*n_ratio*(1.0f - cos_i*cos_i);
    if (sin2_t >= 1.0f) return false;
    float cos_t  = sqrtf(1.0f - sin2_t);
    float k      = n_ratio*cos_i - cos_t;
    float ox = dir.x*n_ratio + norm.x*k;
    float oy = dir.y*n_ratio + norm.y*k;
    float oz = dir.z*n_ratio + norm.z*k;
    float sq = ox*ox + oy*oy + oz*oz;
    if (sq < 1e-18f || !isfinite(sq)) return false;   // degenerate: discard
    float inv = rsqrtf(sq);
    out = dv(ox*inv, oy*inv, oz*inv);
    return true;
}

__device__ __forceinline__
bool d_reflect(const DVec3& dir, const DVec3& norm, DVec3& out)
{
    float d2 = 2.0f * dv_dot(dir, norm);
    float ox = dir.x - norm.x*d2;
    float oy = dir.y - norm.y*d2;
    float oz = dir.z - norm.z*d2;
    float sq = ox*ox + oy*oy + oz*oz;
    if (sq < 1e-18f || !isfinite(sq)) return false;
    float inv = rsqrtf(sq);
    out = dv(ox*inv, oy*inv, oz*inv);
    return true;
}

// ===========================================================================
// Device-side ghost ray trace
// Mirrors trace_ghost_ray() in trace.cpp exactly.
// ===========================================================================

struct DTraceResult { DVec3 position; float weight; bool valid; };

__device__ __forceinline__
DTraceResult d_trace_ghost_ray(const DRay& ray_in,
                                const Surface* surfs, int n_surfs,
                                float sensor_z,
                                int bounce_a, int bounce_b,
                                float lambda_nm)
{
    DRay  ray          = ray_in;
    float current_ior  = 1.0f;
    float weight       = 1.0f;

    // ---- Phase 1: forward 0..bounce_b (reflect at bounce_b) ----
    for (int s = 0; s <= bounce_b; ++s)
    {
        DVec3 hit, norm;
        if (!d_intersect_surface(ray, surfs[s], hit, norm))
            return {dv(0,0,0), 0.0f, false};

        ray.origin = hit;
        float n1   = current_ior;
        float n2   = d_ior_at(surfs[s], lambda_nm);
        float cos_i = fabsf(dv_dot(norm, ray.dir));
        float R     = d_surface_reflectance(cos_i, n1, n2,
                                             surfs[s].coating, lambda_nm);

        if (s == bounce_b)
        {
            DVec3 rdir;
            if (!d_reflect(ray.dir, norm, rdir))
                return {dv(0,0,0), 0.0f, false};
            ray.dir = rdir;
            weight *= R;
        }
        else
        {
            DVec3 new_dir;
            if (!d_refract(ray.dir, norm, n1/n2, new_dir))
                return {dv(0,0,0), 0.0f, false};
            ray.dir     = new_dir;
            weight     *= (1.0f - R);
            current_ior = n2;
        }
    }

    // ---- Phase 2: backward bounce_b-1..bounce_a (reflect at bounce_a) ----
    for (int s = bounce_b - 1; s >= bounce_a; --s)
    {
        DVec3 hit, norm;
        if (!d_intersect_surface(ray, surfs[s], hit, norm))
            return {dv(0,0,0), 0.0f, false};

        ray.origin = hit;
        float n1   = current_ior;
        float n2   = d_ior_before(surfs, s, lambda_nm);
        float cos_i = fabsf(dv_dot(norm, ray.dir));
        float R     = d_surface_reflectance(cos_i, n1, n2,
                                             surfs[s].coating, lambda_nm);

        if (s == bounce_a)
        {
            DVec3 rdir;
            if (!d_reflect(ray.dir, norm, rdir))
                return {dv(0,0,0), 0.0f, false};
            ray.dir     = rdir;
            weight     *= R;
            current_ior = d_ior_at(surfs[bounce_a], lambda_nm);
        }
        else
        {
            DVec3 new_dir;
            if (!d_refract(ray.dir, norm, n1/n2, new_dir))
                return {dv(0,0,0), 0.0f, false};
            ray.dir     = new_dir;
            weight     *= (1.0f - R);
            current_ior = n2;
        }
    }

    // ---- Phase 3: forward bounce_a+1..n_surfs-1 ----
    for (int s = bounce_a + 1; s < n_surfs; ++s)
    {
        DVec3 hit, norm;
        if (!d_intersect_surface(ray, surfs[s], hit, norm))
            return {dv(0,0,0), 0.0f, false};

        ray.origin = hit;
        float n1   = current_ior;
        float n2   = d_ior_at(surfs[s], lambda_nm);
        float cos_i = fabsf(dv_dot(norm, ray.dir));
        float R     = d_surface_reflectance(cos_i, n1, n2,
                                             surfs[s].coating, lambda_nm);

        DVec3 new_dir;
        if (!d_refract(ray.dir, norm, n1/n2, new_dir))
            return {dv(0,0,0), 0.0f, false};
        ray.dir     = new_dir;
        weight     *= (1.0f - R);
        current_ior = n2;
    }

    // ---- Propagate to sensor plane ----
    if (fabsf(ray.dir.z) < 1e-12f) return {dv(0,0,0), 0.0f, false};
    float t = (sensor_z - ray.origin.z) / ray.dir.z;
    // Use !(t > 0) rather than (t < 0) so that NaN is caught as invalid.
    if (!(t > 0.0f)) return {dv(0,0,0), 0.0f, false};

    DVec3 pos = dv(ray.origin.x + ray.dir.x*t,
                   ray.origin.y + ray.dir.y*t,
                   ray.origin.z + ray.dir.z*t);
    return {pos, weight, true};
}

// ===========================================================================
// GPU-side data structures (plain POD, no STL)
// ===========================================================================

struct GPUPair   { int surf_a, surf_b; float area_boost; };
struct GPUSource {
 float angle_x,angle_y,r,g,b;
 float dir_x,dir_y,dir_z;
};
// Optical source direction depends only on source position and the sensor.
// Previously every (pupil, pair, wavelength) ray repeated two costly
// trigonometric functions and a normalization. Perform these once per source
// on the exact same host-provided CUDA stream, with the SAME device math.
__global__ void flarelens_prepare_source_directions(GPUSource* sources,int count){
 const int i=int(blockIdx.x)*256+int(threadIdx.x);
 if(i>=count)return;
 GPUSource s=sources[i];
 const DVec3 d=dv_normalize(dv(tanf(s.angle_x),tanf(s.angle_y),1.f));
 s.dir_x=d.x;s.dir_y=d.y;s.dir_z=d.z;
 sources[i]=s;
}
struct GPUSample { float u, v; };  // entrance-pupil grid point
// One wavelength sample with pre-computed RGB colour weights.
// Matches GPUSpectralSample in ghost_cuda.h.
struct GPUSpectralSampleDev { float lambda, rw, gw, bw; };



#include "FlareLensGpuBridge.hpp"
#include "FlareLensNukeHaze.hpp"
#include "FlareLensOptics.hpp"
#include <cstddef>
#include <cstdint>
#include <limits>

// The original FlareSim CUDA ray physics above is preserved unmodified.
// AE-native output uses host-owned BGRA128, instead of three planar buffers.

// Native AE adaptation of upstream ghost_kernel's *exact* four-pixel splat.
// Only the destination layout differs: interleaved BGRA128 instead of planar RGB.
__global__ void flarelens_direct_kernel(
 const Surface* surfaces,int n_surfs,float sensor_z,const GPUPair* pairs,int n_pairs,
 const GPUSource* sources,int n_sources,const GPUSample* pupil,int n_pupil,
 const GPUSpectralSampleDev* spectra,int n_spectra,
 float front_radius,float start_z,float sensor_half_w,float sensor_half_h,
 int width,int height,int pitch,float gain,float ray_weight,float* out){
 const int ps=blockIdx.x,pi=ps/n_sources,si=ps%n_sources;
 const int index=int(blockIdx.y)*256+int(threadIdx.x);
 if(pi>=n_pairs||index>=n_pupil)return;
 const GPUPair pair=pairs[pi];
 if(pair.surf_a<0||pair.surf_b<=pair.surf_a||pair.surf_b>=n_surfs)return;
 const GPUSource src=sources[si];
 const GPUSample p=pupil[index];
 DRay ray={dv(p.u*front_radius,p.v*front_radius,start_z),
           dv(src.dir_x,src.dir_y,src.dir_z)};
 for(int q=0;q<n_spectra;q++){
  const GPUSpectralSampleDev sp=spectra[q];
  const auto hit=d_trace_ghost_ray(ray,surfaces,n_surfs,sensor_z,
                                   pair.surf_a,pair.surf_b,sp.lambda);
  if(!hit.valid||!isfinite(hit.position.x)||!isfinite(hit.position.y))continue;
  const float base=hit.weight*ray_weight*gain*pair.area_boost;
  if(base<1.e-14f)continue;
  float px=(hit.position.x/(2.f*sensor_half_w)+.5f)*width;
  float py=(hit.position.y/(2.f*sensor_half_h)+.5f)*height;
  if(!isfinite(px)||!isfinite(py))continue;
  px=fmaxf(-2.e9f,fminf(2.e9f,px));
  py=fmaxf(-2.e9f,fminf(2.e9f,py));
  const int x=int(floorf(px-.5f)),y=int(floorf(py-.5f));
  const float fx=px-.5f-x,fy=py-.5f-y;
  const float energy[3]={src.b*sp.bw*base,src.g*sp.gw*base,src.r*sp.rw*base};
  for(int dy=0;dy<2;dy++){
   const int yy=y+dy;
   if(yy<0||yy>=height)continue;
   for(int dx=0;dx<2;dx++){
    const int xx=x+dx;
    if(xx<0||xx>=width)continue;
    const float wt=(dy?fy:1.f-fy)*(dx?fx:1.f-fx);
    const size_t offset=size_t(yy)*pitch+4*xx;
    for(int c=0;c<3;c++){
     if(energy[c]>1.e-14f)atomicAdd(out+offset+c,energy[c]*wt);
    }
   }
  }
 }
}
// Spectrally parallel variant of the unmodified Nuke photon tracer.
// Optical equations, Fresnel, dispersion, and 2x2 splat are identical.
// Each CUDA block works on exactly one wavelength; this can reduce register
// pressure for 7/11-band spectra, at the cost of more thread scheduling.
__global__ void flarelens_spectral_kernel(
 const Surface* surfaces,int n_surfs,float sensor_z,const GPUPair* pairs,int n_pairs,
 const GPUSource* sources,int n_sources,const GPUSample* pupil,int n_pupil,
 const GPUSpectralSampleDev* spectra,int n_spectra,
 float front_radius,float start_z,float sensor_half_w,float sensor_half_h,
 int width,int height,int pitch,float gain,float ray_weight,float* out){
 const int ps=blockIdx.x,pi=ps/n_sources,si=ps%n_sources;
 const int index=int(blockIdx.y)*256+int(threadIdx.x);
 const int q=int(blockIdx.z);
 if(pi>=n_pairs||index>=n_pupil||q>=n_spectra)return;
 const GPUPair pair=pairs[pi];
 if(pair.surf_a<0||pair.surf_b<=pair.surf_a||pair.surf_b>=n_surfs)return;
 const GPUSource src=sources[si];
 const GPUSample p=pupil[index];
 const DRay ray={dv(p.u*front_radius,p.v*front_radius,start_z),
           dv(src.dir_x,src.dir_y,src.dir_z)};
 const GPUSpectralSampleDev sp=spectra[q];
 const auto hit=d_trace_ghost_ray(ray,surfaces,n_surfs,sensor_z,
                                  pair.surf_a,pair.surf_b,sp.lambda);
 if(!hit.valid||!isfinite(hit.position.x)||!isfinite(hit.position.y))return;
 const float base=hit.weight*ray_weight*gain*pair.area_boost;
 if(base<1.e-14f)return;
 float px=(hit.position.x/(2.f*sensor_half_w)+.5f)*width;
 float py=(hit.position.y/(2.f*sensor_half_h)+.5f)*height;
 if(!isfinite(px)||!isfinite(py))return;
 px=fmaxf(-2.e9f,fminf(2.e9f,px));
 py=fmaxf(-2.e9f,fminf(2.e9f,py));
 const int x=int(floorf(px-.5f)),y=int(floorf(py-.5f));
 const float fx=px-.5f-x,fy=py-.5f-y;
 const float energy[3]={src.b*sp.bw*base,src.g*sp.gw*base,src.r*sp.rw*base};
 for(int dy=0;dy<2;dy++){
  const int yy=y+dy;
  if(yy<0||yy>=height)continue;
  for(int dx=0;dx<2;dx++){
   const int xx=x+dx;
   if(xx<0||xx>=width)continue;
   const float wt=(dy?fy:1.f-fy)*(dx?fx:1.f-fx);
   const size_t offset=size_t(yy)*pitch+4*xx;
   for(int c=0;c<3;c++){
    if(energy[c]>1.e-14f)atomicAdd(out+offset+c,energy[c]*wt);
   }
  }
 }
}
// Source Preview needs no optical trace: dim RGB in one GPU pass and
// atomically paint up to 121 pixels per detected source. Its per-source
// cost is constant, independent of the Ray Grid and ghost pair count.
__global__ void flarelens_sources_preview_base(
 const float4* input,int input_pitch,float4* output,int output_pitch,
 int width,int height){
 const int i=int(blockIdx.x)*256+int(threadIdx.x);
 if(i>=width*height)return;
 const int x=i%width,y=i/width;
 const float4 in=input?input[size_t(y)*input_pitch+x]:make_float4(0,0,0,0);
 output[size_t(y)*output_pitch+x]=
   make_float4(in.x*.12f,in.y*.12f,in.z*.12f,1.f);
}
__global__ void flarelens_sources_preview_mark(
 const float4* input,int input_pitch,float4* output,int output_pitch,
 int width,int height,const int2* markers,int count){
 const int s=int(blockIdx.x),t=int(threadIdx.x);
 if(s>=count||t>=121)return;
 const int dx=t%11-5,dy=t/11-5;
 const int r2=dx*dx+dy*dy;
 if(!((r2>=9&&r2<=25)||(dx==0&&r2<=25)||
      (dy==0&&r2<=25)))return;
 const int x=markers[s].x+dx,y=markers[s].y+dy;
 if(x<0||y<0||x>=width||y>=height)return;
 const float4 in=input?input[size_t(y)*input_pitch+x]:make_float4(0,0,0,0);
 float4* dst=output+size_t(y)*output_pitch+x;
 // Duplicate marks write the exact same value, safely via atomics.
 atomicExch(&dst->x,in.x*.12f+1.f);
 atomicExch(&dst->y,in.y*.12f+1.f);
 atomicExch(&dst->z,in.z*.12f+1.f);
}
// Two full-resolution scratch surfaces hold only RGB photon energy (BGRA128).
// The optical ray kernel above is unchanged. All post-processing remains on
// the host-owned CUDA stream; no GPU->CPU readback of pixels is performed.
__global__ void flarelens_init_plate(float4* output,int pitch,int w,int h,int mode){
 const int id=int(blockIdx.x)*256+int(threadIdx.x);
 if(id>=w*h)return;
 const int x=id%w,y=id/w;
 if(mode!=1)output[size_t(y)*pitch+x]=make_float4(0,0,0,1);
}
__global__ void flarelens_fill_haze(float4* rgb,int w,int h,int x,int y,
                                     float r,float g,float b){
 if(threadIdx.x||blockIdx.x)return;
 if(x>=0&&x<w&&y>=0&&y<h){
  const size_t i=size_t(y)*w+x;
  rgb[i].x+=b;rgb[i].y+=g;rgb[i].z+=r;
 }
}
// A separable *sliding window* has O(pixels) work even for broad Nuke haze
// blurs (radius can be hundreds). 128-pixel segments expose enough GPU blocks.
// Edge windows use Nuke's actual variable divisor: no reflected edges.
__global__ void flarelens_blur_rows(
 const float4* src,float4* dst,int w,int h,int radius){
 const int y=int(blockIdx.x)*int(blockDim.x)+int(threadIdx.x);
 const int x_begin=int(blockIdx.y)*128;
 if(y>=h||x_begin>=w)return;
 const int x_end=min(x_begin+128,w);
 const int a=max(0,x_begin-radius),b=min(w-1,x_begin+radius);
 double sb=0,sg=0,sr=0;
 for(int x=a;x<=b;x++){
  const float4 p=src[size_t(y)*w+x];
  sb+=p.x;sg+=p.y;sr+=p.z;
 }
 for(int x=x_begin;x<x_end;x++){
  const double denom=double(min(w-1,x+radius)-max(0,x-radius)+1);
  dst[size_t(y)*w+x]=make_float4(float(sb/denom),float(sg/denom),
                                  float(sr/denom),0);
  const int remove=x-radius,add=x+radius+1;
  if(remove>=0){
   const float4 p=src[size_t(y)*w+remove];
   sb-=p.x;sg-=p.y;sr-=p.z;
  }
  if(add<w){
   const float4 p=src[size_t(y)*w+add];
   sb+=p.x;sg+=p.y;sr+=p.z;
  }
 }
}
__global__ void flarelens_blur_cols(
 const float4* src,float4* dst,int w,int h,int radius){
 const int x=int(blockIdx.x)*int(blockDim.x)+int(threadIdx.x);
 const int y_begin=int(blockIdx.y)*128;
 if(x>=w||y_begin>=h)return;
 const int y_end=min(y_begin+128,h);
 const int a=max(0,y_begin-radius),b=min(h-1,y_begin+radius);
 double sb=0,sg=0,sr=0;
 for(int y=a;y<=b;y++){
  const float4 p=src[size_t(y)*w+x];
  sb+=p.x;sg+=p.y;sr+=p.z;
 }
 for(int y=y_begin;y<y_end;y++){
  const double denom=double(min(h-1,y+radius)-max(0,y-radius)+1);
  dst[size_t(y)*w+x]=make_float4(float(sb/denom),float(sg/denom),
                                  float(sr/denom),0);
  const int remove=y-radius,add=y+radius+1;
  if(remove>=0){
   const float4 p=src[size_t(remove)*w+x];
   sb-=p.x;sg-=p.y;sr-=p.z;
  }
  if(add<h){
   const float4 p=src[size_t(add)*w+x];
   sb+=p.x;sg+=p.y;sr+=p.z;
  }
 }
}
// Small-radius ghost blur (normally radius 4-12 px) should NOT use one
// sequential sliding thread per 128 output pixels. Coalesced one-thread-per-
// output-pixel evaluation has much higher occupancy for these small radii.
__global__ void flarelens_blur_rows_small(
 const float4* src,float4* dst,int w,int h,int radius){
 const int x=int(blockIdx.x)*256+int(threadIdx.x);
 const int y=int(blockIdx.y);
 if(x>=w||y>=h)return;
 const int a=max(0,x-radius),b=min(w-1,x+radius);
 float sb=0.f,sg=0.f,sr=0.f;
 for(int xx=a;xx<=b;xx++){
  const float4 c=src[size_t(y)*w+xx];
  sb+=c.x;sg+=c.y;sr+=c.z;
 }
 const float denom=1.f/(b-a+1);
 dst[size_t(y)*w+x]=make_float4(sb*denom,sg*denom,sr*denom,0);
}
__global__ void flarelens_blur_cols_small(
 const float4* src,float4* dst,int w,int h,int radius){
 const int x=int(blockIdx.x)*256+int(threadIdx.x);
 const int y=int(blockIdx.y);
 if(x>=w||y>=h)return;
 const int a=max(0,y-radius),b=min(h-1,y+radius);
 float sb=0.f,sg=0.f,sr=0.f;
 for(int yy=a;yy<=b;yy++){
  const float4 c=src[size_t(yy)*w+x];
  sb+=c.x;sg+=c.y;sr+=c.z;
 }
 const float denom=1.f/(b-a+1);
 dst[size_t(y)*w+x]=make_float4(sb*denom,sg*denom,sr*denom,0);
}
struct GPUHazeSeed {int x0,y0,x1,y1;float b,g,r;};
// Seeding all HDR sources is one launch instead of one kernel launch per
// source. Bright sources at the same pixel add physically to one field.
__global__ void flarelens_scatter_haze(
 float4* photon,int w,int h,const GPUHazeSeed* src,int count,int footprint){
 const int i=int(blockIdx.x)*256+int(threadIdx.x);
 if(i>=count)return;
 const GPUHazeSeed s=src[i];
 if(s.x0<0||s.y0<0||s.x0>=s.x1||s.y0>=s.y1||
    s.x1>w||s.y1>h)return;
 for(int y=s.y0;y<s.y1;y++)
  for(int x=s.x0;x<s.x1;x++){
   const size_t index=size_t(y)*w+x;
   atomicAdd(&photon[index].x,s.b);
   atomicAdd(&photon[index].y,s.g);
   atomicAdd(&photon[index].z,s.r);
  }
}
// A point-source haze blurred by separable 1D box filters remains exactly
// separable: response(x,y)=response_x[x]*response_y[y] for each source.
// Evaluate those 1D Nuke box responses once per source on CPU (O(W+H)),
// then sum every haze source entirely on the GPU in one coalesced pass.
// This avoids 6+ full-frame CUDA blur sweeps for a ~350px haze radius.
__global__ void flarelens_add_analytic_haze(
 float4* out,int pitch,int w,int h,const GPUSource* source,
 const float* weights_x,const float* weights_y,int count,float gain){
 const int i=int(blockIdx.x)*256+int(threadIdx.x);
 if(i>=w*h)return;
 const int x=i%w,y=i/w;
 float3 added=make_float3(0,0,0);
 for(int s=0;s<count;s++){
  const float amount=weights_x[size_t(s)*w+x]*
                     weights_y[size_t(s)*h+y]*gain;
  if(amount==0)continue;
  added.x+=source[s].b*amount;
  added.y+=source[s].g*amount;
  added.z+=source[s].r*amount;
 }
 float4 pixel=out[size_t(y)*pitch+x];
 pixel.x+=added.x;pixel.y+=added.y;pixel.z+=added.z;
 out[size_t(y)*pitch+x]=pixel;
}
__global__ void flarelens_add_light(const float4* light,float4* dst,
                                     int pitch,int w,int h,float scale){
 const int id=int(blockIdx.x)*256+int(threadIdx.x);
 if(id>=w*h)return;
 const int x=id%w,y=id/w;
 const size_t pos=size_t(y)*pitch+x;
 const float4 v=light[id];
 float4 old=dst[pos];
 old.x+=v.x*scale;old.y+=v.y*scale;old.z+=v.z*scale;
 dst[pos]=old;
}

// A separate pass is required after the atomic photon splats. The initial
// BGRA128 alpha can be zero for an empty shape layer; without this pass the
// RGB ghosts are hidden during standard After Effects compositing.
// This changes only AE presentation alpha, never the upstream ray intensities.
__global__ void flarelens_composite_alpha_kernel(
    const float* input,int input_pitch,float* output,int output_pitch,
    int width,int height){
 const int index=int(blockIdx.x)*256+int(threadIdx.x);
 if(index>=width*height)return;
 const int x=index%width,y=index/width;
 const size_t src=size_t(y)*input_pitch+size_t(x)*4;
 const size_t dst=size_t(y)*output_pitch+size_t(x)*4;
 const float r=fmaxf(0.f,output[dst+2]-input[src+2]);
 const float g=fmaxf(0.f,output[dst+1]-input[src+1]);
 const float b=fmaxf(0.f,output[dst+0]-input[src+0]);
 const float peak=fmaxf(r,fmaxf(g,b));
 const float coverage=isfinite(peak)?fminf(1.f,peak):0.f;
 const float incoming=isfinite(input[src+3])?
    fminf(1.f,fmaxf(0.f,input[src+3])):0.f;
 output[dst+3]=incoming+(1.f-incoming)*coverage;
}
namespace fl::gpu {
namespace {
size_t align16(size_t n){return (n+size_t(15))&~size_t(15);}
std::vector<float> haze_profile(int n,int coordinate,int radius,int passes){
 std::vector<float> output(size_t(n),0.f);
 if(coordinate>=0&&coordinate<n)output[size_t(coordinate)]=1.f;
 if(radius<=0||passes<=0)return output;
 radius=std::min(radius,n);
 passes=std::min(passes,6);
 std::vector<float> next(size_t(n),0.f);
 std::vector<double> prefix(size_t(n)+1);
 for(int k=0;k<passes;k++){
  prefix[0]=0;
  for(int x=0;x<n;x++)prefix[size_t(x)+1]=prefix[size_t(x)]+output[x];
  for(int x=0;x<n;x++){
   const int a=std::max(0,x-radius),b=std::min(n-1,x+radius);
   next[x]=float((prefix[size_t(b)+1]-prefix[size_t(a)])/(b-a+1));
  }
  output.swap(next);
 }
 return output;
}
struct SyncOnExit {
 cudaStream_t stream=nullptr;
 bool pending=false;
 ~SyncOnExit(){if(pending)cudaStreamSynchronize(stream);}
 bool finish(){pending=false;return cudaStreamSynchronize(stream)==cudaSuccess;}
};
}
size_t scratch_bytes(const LensSystem& lens,int sources,int grid,int w,int h,
                     int output_mode,bool need_blur,bool dense_haze,bool analytic_haze) noexcept {
 if(sources<1||sources>1000000||grid<2||grid>2048||w<1||h<1||
    w>16384||h>16384||lens.surfaces.size()<2||lens.surfaces.size()>64||
    (output_mode!=1&&output_mode!=2&&output_mode!=3&&
     output_mode!=4&&output_mode!=5))return 0;
 if(output_mode==3)return align16(size_t(sources)*sizeof(int2));
 const size_t pairs=std::min<size_t>(2048,lens.surfaces.size()*lens.surfaces.size());
 const size_t npx=size_t(w)*size_t(h);
 if(npx>size_t(16384)*16384)return 0;
 // On an opaque Flare/Ghost-only plate without a blur, Nuke scatter can
 // write directly to AE's GPU output. Haze-only needs no scatter buffer.
 const bool ghosts=output_mode!=5;
 const bool photon=(ghosts&&(output_mode==1||need_blur))||dense_haze;
 const bool temporary=(ghosts&&need_blur)||dense_haze;
 return align16(lens.surfaces.size()*sizeof(Surface))+
        align16(pairs*sizeof(GPUPair))+
        align16(size_t(sources)*sizeof(GPUSource))+
        align16(size_t(grid)*grid*sizeof(GPUSample))+
        align16(11*sizeof(GPUSpectralSampleDev))+
        (photon?align16(npx*sizeof(float4)):0)+
        (temporary?align16(npx*sizeof(float4)):0)+
        (analytic_haze?align16(size_t(sources)*size_t(w)*sizeof(float)):0)+
        (analytic_haze?align16(size_t(sources)*size_t(h)*sizeof(float)):0)+
        (dense_haze?align16(size_t(sources)*sizeof(GPUHazeSeed)):0);
}
bool cuda_compose(const LensSystem& lens,const std::vector<Source>& sources,
 const Settings& settings,const View& v,void* scratch,size_t scratch_size){
 if((v.output_mode==1&&!v.input)||!v.output||
    v.width<=0||v.height<=0||v.width>16384||v.height>16384||
    (v.output_mode==1&&v.input_pitch_bytes<v.width*16)||
    v.output_pitch_bytes<v.width*16||
    (v.output_mode==1&&(v.input_pitch_bytes%16)!=0)||
    (v.output_pitch_bytes%16)!=0||
    (v.output_mode!=1&&v.output_mode!=2&&v.output_mode!=3&&
     v.output_mode!=4&&v.output_mode!=5))
    return false;
 const int w=v.width,h=v.height;
 const size_t npx=size_t(w)*h;
 const size_t bytes=npx*sizeof(float4);
 const cudaStream_t stream=reinterpret_cast<cudaStream_t>(v.command_stream);
 // Keep pageable upload data alive until the CUDA stream has completed.
 std::vector<float> host_x,host_y;
 std::vector<GPUHazeSeed> host_seeds;
 std::vector<GPUSource> haze_lights; // Keep pageable upload alive until host CUDA stream sync
 // On every early exit drain already queued commands before CPU upload
 // vectors are destroyed or AE device scratch is released.
 SyncOnExit sync{stream,true};
 if(v.output_mode==3){
  const dim3 block(256),grid((unsigned(npx)+255)/256);
  flarelens_sources_preview_base<<<grid,block,0,stream>>>(
   static_cast<const float4*>(v.input),v.input_pitch_bytes/16,
   static_cast<float4*>(v.output),v.output_pitch_bytes/16,w,h);
  if(cudaPeekAtLastError()!=cudaSuccess)return false;
  sync.pending=true;
  if(!sources.empty()){
   if(!scratch||scratch_size<align16(sources.size()*sizeof(int2)))
    return false;
   std::vector<int2> marker_positions;
   marker_positions.reserve(sources.size());
   for(const auto& source:sources)
    marker_positions.push_back(make_int2(
       int(floorf(source.x*w)),int(floorf(source.y*h))));
   if(cudaMemcpyAsync(scratch,marker_positions.data(),
      marker_positions.size()*sizeof(int2),cudaMemcpyHostToDevice,stream)
      !=cudaSuccess)return false;
   flarelens_sources_preview_mark<<<unsigned(sources.size()),128,0,stream>>>(
     static_cast<const float4*>(v.input),v.input_pitch_bytes/16,
     static_cast<float4*>(v.output),v.output_pitch_bytes/16,w,h,
     static_cast<const int2*>(scratch),int(sources.size()));
   if(cudaPeekAtLastError()!=cudaSuccess)return false;
  }
  return sync.finish();
 }
 if(v.output_mode==1){
  if(cudaMemcpy2DAsync(v.output,v.output_pitch_bytes,v.input,v.input_pitch_bytes,
      size_t(w)*16,h,cudaMemcpyDeviceToDevice,stream)!=cudaSuccess)return false;
 }else{
  const dim3 block(256),grid((unsigned(npx)+255)/256);
  flarelens_init_plate<<<grid,block,0,stream>>>(
   static_cast<float4*>(v.output),v.output_pitch_bytes/16,w,h,v.output_mode);
  if(cudaPeekAtLastError()!=cudaSuccess)return false;
 }
 sync.pending=true;
 const auto& haze_sources=v.haze_sources?*v.haze_sources:sources;
 if(sources.empty()&&haze_sources.empty())return sync.finish();
 const bool ghost_mode=v.output_mode!=5 && settings.gain>0&&!sources.empty();
 const bool haze_mode=(v.output_mode==1||v.output_mode==2||v.output_mode==5)
                     &&v.haze_gain>0&&!haze_sources.empty();
 if(!ghost_mode&&!haze_mode)return sync.finish();
 const bool need_blur=ghost_mode&&v.ghost_blur>0&&v.ghost_blur_passes>0;
 // Dense multi-source haze: one sparse-seed pass followed by O(WH) box
 // convolution. The separable analytical solution is faster for <=8 lights
 // but O(num_sources*pixels) and not viable for 512 HDR emitters.
 const bool dense_haze=haze_mode&&(haze_sources.size()>8||v.haze_footprint>1);
 const size_t need=scratch_bytes(lens,int(std::max(sources.size(),haze_sources.size())),settings.grid,
                                 w,h,v.output_mode,need_blur,dense_haze,
                                 haze_mode&&!dense_haze);
 if(!need||!scratch||scratch_size<need||lens.focal_length<=0)return false;
 const float fov=std::clamp(settings.fov_h_deg,1.f,150.f)*.0174532925199433f;
 const float sx=lens.focal_length*std::tan(fov*.5f);
 const float sy=settings.fov_v_deg>0?
      lens.focal_length*std::tan(std::clamp(settings.fov_v_deg,1.f,150.f)*.00872664625997f):
      sx*h/float(w);
 if(!(sx>0&&sy>0)||!std::isfinite(sx+sy))return false;
 const auto selection=fl::cached_pairs(lens,sx,sy);
 const auto& active=selection->pairs;
 const auto& boosts=selection->boosts;
 const int count=std::min<int>(int(active.size()),settings.max_pairs);
 std::vector<GPUPair> pairs;
 pairs.reserve(std::max(0,count));
 for(int i=0;i<count;i++){
  const bool selected=active[i].surf_a==settings.pair_surface_a&&
                      active[i].surf_b==settings.pair_surface_b;
  if((settings.pair_mode==1&&!selected)||(settings.pair_mode==2&&selected))continue;
  pairs.push_back({active[i].surf_a,active[i].surf_b,boosts[i]});
 }
 // Borrow the immutable MFR-safe pupil instead of copying up to 2 MiB
 // from the cache on every CUDA frame. Source samples stay byte-identical.
 const auto pupil_storage=fl::cached_pupil(settings);
 const auto& pupil=*pupil_storage;
 const auto spectrum=fl::make_spectrum(settings);
 std::vector<GPUSource> lights;
 lights.reserve(sources.size());
 for(const auto& src:sources)
  lights.push_back({std::atan((src.x*2-1)*std::tan(fov*.5f)),
                    std::atan((src.y*2-1)*sy/lens.focal_length),
                    src.r,src.g,src.b,0,0,0});
 std::vector<GPUSample> sample_gpu;
 sample_gpu.reserve(pupil.size());
 for(const auto& p:pupil)sample_gpu.push_back({p.u,p.v});
 std::vector<GPUSpectralSampleDev> bands;
 bands.reserve(spectrum.size());
 for(const auto& s:spectrum)bands.push_back({s.lambda,s.r,s.g,s.b});
 auto* base=static_cast<unsigned char*>(scratch);
 auto* ds=reinterpret_cast<Surface*>(base);
 base+=align16(lens.surfaces.size()*sizeof(Surface));
 auto* dp=reinterpret_cast<GPUPair*>(base);
 base+=align16(std::min<size_t>(2048,lens.surfaces.size()*lens.surfaces.size())*sizeof(GPUPair));
 auto* dl=reinterpret_cast<GPUSource*>(base);
 base+=align16(std::max(sources.size(),haze_sources.size())*sizeof(GPUSource));
 auto* dg=reinterpret_cast<GPUSample*>(base);
 base+=align16(size_t(settings.grid)*settings.grid*sizeof(GPUSample));
 auto* dw=reinterpret_cast<GPUSpectralSampleDev*>(base);
 base+=align16(11*sizeof(GPUSpectralSampleDev));
 const bool own_photon=(ghost_mode&&(v.output_mode==1||need_blur))||dense_haze;
 auto* photon=own_photon?reinterpret_cast<float4*>(base):
                         reinterpret_cast<float4*>(v.output);
 if(own_photon)base+=align16(bytes);
 auto* temporary=(need_blur||dense_haze)?
                   reinterpret_cast<float4*>(base):nullptr;
 if(need_blur||dense_haze)base+=align16(bytes);
 // Profile arrays are used ONLY by the small-source analytic haze path.
 // Zero allocations on ghost-only and dense-haze renders save multiple
 // MiB per frame at high source counts.
 const bool analytic_haze=haze_mode&&!dense_haze;
 auto* haze_x=analytic_haze?reinterpret_cast<float*>(base):nullptr;
 if(analytic_haze)base+=align16(haze_sources.size()*size_t(w)*sizeof(float));
 auto* haze_y=analytic_haze?reinterpret_cast<float*>(base):nullptr;
 if(analytic_haze)base+=align16(haze_sources.size()*size_t(h)*sizeof(float));
 auto* haze_seeds=dense_haze?reinterpret_cast<GPUHazeSeed*>(base):nullptr;
 const auto upload=[&](void* dst,const void* data,size_t n){
  return cudaMemcpyAsync(dst,data,n,cudaMemcpyHostToDevice,stream)==cudaSuccess;
 };
 // Both ghost and analytic-haze kernels read the same tiny source RGB table.
 if(!upload(dl,lights.data(),lights.size()*sizeof(GPUSource)))return false;
 if(ghost_mode&&!pairs.empty()&&!pupil.empty()&&!spectrum.empty()){
  const dim3 source_grid((unsigned(lights.size())+255)/256);
  flarelens_prepare_source_directions<<<source_grid,256,0,stream>>>(
   dl,int(lights.size()));
  if(cudaPeekAtLastError()!=cudaSuccess)return false;
  if(own_photon&&cudaMemsetAsync(photon,0,bytes,stream)!=cudaSuccess)return false;
  if(!upload(ds,lens.surfaces.data(),lens.surfaces.size()*sizeof(Surface))||
     !upload(dp,pairs.data(),pairs.size()*sizeof(GPUPair))||
     !upload(dg,sample_gpu.data(),sample_gpu.size()*sizeof(GPUSample))||
     !upload(dw,bands.data(),bands.size()*sizeof(GPUSpectralSampleDev)))return false;
  const dim3 block(256);
  const bool parallel=v.spectral_parallel>=0?
    v.spectral_parallel!=0:spectrum.size()>=5;
  const int photon_pitch=own_photon?w*4:v.output_pitch_bytes/4;
  // Never enqueue 512*512²*11 rays for every HDR source as ONE enormous
  // uncancellable AE render. Divide the photon stream into bounded chunks;
  // optical equations and per-source normalization remain identical.
  // A single source uses the exact original upstream kernel launch.
  const int work_per_source=std::max(1,settings.grid*settings.grid);
  const int batch=std::clamp(65536/work_per_source,1,64);
  // At >512², a single kernel can occupy the device for seconds or longer
  // (especially with 11 wavelengths and many ghost pairs). Bound the work
  // per launch without altering pupil order, ray weights or accumulation.
  const size_t max_tile=settings.grid>512?
    std::clamp<size_t>(size_t(12'000'000)/
       std::max<size_t>(1,pairs.size()*spectrum.size()),
                       1024u,32768u):pupil.size();
  for(size_t start=0;start<lights.size();start+=batch){
   if(v.check_abort&&v.check_abort(v.abort_context))return false;
   const int count=std::min<int>(batch,int(lights.size()-start));
   const auto* batch_sources=dl+start;
   for(size_t offset=0;offset<pupil.size();offset+=max_tile){
    if(v.check_abort&&v.check_abort(v.abort_context))return false;
    const int n_tile=int(std::min(max_tile,pupil.size()-offset));
    const dim3 grid(unsigned(pairs.size()*count),
                    (unsigned(n_tile)+255)/256,
                    parallel?unsigned(spectrum.size()):1u);
    if(parallel){
     flarelens_spectral_kernel<<<grid,block,0,stream>>>(
      ds,int(lens.surfaces.size()),lens.sensor_z,dp,int(pairs.size()),
      batch_sources,count,dg+offset,n_tile,dw,int(spectrum.size()),
      lens.surfaces.front().semi_aperture,lens.surfaces.front().z-20.f,
      sx,sy,w,h,photon_pitch,settings.gain,1.f/float(pupil.size()),
      reinterpret_cast<float*>(photon));
    }else{
     flarelens_direct_kernel<<<grid,block,0,stream>>>(
      ds,int(lens.surfaces.size()),lens.sensor_z,dp,int(pairs.size()),
      batch_sources,count,dg+offset,n_tile,dw,int(spectrum.size()),
      lens.surfaces.front().semi_aperture,lens.surfaces.front().z-20.f,
      sx,sy,w,h,photon_pitch,settings.gain,1.f/float(pupil.size()),
      reinterpret_cast<float*>(photon));
    }
    if(cudaPeekAtLastError()!=cudaSuccess)return false;
    if((pupil.size()>max_tile||lights.size()>size_t(batch))&&
       cudaStreamSynchronize(stream)!=cudaSuccess)return false;
   }
  }
  // Small Nuke ghost blur (usually <=12 px): massively parallel,
  // coalesced pixel kernels. For unusual broad ghost blur use bounded
  // O(WH) sliding prefix windows instead of O(radius*pixels).
  auto blur=[&](int radius,int passes)->bool{
   radius=std::clamp(radius,0,std::max(w,h));
   passes=std::clamp(passes,0,6);
   if(radius<=0||passes==0)return true;
   const dim3 block(256);
   const dim3 parallel_pixels((unsigned(w)+255)/256,unsigned(h));
   const dim3 rows((unsigned(h)+255)/256,(unsigned(w)+127)/128);
   const dim3 cols((unsigned(w)+255)/256,(unsigned(h)+127)/128);
   for(int i=0;i<passes;i++){
    if(radius<=32){
     flarelens_blur_rows_small<<<parallel_pixels,block,0,stream>>>(
          photon,temporary,w,h,radius);
    }else{
     flarelens_blur_rows<<<rows,block,0,stream>>>(
          photon,temporary,w,h,radius);
    }
    if(cudaPeekAtLastError()!=cudaSuccess)return false;
    if(radius<=32){
     flarelens_blur_cols_small<<<parallel_pixels,block,0,stream>>>(
          temporary,photon,w,h,radius);
    }else{
     flarelens_blur_cols<<<cols,block,0,stream>>>(
          temporary,photon,w,h,radius);
    }
    if(cudaPeekAtLastError()!=cudaSuccess)return false;
   }
   return true;
  };
  const int radius=int(std::round(std::max(0.f,v.ghost_blur)*std::hypot(float(w),float(h))));
  if(!blur(radius,v.ghost_blur_passes))return false;
  const dim3 grid_add((unsigned(npx)+255)/256);
  if(own_photon){
   flarelens_add_light<<<grid_add,block,0,stream>>>(
    photon,static_cast<float4*>(v.output),v.output_pitch_bytes/16,w,h,1.f);
   if(cudaPeekAtLastError()!=cudaSuccess)return false;
  }
 }
 if(haze_mode&&dense_haze){
  // Many Nuke HDR blocks: splat their RGB into a single full-resolution
  // texture and convolve once, NOT one outer product per source/pixel.
  host_seeds.reserve(haze_sources.size());
  for(const auto& s:haze_sources){
    const auto rect=fl::nuke_haze_rect(s,w,h,v.haze_footprint);
    host_seeds.push_back({rect.x0,rect.y0,rect.x1,rect.y1,s.b,s.g,s.r});
  }
  if(!upload(haze_seeds,host_seeds.data(),
             host_seeds.size()*sizeof(GPUHazeSeed)))return false;
  if(cudaMemsetAsync(photon,0,bytes,stream)!=cudaSuccess)return false;
  const dim3 block(256),seed_grid((unsigned(haze_sources.size())+255)/256);
  flarelens_scatter_haze<<<seed_grid,block,0,stream>>>(
    photon,w,h,haze_seeds,int(haze_sources.size()),v.haze_footprint);
  if(cudaPeekAtLastError()!=cudaSuccess)return false;
  const int radius=int(std::round(std::clamp(v.haze_radius,0.f,1.f)*
                                  std::hypot(float(w),float(h))));
  const int passes=std::clamp(v.haze_passes,0,6);
  if(radius>0&&passes>0){
   const dim3 rows((unsigned(h)+255)/256,(unsigned(w)+127)/128);
   const dim3 cols((unsigned(w)+255)/256,(unsigned(h)+127)/128);
   for(int pass=0;pass<passes;pass++){
    flarelens_blur_rows<<<rows,block,0,stream>>>(
      photon,temporary,w,h,radius);
    if(cudaPeekAtLastError()!=cudaSuccess)return false;
    flarelens_blur_cols<<<cols,block,0,stream>>>(
      temporary,photon,w,h,radius);
    if(cudaPeekAtLastError()!=cudaSuccess)return false;
   }
  }
  const dim3 add_grid((unsigned(npx)+255)/256);
  flarelens_add_light<<<add_grid,block,0,stream>>>(
    photon,static_cast<float4*>(v.output),v.output_pitch_bytes/16,
    w,h,v.haze_gain);
  if(cudaPeekAtLastError()!=cudaSuccess)return false;
 }else if(haze_mode){
  // The 2D variable-denominator Nuke box blur is separable. For a point
  // source its convolution is the outer product of two 1D responses,
  // computed at O(w+h) host cost and accumulated with one CUDA kernel.
  const int radius=int(std::round(std::clamp(v.haze_radius,0.f,1.f)*
                                   std::hypot(float(w),float(h))));
  const int passes=std::clamp(v.haze_passes,1,6);
  host_x.assign(size_t(w)*haze_sources.size(),0.f);
  host_y.assign(size_t(h)*haze_sources.size(),0.f);
  for(size_t s=0;s<haze_sources.size();s++){
   const auto x=haze_profile(w,int(haze_sources[s].x*w),radius,passes);
   const auto y=haze_profile(h,int(haze_sources[s].y*h),radius,passes);
   std::copy(x.begin(),x.end(),host_x.begin()+size_t(w)*s);
   std::copy(y.begin(),y.end(),host_y.begin()+size_t(h)*s);
  }
  // Reuse the RGB source table only AFTER the optical trace has
  // completed on the same host stream. Nuke haze uses raw, pre-cap tiles.
  haze_lights.reserve(haze_sources.size());
  for(const auto& s:haze_sources)
   haze_lights.push_back({0.f,0.f,s.r,s.g,s.b,0.f,0.f,0.f});
  if(!upload(dl,haze_lights.data(),haze_lights.size()*sizeof(GPUSource))||
     !upload(haze_x,host_x.data(),host_x.size()*sizeof(float))||
     !upload(haze_y,host_y.data(),host_y.size()*sizeof(float)))return false;
  const dim3 block(256),grid_add((unsigned(npx)+255)/256);
  flarelens_add_analytic_haze<<<grid_add,block,0,stream>>>(
    static_cast<float4*>(v.output),v.output_pitch_bytes/16,w,h,dl,
    haze_x,haze_y,int(haze_sources.size()),v.haze_gain);
  if(cudaPeekAtLastError()!=cudaSuccess)return false;
 }
 if(v.output_mode==1){
  const dim3 block(256),grid_add((unsigned(npx)+255)/256);
  flarelens_composite_alpha_kernel<<<grid_add,block,0,stream>>>(
    static_cast<const float*>(v.input),v.input_pitch_bytes/4,
    static_cast<float*>(v.output),v.output_pitch_bytes/4,w,h);
  if(cudaPeekAtLastError()!=cudaSuccess)return false;
 }
 return sync.finish();
}
} // namespace fl::gpu
