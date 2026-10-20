#include "FlareLensCpuParallel.hpp"
#include "FlareLensCpuTrace.hpp"
#include "FlareLensMesh.hpp"
#include "FlareLensOptics.hpp"
#include "FlareLensThreadPool.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#if defined(_M_X64)||defined(_M_IX86)||defined(__SSE2__)
#include <emmintrin.h>
#define FLARELENS_SPLAT_SSE2 1
#endif

// Threaded CPU ghost renderer.
//
// Work is split into items and run on the shared worker pool in two phases
// per round:
//  1. trace: every item traces its rays and stores each sensor hit as a
//     record, grouped by the horizontal row band it lands in;
//  2. splat: every band is owned by one thread, which adds the records of
//     all items in item order. No atomics, no per-thread frame copies.
// Items are numbered in render_cpu()'s loop order (source, pair, samples,
// wavelength) and each band replays them in that order, so the result is
// deterministic and independent of the thread count. Samples are visited in
// Morton order, so sums differ from render_cpu() only by float rounding.
//
// Pairs that share their second bounce b are traced together (see
// cpu::GroupInput): the path to b and back is computed once per group and
// every pair branches off at its first bounce a, so an item is (source,
// group, samples) and lists its pairs in render_cpu()'s pair order.
//
// Ray Grid above mesh::kCells uses the interpolated pupil mesh described in
// FlareLensMesh.hpp; an item is then (source, group, band of mesh rows).
namespace fl {
namespace {
// rows: bit 0 = add to row iy, bit 1 = add to row iy+1, iy = floor(py-.5).
struct Record {float px,py,e[3];uint32_t rows;};
// An interior mesh cell. Its photons are generated only while a band is
// splatted, instead of storing up to 64 photon records per cell.
struct CellRecord {
 mesh::Corner corner[4];
 float color[3];           // light rgb * spectral weight
 float gain,boost,norm;    // photon base = weight*(gain*boost*norm)
 int sub;
};
struct ItemResult {
 std::vector<Record> records;      // grouped by band, item order inside
 std::vector<uint32_t> band_start; // bands+1 offsets into records
 std::vector<CellRecord> cells;    // mesh items: interior cells, in order
 std::vector<uint32_t> cell_index; // cells grouped by band
 std::vector<uint32_t> cell_start; // bands+1 offsets into cell_index
 uint64_t traced=0,valid=0,hits=0;
};
struct Light {Vec3f dir;float rgb[3];};
struct Scratch {
 std::vector<uint8_t> valid;
 std::vector<float> x,y,w;
 std::vector<Record> raw;
 std::vector<uint32_t> key,next;
 std::vector<uint32_t> cell_key,cell_ref; // (band, cell) pairs of mesh cells
};
struct MeshScratch {
 std::vector<PupilSample> vertex,inside_vertex;
 std::vector<int> inside_index;
 std::vector<uint8_t> inside,ok;
 std::vector<float> px,py;
 std::vector<mesh::ExactSample> exact;
 std::vector<PupilSample> batch;
 std::vector<float> batch_norm;
};
// Without /arch:AVX2, MSVC compiles std::isfinite and std::floor to CRT
// calls (_fdclass, floorf). These exact inline versions run per photon.
inline bool finite(float x){
 uint32_t bits;
 std::memcpy(&bits,&x,sizeof bits);
 return (bits&0x7f800000u)!=0x7f800000u;
}
inline int floor_int(float x){ // |x| < 2^31
 const int t=int(x);
 return t-(float(t)>x?1:0);
}
// A photon's channel energy as the splat adds it: dropped (zero) unless it
// is above 1e-14 and finite. Bilinear weights are in [0,1], so a finite
// energy gives finite products.
inline float splat_energy(float e){
 return e>1e-14f&&e<=std::numeric_limits<float>::max()?e:0.f;
}
// (k+0.5)/sub for the photon lattice of an interior cell, sub <= 8.
struct LatticeTable {
 float f[9][8]={};
 LatticeTable(){for(int s=1;s<=8;s++)for(int k=0;k<s;k++)f[s][k]=(k+.5f)/s;}
};
const LatticeTable kLattice;
constexpr int kChunk=512;                  // pupil samples per exact item
constexpr size_t kGroupPairs=32;           // pairs per traced group
constexpr size_t kMeshGroupPairs=32;       // pairs per mesh item (vertices of one wavelength)
constexpr size_t kRecordBudget=size_t(2)<<20;

// Item buffers are leased across frames (MFR-safe): regrowing hundreds of
// record and cell vectors on every render would page-fault each frame.
using Results=std::vector<ItemResult>;
std::mutex results_mutex;
std::vector<std::unique_ptr<Results>> results_free;
std::unique_ptr<Results> lease_results(){
 std::lock_guard<std::mutex> lock(results_mutex);
 if(results_free.empty())return std::make_unique<Results>();
 auto results=std::move(results_free.back());
 results_free.pop_back();
 return results;
}
void return_results(std::unique_ptr<Results> results) noexcept {
 size_t bytes=0;
 for(const auto& item:*results)
  bytes+=item.records.capacity()*sizeof(Record)+item.cells.capacity()*sizeof(CellRecord)+
         (item.cell_index.capacity()+item.cell_start.capacity()+
          item.band_start.capacity())*sizeof(uint32_t);
 if(bytes>(size_t(256)<<20))return; // unusually large frame: release it
 try{
  std::lock_guard<std::mutex> lock(results_mutex);
  if(results_free.size()<4)results_free.push_back(std::move(results));
 }catch(...){}
}
struct ResultsLease {
 std::unique_ptr<Results> results=lease_results();
 ~ResultsLease(){return_results(std::move(results));}
};
// A pooled frame buffer that goes back to the pool on every exit path.
struct BufferLease {
 std::vector<float> data;
 ~BufferLease(){if(!data.empty())release_buffer(std::move(data));}
};

bool use_avx2(){
#if defined(FLARELENS_HAVE_AVX2_TRACE)
 static const bool enabled=[]{
  const char* env=std::getenv("FLARESIM_SIMD");
  if(env&&env[0]=='0')return false;
  return cpu::avx2_supported();
 }();
 return enabled;
#else
 return false;
#endif
}
} // namespace

Image render_sources_parallel(const LensSystem& lens,
 const std::vector<Source>& sources,const Settings& s,
 int w,int h,RenderControl control,int requested_threads){
 // AE cancellation belongs on the render thread; honour an already
 // cancelled frame before launching any asynchronous optical tasks.
 if(control.check_abort){
  if(const int code=control.check_abort(control.context)){
   Image aborted{};aborted.width=w;aborted.height=h;
   aborted.abort_error=code;return aborted;
  }
 }
 if(requested_threads==1||thread_count()<=1)
  return render_cpu(lens,sources,s,w,h,control);
 // Identical early exits and setup to render_cpu().
 Image out{w,h,{}};
 if(w<1||h<1||w>16384||h>16384||lens.num_surfaces()<2||
    sources.empty()||lens.focal_length<=0||s.max_pairs<=0)return out;
 if(size_t(w)*h>size_t(16384)*16384)return out;
 if(!s.debug_ray_density&&!(s.gain>0))return out;
 const float fov_h=std::clamp(s.fov_h_deg,1.f,150.f)*.0174532925199433f;
 const float sx=lens.focal_length*std::tan(fov_h*.5f);
 const float sy=s.fov_v_deg>0?
    lens.focal_length*std::tan(std::clamp(s.fov_v_deg,1.f,150.f)*.00872664625997f):
    sx*h/float(w);
 if(!(sx>0&&sy>0)||!std::isfinite(sx+sy))return out;
 const float gain=std::max(0.f,s.gain);
 const auto selection=cached_pairs(lens,sx,sy);
 const auto& pairs=selection->pairs;
 const auto& boosts=selection->boosts;
 const int np=std::min<int>(int(pairs.size()),s.max_pairs);
 if(np<1)return out;
 const mesh::Plan plan=mesh::plan(s);
 // Morton order: each 8-wide batch traces one compact pupil patch. Only the
 // order of identical additions changes (float rounding, ~1e-7 relative).
 // The mesh path never touches the dense pupil list.
 const auto pupil_storage=plan.enabled?nullptr:coherent_pupil(cached_pupil(s));
 const auto spectrum=make_spectrum(s);
 if((!plan.enabled&&pupil_storage->empty())||spectrum.empty())return out;
 out.rgb=acquire_buffer(size_t(w)*h*3);
 // Mesh frames splat tens of millions of photons. Up to about 4K they
 // accumulate in RGBA, one 4-wide add per pixel instead of three dependent
 // scalar adds, and are packed into out.rgb at the end (which therefore
 // needs no clearing). Larger frames skip the extra 16-byte-per-pixel buffer.
 const bool use_rgba=plan.enabled&&size_t(w)*h<=size_t(4096)*2304;
 BufferLease rgba;
 if(use_rgba)rgba.data=acquire_buffer(size_t(w)*h*4);
 parallel_zero(use_rgba?rgba.data:out.rgb);
 const float normalization=plan.enabled?0.f:1.f/float(pupil_storage->size());
 const float tan_h=std::tan(fov_h*.5f);

 std::vector<Light> lights;
 lights.reserve(sources.size());
 for(const auto& light:sources){
  const float ax=std::atan((light.x*2.f-1.f)*tan_h);
  const float ay=std::atan((light.y*2.f-1.f)*sy/lens.focal_length);
  if(!std::isfinite(ax+ay))continue;
  lights.push_back({Vec3f(std::tan(ax),std::tan(ay),1.f).normalized(),
                    {light.r,light.g,light.b}});
 }
 std::vector<int> active;
 for(int p=0;p<np;p++){
  const bool selected=pairs[p].surf_a==s.pair_surface_a&&
                      pairs[p].surf_b==s.pair_surface_b;
  if((s.pair_mode==1&&!selected)||(s.pair_mode==2&&selected))continue;
  active.push_back(p);
 }
 out.diagnostics.active_pairs=int(lights.size()*active.size());
 const int n_surfaces=lens.num_surfaces();
 // Groups list their pairs by decreasing first bounce a; the largest come
 // first so the pool starts the longest items early.
 struct PairGroup {std::vector<int> pair,a;};
 std::vector<PairGroup> groups;
 {
  const size_t limit=plan.enabled?kMeshGroupPairs:kGroupPairs;
  std::vector<std::vector<int>> by_b(size_t(std::max(1,n_surfaces)));
  for(const int p:active)by_b[size_t(pairs[p].surf_b)].push_back(p);
  for(auto& list:by_b){
   std::stable_sort(list.begin(),list.end(),[&](int x,int y){
    return pairs[x].surf_a>pairs[y].surf_a;});
   for(size_t i=0;i<list.size();i+=limit){
    PairGroup group;
    for(size_t k=i;k<std::min(list.size(),i+limit);k++){
     group.pair.push_back(list[k]);group.a.push_back(pairs[list[k]].surf_a);
    }
    groups.push_back(std::move(group));
   }
  }
  std::stable_sort(groups.begin(),groups.end(),[](const PairGroup& x,const PairGroup& y){
   return x.pair.size()>y.pair.size();});
 }
 const size_t max_group=groups.empty()?1:groups.front().pair.size();
 const int bands_wanted=std::clamp(thread_count()*4,1,std::max(1,(h+7)/8));
 const int rows_per_band=(h+bands_wanted-1)/bands_wanted;
 const int bands=(h+rows_per_band-1)/rows_per_band;
 const int Q=int(spectrum.size());
 const int chunks=plan.enabled?mesh::kCells/mesh::kRowsPerItem:
    int((pupil_storage->size()+kChunk-1)/kChunk);
 const size_t total=lights.size()*groups.size()*size_t(chunks);

 std::vector<std::vector<float>> ior(Q,std::vector<float>(n_surfaces));
 for(int q=0;q<Q;q++)for(int k=0;k<n_surfaces;k++)
  ior[q][k]=lens.surfaces[k].ior_at(spectrum[q].lambda);

 const bool simd=use_avx2();
 const std::thread::id render_thread=std::this_thread::get_id();
 std::atomic<int> abort_code{0};
 const auto poll_abort=[&]{
  if(control.check_abort&&std::this_thread::get_id()==render_thread&&
     abort_code.load(std::memory_order_relaxed)==0){
   if(const int code=control.check_abort(control.context))
    abort_code.store(code,std::memory_order_relaxed);
  }
  return abort_code.load(std::memory_order_relaxed)!=0;
 };

 const auto input=[&](int p,const Light& light,int q){
  cpu::TraceInput in;
  in.surfaces=lens.surfaces.data();in.n=n_surfaces;in.sensor_z=lens.sensor_z;
  in.a=pairs[p].surf_a;in.b=pairs[p].surf_b;
  in.lambda=spectrum[q].lambda;in.ior=ior[q].data();in.dir=light.dir;
  in.front_radius=lens.surfaces.front().semi_aperture;
  in.start_z=lens.surfaces.front().z-20.f;
  return in;
 };
 const auto trace=[&](const cpu::TraceInput& in,const PupilSample* samples,int n,
                      uint8_t* valid,float* x,float* y,float* wt){
#if defined(FLARELENS_HAVE_AVX2_TRACE)
  if(simd){cpu::trace_batch_avx2(in,samples,n,valid,x,y,wt);return;}
#endif
  cpu::trace_batch_scalar(in,samples,n,valid,x,y,wt);
 };
 const auto group_input=[&](const PairGroup& group,const Light& light,int q){
  cpu::GroupInput in;
  in.base=input(group.pair.front(),light,q);
  in.a=group.a.data();in.count=int(group.a.size());
  return in;
 };
 // Pair j of the group goes to valid/x/y/wt[j*stride+k].
 const auto trace_group=[&](const cpu::GroupInput& in,const PupilSample* samples,int n,
                            int stride,uint8_t* valid,float* x,float* y,float* wt){
#if defined(FLARELENS_HAVE_AVX2_TRACE)
  if(simd){cpu::trace_group_avx2(in,samples,n,stride,valid,x,y,wt);return;}
#endif
  cpu::trace_group_scalar(in,samples,n,stride,valid,x,y,wt);
 };
 // One photon that reached the sensor: same filters and energy as
 // render_cpu(), then routed to the band(s) owning its two pixel rows.
 const auto emit=[&](Scratch& sc,ItemResult& r,float px,float py,float base,
                     const Light& light,int q){
  if(!finite(px)||!finite(py)||px<-2||py<-2||
     px>=w+2||py>=h+2)return;
  ++r.hits;
  if(!finite(base)||base<=0)return;
  const auto& spec=spectrum[q];
  Record rec{px,py,{
    s.debug_ray_density?base:light.rgb[0]*spec.r*base,
    s.debug_ray_density?base:light.rgb[1]*spec.g*base,
    s.debug_ray_density?base:light.rgb[2]*spec.b*base},0};
  const int ix=floor_int(px-.5f),iy=floor_int(py-.5f);
  if(ix+1<0||ix>=w)return;
  const bool top=iy>=0&&iy<h,bottom=iy+1>=0&&iy+1<h;
  if(top&&bottom&&iy/rows_per_band==(iy+1)/rows_per_band){
   rec.rows=3;sc.raw.push_back(rec);sc.key.push_back(uint32_t(iy/rows_per_band));
   return;
  }
  if(top){
   rec.rows=1;sc.raw.push_back(rec);sc.key.push_back(uint32_t(iy/rows_per_band));
  }
  if(bottom){
   rec.rows=2;sc.raw.push_back(rec);sc.key.push_back(uint32_t((iy+1)/rows_per_band));
  }
 };
 // Stable counting sort by band keeps the original order inside each band.
 const auto finish=[&](Scratch& sc,ItemResult& r){
  for(const uint32_t key:sc.key)r.band_start[key+1]++;
  for(int b=0;b<bands;b++)r.band_start[b+1]+=r.band_start[b];
  r.records.resize(sc.raw.size());
  sc.next.assign(r.band_start.begin(),r.band_start.end()-1);
  for(size_t i=0;i<sc.raw.size();i++)r.records[sc.next[sc.key[i]]++]=sc.raw[i];
  if(sc.cell_key.empty())return;
  for(const uint32_t key:sc.cell_key)r.cell_start[key+1]++;
  for(int b=0;b<bands;b++)r.cell_start[b+1]+=r.cell_start[b];
  r.cell_index.resize(sc.cell_ref.size());
  sc.next.assign(r.cell_start.begin(),r.cell_start.end()-1);
  for(size_t i=0;i<sc.cell_ref.size();i++)
   r.cell_index[sc.next[sc.cell_key[i]]++]=sc.cell_ref[i];
 };

 const auto exact_item=[&](size_t index,ItemResult& r){
  thread_local Scratch scratch;
  auto& sc=scratch;
  const auto& pupil=*pupil_storage;
  const int chunk=int(index%size_t(chunks));
  const size_t rest=index/size_t(chunks);
  const PairGroup& group=groups[rest%groups.size()];
  const Light& light=lights[rest/groups.size()];
  const int m=int(group.pair.size());
  const int k0=chunk*kChunk;
  const int n=int(std::min<size_t>(kChunk,pupil.size()-size_t(k0)));
  // Layout: [(pair*Q+q)*n+sample].
  const size_t lanes=size_t(n)*Q*m;
  sc.valid.resize(lanes);sc.x.resize(lanes);sc.y.resize(lanes);sc.w.resize(lanes);
  for(int q=0;q<Q;q++){
   const size_t o=size_t(q)*n;
   trace_group(group_input(group,light,q),pupil.data()+k0,n,Q*n,sc.valid.data()+o,
               sc.x.data()+o,sc.y.data()+o,sc.w.data()+o);
  }
  sc.raw.clear();sc.key.clear();sc.cell_key.clear();sc.cell_ref.clear();
  for(int j=m-1;j>=0;j--){ // increasing a: render_cpu()'s pair order
   const float boost=boosts[group.pair[size_t(j)]];
   for(int k=0;k<n;k++){
    for(int q=0;q<Q;q++){
     const size_t o=(size_t(j)*Q+q)*n+k;
     ++r.traced;
     if(!sc.valid[o]||!finite(sc.x[o])||!finite(sc.y[o]))continue;
     ++r.valid;
     const float px=(sc.x[o]/(2*sx)+.5f)*w;
     const float py=(sc.y[o]/(2*sy)+.5f)*h;
     const float base=s.debug_ray_density?1.f:(sc.w[o]*gain*boost*normalization);
     emit(sc,r,px,py,base,light,q);
    }
   }
  }
  finish(sc,r);
 };

 // Interpolated pupil mesh for very high Ray Grid (see FlareLensMesh.hpp).
 const auto mesh_item=[&](size_t index,ItemResult& r){
  thread_local Scratch scratch;
  thread_local MeshScratch mesh_scratch;
  auto& sc=scratch;
  auto& ms=mesh_scratch;
  const int chunk=int(index%size_t(chunks));
  const size_t rest=index/size_t(chunks);
  const PairGroup& group=groups[rest%groups.size()];
  const Light& light=lights[rest/groups.size()];
  const int m=int(group.pair.size());
  const int j0=chunk*mesh::kRowsPerItem;
  constexpr int V=mesh::kCells+1;
  constexpr int nv=(mesh::kRowsPerItem+1)*V;
  ms.vertex.resize(nv);ms.inside.resize(nv);
  for(int jj=0;jj<=mesh::kRowsPerItem;jj++){
   for(int i=0;i<V;i++){
    const PupilSample vertex=mesh::vertex(i,j0+jj);
    ms.vertex[size_t(jj)*V+i]=vertex;
    ms.inside[size_t(jj)*V+i]=plan.inside(vertex.u,vertex.v)?1:0;
   }
  }
  // Only vertices inside the aperture are traced (the square mesh also
  // covers the corners outside the pupil disk).
  ms.inside_vertex.clear();ms.inside_index.clear();
  for(int k=0;k<nv;k++)if(ms.inside[k]){
   ms.inside_vertex.push_back(ms.vertex[k]);ms.inside_index.push_back(k);
  }
  const int ni=int(ms.inside_vertex.size());
  sc.raw.clear();sc.key.clear();
  sc.cell_key.clear();sc.cell_ref.clear();
  // One wavelength at a time: the group's vertices are traced together
  // ([pair*ni+vertex]), then each pair's cells, by increasing a.
  thread_local std::vector<uint8_t> tv;
  thread_local std::vector<float> tx,ty,tw;
  const size_t lanes=size_t(ni)*m;
  tv.resize(lanes);tx.resize(lanes);ty.resize(lanes);tw.resize(lanes);
  ms.ok.resize(nv);ms.px.resize(nv);ms.py.resize(nv);
  for(int q=0;q<Q;q++){
   if(ni)trace_group(group_input(group,light,q),ms.inside_vertex.data(),ni,ni,
                     tv.data(),tx.data(),ty.data(),tw.data());
   r.traced+=uint64_t(ni)*m;
   const auto& spec=spectrum[q];
   for(int pj=m-1;pj>=0;pj--){ // increasing a: render_cpu()'s pair order
    const int p=group.pair[size_t(pj)];
    const float boost=boosts[p];
    sc.valid.assign(size_t(nv),0);sc.x.resize(size_t(nv));
    sc.y.resize(size_t(nv));sc.w.resize(size_t(nv));
    const size_t t0=size_t(pj)*ni;
    for(int k=0;k<ni;k++){
     const size_t d=size_t(ms.inside_index[k]);
     sc.valid[d]=tv[t0+k];sc.x[d]=tx[t0+k];sc.y[d]=ty[t0+k];sc.w[d]=tw[t0+k];
    }
    for(int k=0;k<nv;k++){
     const float px=(sc.x[k]/(2*sx)+.5f)*w;
     const float py=(sc.y[k]/(2*sy)+.5f)*h;
     ms.px[k]=px;ms.py[k]=py;
     ms.ok[k]=uint8_t(ms.inside[k]&&sc.valid[k]&&
                      finite(sc.x[k])&&finite(sc.y[k])&&finite(px)&&finite(py));
    }
    ms.exact.clear();
    for(int jj=0;jj<mesh::kRowsPerItem;jj++){
     for(int i=0;i<mesh::kCells;i++){
      const int c[4]={jj*V+i,jj*V+i+1,(jj+1)*V+i,(jj+1)*V+i+1}; // 00,10,01,11
      mesh::Corner corner[4];
      for(int k=0;k<4;k++)corner[k]={ms.px[c[k]],ms.py[c[k]],sc.w[c[k]],ms.ok[c[k]]!=0};
      const auto cell=mesh::classify(plan,corner);
      if(cell.kind==mesh::CellKind::Empty)continue;
      if(cell.kind!=mesh::CellKind::Interior){
       mesh::Corner around[2][4];
       for(int r2=0;r2<2;r2++)for(int k=0;k<4;k++){
        const int col=i-1+k;
        if(col<0||col>=V){around[r2][k]={0.f,0.f,0.f,false};continue;}
        const size_t idx=size_t(jj+r2)*V+col;
        around[r2][k]={ms.px[idx],ms.py[idx],sc.w[idx],ms.ok[idx]!=0};
       }
       mesh::queue_exact(plan,i,j0+jj,q,mesh::exact_sub(plan,cell.kind,around),ms.exact);
       continue;
      }
      ++r.valid;
      // Interpolated photons stay inside the corners' bounding box.
      float x0=corner[0].px,x1=x0,y0=corner[0].py,y1=y0;
      for(int k=1;k<4;k++){
       x0=std::min(x0,corner[k].px);x1=std::max(x1,corner[k].px);
       y0=std::min(y0,corner[k].py);y1=std::max(y1,corner[k].py);
      }
      if(x1<-2||y1<-2||x0>=w+2||y0>=h+2)continue;
      const int row0=std::max(0,floor_int(std::max(y0,-2.f)-.5f));
      const int row1=std::min(h-1,floor_int(std::min(y1,h+2.f)-.5f)+1);
      if(row0>row1)continue;
      CellRecord record;
      for(int k=0;k<4;k++)record.corner[k]=corner[k];
      record.color[0]=light.rgb[0]*spec.r;
      record.color[1]=light.rgb[1]*spec.g;
      record.color[2]=light.rgb[2]*spec.b;
      record.gain=gain;record.boost=boost;
      record.norm=plan.cell_norm/float(cell.sub*cell.sub);
      record.sub=cell.sub;
      const uint32_t ref=uint32_t(r.cells.size());
      r.cells.push_back(record);
      r.hits+=uint64_t(cell.sub)*cell.sub;
      for(int band=row0/rows_per_band;band<=row1/rows_per_band;band++){
       sc.cell_key.push_back(uint32_t(band));sc.cell_ref.push_back(ref);
      }
     }
    }
    // Edge and fold cells: exact rays at the requested density, 8 at a time.
    const int n=int(ms.exact.size());
    if(!n)continue;
    ms.batch.clear();ms.batch_norm.clear();
    for(const auto& e:ms.exact){ms.batch.push_back({e.u,e.v});ms.batch_norm.push_back(e.norm);}
    sc.valid.resize(size_t(n));sc.x.resize(size_t(n));sc.y.resize(size_t(n));sc.w.resize(size_t(n));
    trace(input(p,light,q),ms.batch.data(),n,sc.valid.data(),sc.x.data(),sc.y.data(),sc.w.data());
    r.traced+=uint64_t(n);
    for(int k=0;k<n;k++){
     if(!sc.valid[k]||!finite(sc.x[k])||!finite(sc.y[k]))continue;
     ++r.valid;
     const float px=(sc.x[k]/(2*sx)+.5f)*w;
     const float py=(sc.y[k]/(2*sy)+.5f)*h;
     emit(sc,r,px,py,sc.w[k]*gain*boost*ms.batch_norm[k],light,q);
    }
   }
  }
  finish(sc,r);
 };

 const auto trace_item=[&](size_t index,ItemResult& r){
  r.records.clear();r.cells.clear();r.cell_index.clear();
  r.traced=r.valid=r.hits=0;
  r.band_start.assign(size_t(bands)+1,0);
  r.cell_start.assign(size_t(bands)+1,0);
  if(poll_abort())return;
  if(plan.enabled)mesh_item(index,r);
  else exact_item(index,r);
 };

 float* const image=out.rgb.data();
 float* const accum=use_rgba?rgba.data.data():nullptr;
 ResultsLease lease;
 Results& results=*lease.results;
 // Adds a photon's bilinear footprint to the rows [y0,y1) it may touch.
 const auto splat=[&](float px,float py,const float* e,int y0,int y1){
  const int ix=floor_int(px-.5f),iy=floor_int(py-.5f);
  if(ix+1<0||ix>=w)return;
  const float fx=px-.5f-ix,fy=py-.5f-iy;
  for(int dy=0;dy<2;dy++){
   const int y=iy+dy;
   if(y<y0||y>=y1)continue;
   const float wy=dy?fy:1.f-fy;
   float* row=image+size_t(y)*w*3;
   if(ix>=0){
    float* o=row+size_t(ix)*3;const float wt=wy*(1.f-fx);
    o[0]+=e[0]*wt;o[1]+=e[1]*wt;o[2]+=e[2]*wt;
   }
   if(ix+1<w){
    float* o=row+size_t(ix+1)*3;const float wt=wy*fx;
    o[0]+=e[0]*wt;o[1]+=e[1]*wt;o[2]+=e[2]*wt;
   }
  }
 };
 // The same footprint added to the RGBA accumulator; per channel the
 // arithmetic (o += e*wt in float) is identical.
 const auto splat4=[&](float px,float py,const float* e,int y0,int y1){
  const int ix=floor_int(px-.5f),iy=floor_int(py-.5f);
  if(ix+1<0||ix>=w)return;
  const float fx=px-.5f-ix,fy=py-.5f-iy;
#if defined(FLARELENS_SPLAT_SSE2)
  const __m128 energy=_mm_setr_ps(e[0],e[1],e[2],0.f);
#endif
  for(int dy=0;dy<2;dy++){
   const int y=iy+dy;
   if(y<y0||y>=y1)continue;
   const float wy=dy?fy:1.f-fy;
   float* row=accum+size_t(y)*w*4;
   for(int dx=0;dx<2;dx++){
    const int x=ix+dx;
    if(x<0||x>=w)continue;
    const float wt=wy*(dx?fx:1.f-fx);
    float* o=row+size_t(x)*4;
#if defined(FLARELENS_SPLAT_SSE2)
    _mm_storeu_ps(o,_mm_add_ps(_mm_loadu_ps(o),_mm_mul_ps(energy,_mm_set1_ps(wt))));
#else
    o[0]+=e[0]*wt;o[1]+=e[1]*wt;o[2]+=e[2]*wt;
#endif
   }
  }
 };
 const float x_end=float(w)+2.f,y_end=float(h)+2.f;
 const auto splat_band=[&](int band,size_t count){
  const int band_y0=band*rows_per_band,band_y1=std::min(h,band_y0+rows_per_band);
  // A photon at py adds to rows floor(py-.5) and the next one.
  // The 0.01 px margin covers the rounding of the lerps below.
  const float reach_lo=float(band_y0)-.51f,reach_hi=float(band_y1)+.51f;
  for(size_t i=0;i<count;i++){
   const auto& r=results[i];
   // Interior mesh cells: the photons, filters and splat that emit() would
   // have recorded, restricted to the rows of this band. The photons of one
   // lattice row lie between the cell's left and right edges, so rows that
   // cannot reach the band are skipped whole.
   for(uint32_t k=r.cell_start[band];k<r.cell_start[band+1];k++){
    const CellRecord& cell=r.cells[r.cell_index[k]];
    const mesh::Corner* c=cell.corner;
    const float* f=kLattice.f[cell.sub];
    const float scale=cell.gain*cell.boost*cell.norm;
    for(int b=0;b<cell.sub;b++){
     const float fb=f[b],gb=1.f-fb;
     const float ly=c[0].py*gb+c[2].py*fb,ry=c[1].py*gb+c[3].py*fb;
     if((ly<ry?ry:ly)<reach_lo||(ly<ry?ly:ry)>=reach_hi)continue;
     const float lx=c[0].px*gb+c[2].px*fb,rx=c[1].px*gb+c[3].px*fb;
     const float lw=c[0].weight*gb+c[2].weight*fb,rw=c[1].weight*gb+c[3].weight*fb;
     for(int a=0;a<cell.sub;a++){
      const float fa=f[a],ga=1.f-fa;
      const float px=lx*ga+rx*fa,py=ly*ga+ry*fa;
      if(!(px>=-2.f&&py>=-2.f&&px<x_end&&py<y_end))continue; // also NaN
      const float base=(lw*ga+rw*fa)*scale;
      if(!(base>0.f&&base<=std::numeric_limits<float>::max()))continue;
      const float e[3]={splat_energy(cell.color[0]*base),
                        splat_energy(cell.color[1]*base),
                        splat_energy(cell.color[2]*base)};
      if(accum)splat4(px,py,e,band_y0,band_y1);
      else splat(px,py,e,band_y0,band_y1);
     }
    }
   }
   for(uint32_t k=r.band_start[band];k<r.band_start[band+1];k++){
    const Record& rec=r.records[k];
    const float e[3]={splat_energy(rec.e[0]),splat_energy(rec.e[1]),
                      splat_energy(rec.e[2])};
    const int iy=floor_int(rec.py-.5f);
    const int y0=(rec.rows&1u)?iy:iy+1,y1=(rec.rows&2u)?iy+2:iy+1;
    if(accum)splat4(rec.px,rec.py,e,y0,y1);
    else splat(rec.px,rec.py,e,y0,y1);
   }
  }
 };

 // Rounds adapt to the records actually produced, so memory stays near
 // kRecordBudget while every round still has work for all threads.
 const size_t min_round=size_t(std::max(1,thread_count()))*2;
 size_t round=plan.enabled?min_round:
    std::max(min_round,kRecordBudget/std::max<size_t>(1,size_t(std::min<size_t>(
      kChunk,pupil_storage->size()))*Q*max_group));
 for(size_t start=0;start<total;){
  if(poll_abort())break;
  const size_t count=std::min(round,total-start);
  if(results.size()<count)results.resize(count);
  parallel_for(int(count),[&](int i){trace_item(start+size_t(i),results[size_t(i)]);});
  if(poll_abort())break;
  parallel_for(bands,[&](int b){splat_band(b,count);});
  size_t produced=0;
  for(size_t i=0;i<count;i++){
   out.diagnostics.traced_rays+=results[i].traced;
   out.diagnostics.valid_rays+=results[i].valid;
   out.diagnostics.sensor_hits+=results[i].hits;
   produced+=results[i].records.size()+results[i].cells.size()*4;
  }
  start+=count;
  const double per_item=std::max(1.,double(produced)/double(count));
  round=std::max(min_round,size_t(double(kRecordBudget)/per_item));
 }
 if(const int code=abort_code.load()){out.abort_error=code;return out;}
 // Pack the RGBA accumulator and gather screen statistics, combined in row
 // order.
 std::vector<int> lit(size_t(h),0);
 std::vector<float> peak(size_t(h),0.f);
 std::vector<double> sum(size_t(h),0.);
 parallel_for(h,[&](int y){
  float* row=image+size_t(y)*w*3;
  if(accum){
   const float* src=accum+size_t(y)*w*4;
   for(int x=0;x<w;x++){
    row[x*3]=src[x*4];row[x*3+1]=src[x*4+1];row[x*3+2]=src[x*4+2];
   }
  }
  // Accumulated locally: neighbouring rows run on other threads, and
  // per-pixel writes to lit/peak/sum would bounce their cache lines.
  int count=0;
  float top=0.f;
  double total=0.;
  for(int x=0;x<w;x++){
   const float l=.2126f*row[x*3]+.7152f*row[x*3+1]+.0722f*row[x*3+2];
   if(l>0&&finite(l)){
    count++;top=std::max(top,l);total+=l;
   }
  }
  lit[y]=count;peak[y]=top;sum[y]=total;
 });
 for(int y=0;y<h;y++){
  out.diagnostics.screen_samples+=lit[y];
  out.diagnostics.max_luminance=std::max(out.diagnostics.max_luminance,peak[y]);
  out.diagnostics.sum_luminance+=sum[y];
 }
 return out;
}
} // namespace fl
