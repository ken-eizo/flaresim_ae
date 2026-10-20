#pragma once
// Interpolated pupil mesh for high Ray Grid values (shared by CPU and CUDA).
//
// A Ray Grid of g traces g^2 photons per ghost pair and wavelength. Above
// kCells the extra photons mostly land between their neighbours: a ghost's
// pupil-to-sensor map is smooth, so the dense grid converges to one image.
// The mesh traces the (kCells+1)^2 pupil vertices exactly and then:
//  - interior cells (all four corners reach the sensor, no fold) emit
//    photons interpolated bilinearly from the corners, at the requested
//    density but never more than kSamplesPerPixel per covered pixel;
//  - cells crossing a vignetting / aperture edge or a caustic fold are
//    traced exactly, with as many photons as their size on the sensor needs
//    (kEdgeSamplesPerPixel / kFoldSamplesPerPixel, at most the requested
//    density), which keeps ghost outlines and caustic rims sharp.
// Each photon carries (pupil area it represents) / (pupil shape area), the
// dense-grid normalisation 1/N in the limit. Ray Grid <= kCells keeps the
// original per-photon tracing (and its Nuke parity) unchanged.
#include "FlareLensCore.hpp"
#include "FlareLensOptics.hpp"
#include <math.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#if defined(__CUDACC__)
#define FL_HD __host__ __device__
#else
#define FL_HD
#endif

namespace fl::mesh {
constexpr int kCells=256;            // mesh cells per pupil axis
constexpr int kRowsPerItem=16;       // CPU work item: 16 mesh rows
constexpr float kSamplesPerPixel=1.0f;
constexpr float kEdgeSamplesPerPixel=2.0f;
constexpr float kFoldSamplesPerPixel=4.0f;
constexpr float kPi=3.14159265358979323846f;

struct Plan {
 bool enabled=false;
 int density=1;              // requested photons per cell axis
 int edge_sub=1,fold_sub=1;  // exact photons per cell axis on edges / folds
 int blades=0;
 float rotate=0,sector=1,apothem=1;
 float cell=2.f/kCells;
 float cell_norm=0;          // cell pupil area / pupil shape area
 // cached_pupil()'s aperture test (unit disk or regular polygon).
 FL_HD bool inside(float u,float v) const {
  const float r2=u*u+v*v;
  if(r2>1)return false;
  if(blades>=3){
   float t=fmodf(atan2f(v,u)-rotate,sector);
   if(t<0)t+=sector;
   if(sqrtf(r2)*cosf(t-sector*.5f)>apothem)return false;
  }
  return true;
 }
};

// FLARESIM_MESH=0 forces exact per-photon tracing at every Ray Grid.
inline bool mesh_allowed(){
 static const bool allowed=[]{
  const char* env=std::getenv("FLARESIM_MESH");
  return !(env&&env[0]=='0');
 }();
 return allowed;
}
// True when this Ray Grid renders through the mesh (GPU scratch sizing).
inline bool uses_mesh(int grid){return std::clamp(grid,2,2048)>kCells&&mesh_allowed();}
inline Plan plan(const Settings& s){
 Plan p;
 const int g=std::clamp(s.grid,2,2048);
 p.enabled=!s.debug_ray_density&&uses_mesh(g);
 if(!p.enabled)return p;
 p.density=(g+kCells-1)/kCells;
 p.edge_sub=p.density;
 p.fold_sub=p.density;
 p.blades=std::clamp(s.aperture_blades,0,16);
 p.rotate=std::isfinite(s.aperture_rotation_deg)?s.aperture_rotation_deg*kPi/180.f:0;
 p.sector=p.blades>=3?2*kPi/p.blades:1;
 p.apothem=p.blades>=3?std::cos(kPi/p.blades):1;
 const double pi=3.14159265358979323846;
 const double area=p.blades>=3?.5*p.blades*std::sin(2*pi/p.blades):pi;
 p.cell_norm=float(double(p.cell)*p.cell/area);
 return p;
}

FL_HD inline PupilSample vertex(int i,int j){
 return {-1.f+float(i)*(2.f/kCells),-1.f+float(j)*(2.f/kCells)};
}

struct Corner {float px,py,weight;bool ok;};   // order: 00, 10, 01, 11
enum class CellKind {Empty,Interior,Edge,Fold};
struct Cell {CellKind kind;int sub;};

// Squared sensor spacing of the mesh around cell (i,j): the longest
// distance between horizontally or vertically adjacent reached vertices in
// vertex rows j and j+1, columns i-1..i+2 (row[r][0] is column i-1). Both
// renderers always hold these eight vertices, so CPU and CUDA derive the
// same edge sample counts. Negative when no adjacent pair reached the sensor.
FL_HD inline float spacing2(const Corner (&row)[2][4]){
 float d2=-1.f;
 for(int r=0;r<2;r++)for(int k=0;k<3;k++){
  if(!row[r][k].ok||!row[r][k+1].ok)continue;
  const float dx=row[r][k+1].px-row[r][k].px,dy=row[r][k+1].py-row[r][k].py;
  const float e=dx*dx+dy*dy;
  if(e>d2)d2=e;
 }
 for(int k=0;k<4;k++){
  if(!row[0][k].ok||!row[1][k].ok)continue;
  const float dx=row[1][k].px-row[0][k].px,dy=row[1][k].py-row[0][k].py;
  const float e=dx*dx+dy*dy;
  if(e>d2)d2=e;
 }
 return d2;
}
// ceil(x) for 0 <= x < 2^31 (the CRT ceilf is an out-of-line call on MSVC).
FL_HD inline int ceil_int(float x){
 const int t=int(x);
 return t+(float(t)<x?1:0);
}
// Exact photons per axis for an edge or fold cell: enough for the sensor
// spacing of the vertices around it (see spacing2), at most the density.
FL_HD inline int exact_sub(const Plan& plan,CellKind kind,const Corner (&row)[2][4]){
 const bool fold=kind==CellKind::Fold;
 const int cap=fold?plan.fold_sub:plan.edge_sub;
 const float d2=spacing2(row);
 if(!(d2>=0.f))return cap;
 const float wanted=sqrtf((fold?kFoldSamplesPerPixel:kEdgeSamplesPerPixel)*d2);
 if(!(wanted<float(cap)))return cap;
 const int sub=ceil_int(wanted);
 return sub<2?2:sub;
}

// Interior cells get their photon count here. Edge and fold cells are only
// tagged: the caller gathers the vertices around them for exact_sub().
FL_HD inline Cell classify(const Plan& plan,const Corner* c){
 const int ok=int(c[0].ok)+int(c[1].ok)+int(c[2].ok)+int(c[3].ok);
 if(ok==0)return {CellKind::Empty,0};
 if(ok<4)return {CellKind::Edge,0};
 // Twice the signed areas of triangles 00-10-11 and 00-11-01 on the sensor.
 const float ax=c[1].px-c[0].px,ay=c[1].py-c[0].py;
 const float bx=c[3].px-c[0].px,by=c[3].py-c[0].py;
 const float cx=c[2].px-c[0].px,cy=c[2].py-c[0].py;
 const float t1=ax*by-ay*bx,t2=bx*cy-by*cx;
 if(!((t1>0&&t2>0)||(t1<0&&t2<0)))return {CellKind::Fold,0};
 const float area=.5f*(fabsf(t1)+fabsf(t2));
 const int cap=plan.density*plan.density;
 const float want=kSamplesPerPixel*area;
 int wanted=want<float(cap)?ceil_int(want):cap;
 if(wanted<1)wanted=1;
 int sub=1;                       // ceil(sqrt(wanted)) <= density
 while(sub*sub<wanted)sub++;
 return {CellKind::Interior,sub};
}


struct Point {float px,py,weight;};
FL_HD inline Point interpolate(const Corner* c,float fa,float fb){
 const float w00=(1.f-fa)*(1.f-fb),w10=fa*(1.f-fb),w01=(1.f-fa)*fb,w11=fa*fb;
 return {c[0].px*w00+c[1].px*w10+c[2].px*w01+c[3].px*w11,
         c[0].py*w00+c[1].py*w10+c[2].py*w01+c[3].py*w11,
         c[0].weight*w00+c[1].weight*w10+c[2].weight*w01+c[3].weight*w11};
}

struct ExactSample {float u,v,norm;int q;};
// Sub-cell centres of cell (i,j) inside the aperture, for exact tracing.
inline void queue_exact(const Plan& plan,int i,int j,int q,int sub,
                        std::vector<ExactSample>& out){
 const PupilSample origin=vertex(i,j);
 const float norm=plan.cell_norm/float(sub*sub);
 for(int b=0;b<sub;b++){
  const float v=origin.v+plan.cell*(b+.5f)/sub;
  for(int a=0;a<sub;a++){
   const float u=origin.u+plan.cell*(a+.5f)/sub;
   if(plan.inside(u,v))out.push_back({u,v,norm,q});
  }
 }
}
} // namespace fl::mesh
