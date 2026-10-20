// Frame-level CPU and CUDA benchmark with After Effects-like settings.
// CPU timing covers ghost trace, ghost blur, haze, diffraction and a
// float composite (render_worlds without host pixel conversion). CUDA timing
// covers cuda_compose on a 1080p BGRA128 plate, including stream sync.
//
//   FlareLensFrameBenchmark [--dump DIR] [--compare DIR] [--cpu-only]
//                           [--gpu-only] [--quick] [--case NAME] [--sweep]
//
// --sweep times one slider at a time from the After Effects defaults
// (catalog lens 1, 109 degree FOV) instead of the standard cases.
//
// --light X Y (0..1) and --grid N override the catalog light and Ray Grid.
// --catalog FIRST LAST STEP times every catalog lens (FlareLensCatalog.tsv
// next to the executable) with the sweep's AE defaults and prints one tab-
// separated line per lens: id, surfaces, pairs, rays, CPU first frame, CPU
// next frame, GPU frame (ms), the lens file and the ghost pair selection
// (the first cached_pairs() call for that lens and FOV, ms).
// --dump writes each case's CPU RGB and GPU BGRA float images so another
// build can be checked against it with --compare (relative L1 and max error).
#include "FlareLensGpuBridge.hpp"
#include "FlareLensCpuParallel.hpp"
#include "FlareLensOptics.hpp"
#include "FlareLensComposite.hpp"
#include "FlareLensThreadPool.hpp"
#include <cuda_runtime.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
namespace fs=std::filesystem;
struct Case {
 const char* name;
 const char* lens;
 int w,h,grid,spectral,sources;
 float ghost_blur;int blur_passes;
 float haze_gain,star_gain;
 int cpu_repeats,gpu_repeats;
 // Optional slider overrides (sweep mode).
 float fov=40.f,gain=1000.f,haze_radius=.15f,star_scale=.15f,rotation=0.f;
 int haze_passes=3,pupil=3,blades=-1;
 bool rotate_each_frame=false; // new aperture rotation per frame
 float light_x=1450.f/1920.f,light_y=300.f/1080.f;
};
const fs::path kLensDir=fs::path(__FILE__).parent_path().parent_path()/
                        "upstream/lenses/lens_files";
constexpr const char* kRoss="100mm_F10_(Ross_F10_APO_Process_Xpres).lens";
constexpr const char* kNikkor="AI_Nikkor_50mm_F1.2S.lens";
constexpr const char* kZoom="AI_AF_Nikkor_24-120mm_F3.5-5.6D.lens";

// Detected HDR blocks cluster around a few bright lights, like Nuke's
// 4x4 scan of street lights. Brightest blocks first, as the detector sorts.
std::vector<fl::Source> make_sources(const Case& c){
 const int count=c.sources,w=c.w,h=c.h;
 if(count==1)return {{c.light_x,c.light_y,28950.f,28950.f,28950.f}};
 const float centers[4][2]={{.72f,.28f},{.30f,.62f},{.55f,.45f},{.85f,.75f}};
 std::vector<fl::Source> out;
 out.reserve(count);
 for(int ring=0;int(out.size())<count;ring++){
  for(int c=0;c<4&&int(out.size())<count;c++){
   for(int k=0;k<8*std::max(1,ring)&&int(out.size())<count;k++){
    const float a=6.2831853f*k/(8*std::max(1,ring))+c;
    const float px=centers[c][0]*w+std::cos(a)*4.f*ring;
    const float py=centers[c][1]*h+std::sin(a)*4.f*ring;
    const float e=60.f/(1.f+.15f*ring);
    out.push_back({px/w,py/h,e*1.1f,e,e*.85f});
   }
  }
 }
 return out;
}
fl::Settings settings_for(const Case& c){
 fl::Settings s{};
 s.gain=c.gain;s.grid=c.grid;s.max_pairs=2048;
 s.spectral_samples=c.spectral;s.pupil_jitter=c.pupil;
 s.fov_h_deg=c.fov;
 s.aperture_blades=c.blades>=0?c.blades:c.star_gain>0?6:0;
 s.aperture_rotation_deg=c.rotation;
 return s;
}
double median(std::vector<double> v){
 std::sort(v.begin(),v.end());
 return v.empty()?0.:v[v.size()/2];
}
bool write_raw(const fs::path& p,const std::vector<float>& v){
 std::ofstream f(p,std::ios::binary);
 f.write(reinterpret_cast<const char*>(v.data()),std::streamsize(v.size()*sizeof(float)));
 return bool(f);
}
bool read_raw(const fs::path& p,std::vector<float>& v,size_t n){
 std::ifstream f(p,std::ios::binary);
 v.assign(n,0.f);
 f.read(reinterpret_cast<char*>(v.data()),std::streamsize(n*sizeof(float)));
 return bool(f);
}
void compare(const char* label,const std::vector<float>& now,const fs::path& ref){
 std::vector<float> base;
 if(!read_raw(ref,base,now.size())){std::cout<<"  "<<label<<" compare: no baseline\n";return;}
 double diff=0,total=0;float worst=0;
 for(size_t i=0;i<now.size();i++){
  const double d=std::fabs(double(now[i])-base[i]);
  diff+=d;total+=std::fabs(double(base[i]));
  worst=std::max(worst,float(d));
 }
 std::printf("  %s vs baseline: rel_L1=%.3g max_abs=%.3g energy=%.6g\n",
             label,diff/std::max(1e-12,total),double(worst),total);
}

double since(std::chrono::steady_clock::time_point t){
 return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();
}
double g_blur_ms=0,g_post_ms=0,g_composite_ms=0;
unsigned long long g_traced=0,g_hits=0;
// render_worlds equivalent on float RGB, without the AE world conversion.
std::vector<float> cpu_frame(const LensSystem& lens,const std::vector<fl::Source>& sources,
                             const Case& c,const fl::Settings& s,double& trace_ms){
 const auto t0=std::chrono::steady_clock::now();
 auto ghost=fl::render_sources_parallel(lens,sources,s,c.w,c.h,{},3);
 trace_ms=since(t0);
 g_traced=ghost.diagnostics.traced_rays;g_hits=ghost.diagnostics.sensor_hits;
 const auto t1=std::chrono::steady_clock::now();
 if(!ghost.rgb.empty()&&c.ghost_blur>0){
  const int radius=std::max(1,int(std::round(c.ghost_blur*std::hypot(float(c.w),float(c.h)))));
  fl::box_blur_rgb(ghost.rgb,c.w,c.h,radius,c.blur_passes);
 }
 g_blur_ms=since(t1);
 const auto t2=std::chrono::steady_clock::now();
 std::vector<float> post;
 if(c.haze_gain>0||c.star_gain>0)post=fl::acquire_buffer(size_t(c.w)*c.h*3);
 const auto merge=[&]{
  if(post.empty())return;
  if(ghost.rgb.empty()){ghost.rgb.swap(post);return;}
  for(size_t i=0;i<ghost.rgb.size();i++)ghost.rgb[i]+=post[i];
 };
 if(c.haze_gain>0){
  fl::render_haze(post,sources,c.w,c.h,c.haze_gain,c.haze_radius,c.haze_passes,1);merge();
 }
 if(c.star_gain>0){
  fl::render_diffraction(post,sources,c.w,c.h,s.aperture_blades,
                         s.aperture_rotation_deg,c.star_gain,c.star_scale);
  merge();
 }
 g_post_ms=since(t2);
 const auto t3=std::chrono::steady_clock::now();
 // Composite onto a transparent black plate, as Composite mode does. AE owns
 // the output world, so the plate is reused like a host buffer.
 static std::vector<float> plate;
 plate.resize(size_t(c.w)*c.h*4);
 std::vector<float>& out=plate;
 std::fill(out.begin(),out.end(),0.f);
 const auto t4=std::chrono::steady_clock::now();
 if(!ghost.rgb.empty()){
  fl::parallel_for(c.h,[&](int y){
   for(size_t i=size_t(y)*c.w;i<size_t(y+1)*c.w;i++){
    const float r=ghost.rgb[i*3],g=ghost.rgb[i*3+1],b=ghost.rgb[i*3+2];
    out[i*4]=fl::alpha_for_emission(0.f,r,g,b);
    out[i*4+1]=r;out[i*4+2]=g;out[i*4+3]=b;
   }
  });
 }
 g_composite_ms=since(t4);
 (void)t3;
 fl::release_buffer(std::move(ghost.rgb));
 fl::release_buffer(std::move(post));
 return out;
}
} // namespace

// Catalog lenses in id order, from the TSV CMake writes next to the binaries.
std::vector<std::string> catalog_names(const fs::path& exe){
 std::vector<std::string> names;
 std::ifstream f(exe.parent_path()/"FlareLensCatalog.tsv");
 std::string line;
 while(std::getline(f,line)){
  const auto tab=line.find('\t');
  if(tab==std::string::npos)continue;
  std::string name=line.substr(tab+1);
  while(!name.empty()&&(name.back()=='\r'||name.back()=='\n'))name.pop_back();
  names.push_back(name);
 }
 return names;
}
int run_catalog(const fs::path& exe,int first,int last,int step,bool cpu,bool gpu,
                float light_x,float light_y,int grid){
 const auto names=catalog_names(exe);
 if(names.empty()){std::cerr<<"FlareLensCatalog.tsv not found\n";return 1;}
 Case c{"catalog",kRoss,1920,1080,64,3,1,.0008f,2,0,0,1,3};
 c.fov=109.f;c.light_x=832.f/1920.f;c.light_y=540.f/1080.f;
 if(light_x>=0)c.light_x=light_x;
 if(light_y>=0)c.light_y=light_y;
 if(grid>0)c.grid=grid;
 const auto sources=make_sources(c);
 const auto s=settings_for(c);
 const size_t bytes=size_t(c.w)*c.h*16;
 void *src=nullptr,*out=nullptr,*scratch=nullptr;
 size_t scratch_size=0;
 if(gpu){
  if(cudaMalloc(&src,bytes)!=cudaSuccess||cudaMalloc(&out,bytes)!=cudaSuccess)return 2;
  cudaMemset(src,0,bytes);
 }
 std::printf("id\tsurfaces\tpairs\trays\tcpu_first_ms\tcpu_ms\tgpu_ms\tlens\tselect_ms\n");
 last=std::min(last,int(names.size()));
 for(int id=std::max(1,first);id<=last;id+=std::max(1,step)){
  const std::string& name=names[size_t(id-1)];
  LensSystem lens;
  if(!lens.load((kLensDir/name).string().c_str())||lens.num_surfaces()<2){
   std::printf("%d\t0\t0\t0\t0\t0\t0\t%s (not loaded)\n",id,name.c_str());continue;
  }
  const float sx=lens.focal_length*std::tan(s.fov_h_deg*.00872664625997f);
  const auto t_select=std::chrono::steady_clock::now();
  const auto pairs=fl::cached_pairs(lens,sx,sx*c.h/float(c.w));
  const double select_ms=since(t_select);
  double first_ms=0,next_ms=0,gpu_ms=0;
  if(cpu){
   double trace_ms=0;
   auto t=std::chrono::steady_clock::now();
   cpu_frame(lens,sources,c,s,trace_ms);
   first_ms=since(t);
   t=std::chrono::steady_clock::now();
   cpu_frame(lens,sources,c,s,trace_ms);
   next_ms=since(t);
  }
  if(gpu){
   const size_t need=fl::gpu::scratch_bytes(lens,int(sources.size()),s.grid,c.w,c.h,1,
                                            true,false,false,false);
   if(need>scratch_size){
    if(scratch)cudaFree(scratch);
    scratch=nullptr;scratch_size=0;
    if(cudaMalloc(&scratch,need)!=cudaSuccess)return 2;
    scratch_size=need;
   }
   fl::gpu::View v{src,out,c.w,c.h,c.w*16,c.w*16};
   v.output_mode=1;v.ghost_blur=c.ghost_blur;v.ghost_blur_passes=c.blur_passes;
   std::vector<double> times;
   for(int r=0;r<4;r++){
    const auto t=std::chrono::steady_clock::now();
    if(!fl::gpu::cuda_compose(lens,sources,s,v,scratch,scratch_size)){times.clear();break;}
    if(r)times.push_back(since(t));
   }
   gpu_ms=times.empty()?-1.:median(times);
  }
  std::printf("%d\t%d\t%zu\t%llu\t%.2f\t%.2f\t%.2f\t%s\t%.2f\n",id,lens.num_surfaces(),
              pairs->pairs.size(),g_traced,first_ms,next_ms,gpu_ms,name.c_str(),select_ms);
  std::fflush(stdout);
 }
 if(src)cudaFree(src);
 if(out)cudaFree(out);
 if(scratch)cudaFree(scratch);
 return 0;
}

int main(int argc,char** argv){
 fs::path dump,ref;
 bool cpu=true,gpu=true,quick=false,sweep=false;
 int reps_override=0,catalog_first=0,catalog_last=0,catalog_step=1,grid_override=0;
 float light_x=-1.f,light_y=-1.f;
 std::string only;
 for(int i=1;i<argc;i++){
  const std::string a=argv[i];
  if(a=="--dump"&&i+1<argc)dump=argv[++i];
  else if(a=="--compare"&&i+1<argc)ref=argv[++i];
  else if(a=="--cpu-only")gpu=false;
  else if(a=="--gpu-only")cpu=false;
  else if(a=="--quick")quick=true;
  else if(a=="--case"&&i+1<argc)only=argv[++i];
  else if(a=="--reps"&&i+1<argc)reps_override=std::atoi(argv[++i]);
  else if(a=="--sweep")sweep=true;
  else if(a=="--light"&&i+2<argc){
   light_x=float(std::atof(argv[i+1]));light_y=float(std::atof(argv[i+2]));i+=2;
  }
  else if(a=="--grid"&&i+1<argc)grid_override=std::atoi(argv[++i]);
  else if(a=="--catalog"&&i+3<argc){
   catalog_first=std::atoi(argv[i+1]);catalog_last=std::atoi(argv[i+2]);
   catalog_step=std::atoi(argv[i+3]);i+=3;
  }
 }
 if(!dump.empty())fs::create_directories(dump);
 int devices=0;
 if(gpu&&(cudaGetDeviceCount(&devices)!=cudaSuccess||!devices)){
  std::cout<<"No CUDA device; GPU cases skipped\n";gpu=false;
 }
 if(catalog_first>0)
  return run_catalog(fs::absolute(argv[0]),catalog_first,catalog_last,catalog_step,cpu,gpu,
                     light_x,light_y,grid_override);
 std::vector<Case> cases{
  // name            lens     w    h   grid spec src blur   pass haze star cpu gpu
  {"default_ross",   kRoss,  1920,1080, 64, 3,   1,.0008f,2,   0,  0,   3, 15},
  {"default_nikkor", kNikkor,1920,1080, 64, 3,   1,.0008f,2,   0,  0,   3, 15},
  {"zoom_24_120",    kZoom,  1920,1080, 64, 3,   1,.0008f,2,   0,  0,   2, 10},
  {"detect_128",     kNikkor,1920,1080, 64, 3, 128,.0008f,2,   0,  0,   1,  5},
  {"detect_512",     kNikkor,1920,1080, 64, 3, 512,.0008f,2,   0,  0,   1,  3},
  {"hq_grid256_7",   kNikkor,1920,1080,256, 7,   1,.0008f,2,   0,  0,   1,  5},
  {"haze",           kNikkor,1920,1080, 64, 3,   1,.0008f,2,   1,  0,   3, 10},
  {"starburst",      kNikkor,1920,1080, 64, 3,   1,.0008f,2,   0,  1,   3, 10},
  {"uhd_default",    kNikkor,3840,2160, 64, 3,   1,.0008f,2,   0,  0,   2, 10},
 };
 if(sweep){
  // AE defaults with the catalog lens 1 and the 109 degree FOV from a real
  // project; each case changes one slider.
  Case base{"sw_base",kRoss,1920,1080,64,3,1,.0008f,2,0,0,3,9};
  base.fov=109.f;base.light_x=832.f/1920.f;base.light_y=540.f/1080.f;
  cases.clear();
  const auto add=[&](const char* name,auto&& change){
   Case c=base;c.name=name;change(c);cases.push_back(c);
  };
  add("sw_base",[](Case&){});
  add("sw_gain_100000",[](Case& c){c.gain=100000.f;});
  add("sw_grid_256",[](Case& c){c.grid=256;});
  add("sw_grid_512",[](Case& c){c.grid=512;});
  add("sw_grid_1024",[](Case& c){c.grid=1024;c.cpu_repeats=1;c.gpu_repeats=5;});
  add("sw_grid_1942",[](Case& c){c.grid=1942;c.cpu_repeats=1;c.gpu_repeats=3;});
  add("sw_spectral_11",[](Case& c){c.spectral=11;});
  add("sw_regular_1024",[](Case& c){c.grid=1024;c.pupil=0;c.cpu_repeats=1;c.gpu_repeats=5;});
  add("sw_stratified_1024",[](Case& c){c.grid=1024;c.pupil=1;c.cpu_repeats=1;c.gpu_repeats=5;});
  add("sw_halton_1024",[](Case& c){c.grid=1024;c.pupil=2;c.cpu_repeats=1;c.gpu_repeats=5;});
  add("sw_blades_6",[](Case& c){c.blades=6;});
  add("sw_star",[](Case& c){c.star_gain=1.f;});
  add("sw_star_scale_1",[](Case& c){c.star_gain=1.f;c.star_scale=1.f;});
  add("sw_star_rotating",[](Case& c){c.star_gain=1.f;c.rotate_each_frame=true;});
  add("sw_haze",[](Case& c){c.haze_gain=1.f;});
  add("sw_haze_max",[](Case& c){c.haze_gain=1.f;c.haze_radius=1.f;c.haze_passes=6;});
  add("sw_blur_max",[](Case& c){c.ghost_blur=.1f;c.blur_passes=6;});
  add("sw_lens_63",[](Case& c){
   c.lens="Nikon_180-400mm_F4_IF_OS_(AF-S_Nikkor_180-400mm_F4E_TC1.4_FL_ED_VR_with_TC).lens";
   c.cpu_repeats=1;c.gpu_repeats=5;});
  add("sw_lens_63_grid_256",[](Case& c){
   c.lens="Nikon_180-400mm_F4_IF_OS_(AF-S_Nikkor_180-400mm_F4E_TC1.4_FL_ED_VR_with_TC).lens";
   c.grid=256;c.cpu_repeats=1;c.gpu_repeats=3;});
  add("sw_lens_63_grid_1024",[](Case& c){
   c.lens="Nikon_180-400mm_F4_IF_OS_(AF-S_Nikkor_180-400mm_F4E_TC1.4_FL_ED_VR_with_TC).lens";
   c.grid=1024;c.cpu_repeats=1;c.gpu_repeats=3;});
  add("sw_detect_512_star",[](Case& c){c.sources=512;c.star_gain=1.f;c.cpu_repeats=1;c.gpu_repeats=3;});
 }
 for(const auto& c:cases){
  if(!only.empty()&&only!=c.name)continue;
  LensSystem lens;
  if(!lens.load((kLensDir/c.lens).string().c_str())){
   std::cerr<<"Cannot load "<<c.lens<<"\n";return 1;
  }
  const auto sources=make_sources(c);
  auto s=settings_for(c);
  const float sx=lens.focal_length*std::tan(s.fov_h_deg*.00872664625997f);
  const auto pairs=fl::cached_pairs(lens,sx,sx*c.h/float(c.w));
  std::printf("CASE %s %dx%d lens_surfaces=%d pairs=%zu grid=%d spectral=%d sources=%zu\n",
              c.name,c.w,c.h,lens.num_surfaces(),pairs->pairs.size(),c.grid,
              c.spectral,sources.size());
  if(cpu&&c.cpu_repeats>0){
   const int reps=reps_override>0?reps_override:quick?1:c.cpu_repeats;
   std::vector<double> total,trace;
   std::vector<float> image;
   for(int r=0;r<reps;r++){
    double trace_ms=0;
    if(c.rotate_each_frame)s.aperture_rotation_deg=c.rotation+1.f+r;
    const auto t=std::chrono::steady_clock::now();
    image=cpu_frame(lens,sources,c,s,trace_ms);
    total.push_back(std::chrono::duration<double,std::milli>(
       std::chrono::steady_clock::now()-t).count());
    trace.push_back(trace_ms);
   }
   std::printf("  CPU frame_ms=%.2f trace_ms=%.2f (runs=%d) last: blur=%.2f post=%.2f"
               " composite=%.2f rays=%llu photons=%llu\n",
               median(total),median(trace),reps,g_blur_ms,g_post_ms,g_composite_ms,
               g_traced,g_hits);
   const std::string file=std::string(c.name)+"_cpu.raw";
   if(!dump.empty())write_raw(dump/file,image);
   if(!ref.empty())compare("CPU",image,ref/file);
  }
  if(gpu&&c.gpu_repeats>0){
   const size_t bytes=size_t(c.w)*c.h*16;
   const bool need_blur=c.ghost_blur>0;
   const bool dense_haze=c.haze_gain>0&&sources.size()>8;
   const size_t size=fl::gpu::scratch_bytes(lens,int(sources.size()),s.grid,c.w,c.h,1,
                                            need_blur,dense_haze,c.haze_gain>0&&!dense_haze,
                                            c.star_gain>0);
   void *src=nullptr,*out=nullptr,*scratch=nullptr;
   if(!size||cudaMalloc(&src,bytes)!=cudaSuccess||cudaMalloc(&out,bytes)!=cudaSuccess||
      cudaMalloc(&scratch,size)!=cudaSuccess){std::cerr<<"CUDA allocation failed\n";return 2;}
   cudaMemset(src,0,bytes);
   fl::gpu::View v{src,out,c.w,c.h,c.w*16,c.w*16};
   v.output_mode=1;v.ghost_blur=c.ghost_blur;v.ghost_blur_passes=c.blur_passes;
   v.haze_gain=c.haze_gain;v.haze_radius=c.haze_radius;v.haze_passes=c.haze_passes;
   v.star_gain=c.star_gain;v.star_scale=c.star_scale;
   v.star_blades=s.aperture_blades;v.star_rotation=s.aperture_rotation_deg;
   if(!fl::gpu::cuda_compose(lens,sources,s,v,scratch,size)){
    std::cerr<<"cuda_compose failed for "<<c.name<<"\n";return 3;
   }
   const int reps=reps_override>0?reps_override:quick?2:c.gpu_repeats;
   std::vector<double> times;
   for(int r=0;r<reps;r++){
    if(c.rotate_each_frame)v.star_rotation=c.rotation+101.f+r;
    const auto t=std::chrono::steady_clock::now();
    if(!fl::gpu::cuda_compose(lens,sources,s,v,scratch,size))return 4;
    times.push_back(std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-t).count());
   }
   std::vector<float> image(size_t(c.w)*c.h*4);
   cudaMemcpy(image.data(),out,bytes,cudaMemcpyDeviceToHost);
   std::printf("  GPU frame_ms=%.2f (runs=%d)\n",median(times),reps);
   const std::string file=std::string(c.name)+"_gpu.raw";
   if(!dump.empty())write_raw(dump/file,image);
   if(!ref.empty()){
    if(fs::exists(ref/file))compare("GPU",image,ref/file);
    else{
     // No GPU baseline (the old build could not render this on CUDA):
     // compare with the CPU baseline, ARGB -> BGRA.
     std::vector<float> cpu_base;
     if(read_raw(ref/(std::string(c.name)+"_cpu.raw"),cpu_base,image.size())){
      std::vector<float> as_bgra(image.size());
      for(size_t i=0;i<image.size()/4;i++){
       as_bgra[i*4]=cpu_base[i*4+3];as_bgra[i*4+1]=cpu_base[i*4+2];
       as_bgra[i*4+2]=cpu_base[i*4+1];as_bgra[i*4+3]=cpu_base[i*4];
      }
      const auto tmp=fs::temp_directory_path()/"flaresim_cross.raw";
      write_raw(tmp,as_bgra);
      compare("GPU vs CPU",image,tmp);
     }
    }
   }
   cudaFree(src);cudaFree(out);cudaFree(scratch);
  }
  std::fflush(stdout);
 }
 return 0;
}
