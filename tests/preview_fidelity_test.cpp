// After Effects renders Half, Third and Quarter previews into a downsampled
// world (in_data->downsample_*). Each preview must look like the Full frame
// scaled down: same brightness per pixel, ghost sampling, colour, detected
// sources, blur, haze and diffraction. Renders the real AEX (CPU path) at
// 1/1, 1/2, 1/3 and 1/4 and compares every reduced frame with the Full frame
// box-filtered to the same size.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
#include "AE_Effect.h"
#include "AE_EffectCBSuites.h"
#include "AE_Macros.h"
#include "mock_custom_ui_host.hpp"
#include "SPBasic.h"

namespace {
using Main=PF_Err(*)(PF_Cmd,PF_InData*,PF_OutData*,PF_ParamDef**,PF_LayerDef*,void*);
PF_WorldSuite2 g_world{};
std::vector<PF_ParamDef> g_params;
PF_Err abort_ok(PF_ProgPtr){return PF_Err_NONE;}
PF_Err collect(PF_ProgPtr,PF_ParamIndex index,PF_ParamDefPtr p){
 if(index!=-1||!p)return PF_Err_BAD_CALLBACK_PARAM;
 g_params.push_back(*p);fl_mock::activate(g_params.back());
 return PF_Err_NONE;
}
PF_Err world_format(const PF_EffectWorld*,PF_PixelFormat* format){
 if(!format)return PF_Err_BAD_CALLBACK_PARAM;
 *format=PF_PixelFormat_ARGB128;
 return PF_Err_NONE;
}
SPErr SPAPI acquire(const char* name,int version,const void** suite){
 if(!name||!suite||std::strcmp(name,kPFWorldSuite)!=0||
    version!=kPFWorldSuiteVersion2)return 1;
 *suite=&g_world;
 return 0;
}
SPErr SPAPI release(const char*,int){return 0;}

// Capture index = parameter id - 1 (the input layer is not captured).
enum {I_LIGHT=0,I_GRID=2,I_AUTO=5,I_OUTPUT_MODE=10,I_CLUSTER=23,I_GHOST_BLUR=26,
      I_HAZE_GAIN=28,I_STAR_GAIN=31,I_HOST_TRACE=55};
constexpr int kW=1920,kH=1080;

// Box-filter an RGB image by an integer factor.
std::vector<float> shrink(const std::vector<float>& rgb,int w,int h,int d){
 const int sw=w/d,sh=h/d;
 std::vector<float> out(size_t(sw)*sh*3,0.f);
 for(int y=0;y<sh;y++)for(int x=0;x<sw;x++)for(int c=0;c<3;c++){
  double sum=0;
  for(int yy=0;yy<d;yy++)for(int xx=0;xx<d;xx++)
   sum+=rgb[(size_t(y*d+yy)*w+x*d+xx)*3+c];
  out[(size_t(y)*sw+x)*3+c]=float(sum/(d*d));
 }
 return out;
}

// Renders the AEX at downsample 1/d. input_rgb (Full size) may be empty.
std::vector<float> render(Main effect,PF_InData& in,int d,const std::vector<float>& input_rgb,
                          const std::function<void(std::vector<PF_ParamDef>&)>& setup){
 const int w=kW/d,h=kH/d;
 std::vector<PF_PixelFloat> input(size_t(w)*h),output(size_t(w)*h);
 if(!input_rgb.empty()){
  const auto scaled=shrink(input_rgb,kW,kH,d);
  for(size_t i=0;i<input.size();i++){
   input[i].alpha=1.f;
   input[i].red=scaled[i*3];input[i].green=scaled[i*3+1];input[i].blue=scaled[i*3+2];
  }
 }
 PF_LayerDef src{},dst{};
 src.width=dst.width=w;src.height=dst.height=h;
 src.rowbytes=dst.rowbytes=int(w*sizeof(PF_PixelFloat));
 src.data=reinterpret_cast<PF_Pixel*>(input.data());
 dst.data=reinterpret_cast<PF_Pixel*>(output.data());
 in.width=kW;in.height=kH;
 in.downsample_x.num=1;in.downsample_x.den=d;
 in.downsample_y.num=1;in.downsample_y.den=d;
 auto definitions=g_params;
 // Like After Effects, hand the point over in the downsampled world.
 definitions[I_LIGHT].u.td.x_value=FLOAT2FIX(.43f*kW/d);
 definitions[I_LIGHT].u.td.y_value=FLOAT2FIX(.5f*kH/d);
 definitions[I_OUTPUT_MODE].u.pd.value=2; // Flare Only
 definitions[I_HOST_TRACE].u.bd.value=FALSE;
 setup(definitions);
 PF_ParamDef layer{};
 layer.param_type=PF_Param_LAYER;
 layer.u.ld=src;
 std::vector<PF_ParamDef*> params(definitions.size()+1);
 params[0]=&layer;
 for(size_t i=0;i<definitions.size();i++)params[i+1]=&definitions[i];
 PF_OutData out{};
 if(effect(PF_Cmd_RENDER,&in,&out,params.data(),&dst,nullptr)!=PF_Err_NONE)return {};
 std::vector<float> rgb(size_t(w)*h*3);
 for(size_t i=0;i<output.size();i++){
  rgb[i*3]=output[i].red;rgb[i*3+1]=output[i].green;rgb[i*3+2]=output[i].blue;
 }
 return rgb;
}

struct Match {double energy,l1;};
Match compare(const std::vector<float>& low,const std::vector<float>& ref){
 double sum_low=0,sum_ref=0,diff=0;
 for(size_t i=0;i<low.size();i++){
  sum_low+=low[i];sum_ref+=ref[i];diff+=std::fabs(double(low[i])-ref[i]);
 }
 return {sum_ref>0?sum_low/sum_ref:0.,sum_ref>0?diff/sum_ref:1.};
}
} // namespace

int main(){
 wchar_t exe[MAX_PATH]{};
 if(!GetModuleFileNameW(nullptr,exe,MAX_PATH))return 1;
 const auto path=std::filesystem::path(exe).parent_path()/L"FlareLensAE.aex";
 const auto dll=LoadLibraryW(path.c_str());
 if(!dll){std::cerr<<"Missing AEX "<<path.string()<<"\n";return 2;}
 const auto effect=reinterpret_cast<Main>(GetProcAddress(dll,"EffectMain"));
 if(!effect)return 3;
 g_world.PF_GetPixelFormat=&world_format;
 SPBasicSuite basic{};
 basic.AcquireSuite=&acquire;
 basic.ReleaseSuite=&release;
 PF_InData in{};
 fl_mock::configure(in);
 in.pica_basicP=&basic;
 in.inter.add_param=&collect;
 in.inter.abort=&abort_ok;
 PF_OutData out{};
 if(effect(PF_Cmd_PARAMS_SETUP,&in,&out,nullptr,nullptr,nullptr)!=PF_Err_NONE||
    g_params.size()!=64)return 4;

 // HDR plate for source detection: two small bright lamps.
 std::vector<float> plate(size_t(kW)*kH*3,0.f);
 const auto lamp=[&](float cx,float cy,float r,float v){
  for(int y=0;y<kH;y++)for(int x=0;x<kW;x++){
   const float dx=x+.5f-cx,dy=y+.5f-cy;
   if(dx*dx+dy*dy<=r*r)for(int c=0;c<3;c++)plate[(size_t(y)*kW+x)*3+c]=v;
  }
 };
 lamp(.43f*kW,.5f*kH,14.f,40.f);
 lamp(.72f*kW,.28f*kH,9.f,25.f);
 // 60 px from the first lamp: one cluster only if the radius were not
 // scaled with the preview (24 px at Full reaching 96 px at Quarter).
 lamp(.43f*kW+60.f,.5f*kH,9.f,30.f);

 // third_l1_tol < 0: the same tolerance at Third.
 struct Scene {const char* name;bool plate;std::function<void(std::vector<PF_ParamDef>&)> setup;
               double energy_tol,l1_tol,third_l1_tol=-1.;};
 const std::vector<Scene> scenes{
  // Ghosts are traced and blurred at full resolution and box-averaged, and
  // diffraction is supersampled: the same image up to float rounding.
  {"manual",false,[](auto&){},.005,.01},
  {"manual_no_blur",false,[](auto& p){p[I_GHOST_BLUR].u.fs_d.value=0.;},.005,.01},
  // Haze is blurred on the preview raster with the Full blur's variance.
  {"manual_haze",false,[](auto& p){p[I_HAZE_GAIN].u.fs_d.value=1.;},.005,.02},
  {"manual_diffraction",false,[](auto& p){p[I_STAR_GAIN].u.fs_d.value=1.;},.005,.01},
  {"manual_grid_512",false,[](auto& p){p[I_GRID].u.sd.value=512;},.005,.01},
  // Detection blocks match the Full ones only when the scan step divides
  // by the downsample (Third: 3x3 instead of 4x4 pixels); energy holds.
  {"detected_sources",true,[](auto& p){p[I_AUTO].u.bd.value=TRUE;},.02,.01,.30},
  {"detected_clustered",true,[](auto& p){p[I_AUTO].u.bd.value=TRUE;
                                         p[I_CLUSTER].u.sd.value=24;},.02,.01,.30},
 };
 bool ok=true;
 for(const auto& scene:scenes){
  const std::vector<float> no_plate;
  const auto& input=scene.plate?plate:no_plate;
  auto t0=std::chrono::steady_clock::now();
  const auto full=render(effect,in,1,input,scene.setup);
  const double full_ms=std::chrono::duration<double,std::milli>(
     std::chrono::steady_clock::now()-t0).count();
  if(full.empty()){std::cerr<<scene.name<<": full render failed\n";return 5;}
  for(int d:{2,3,4}){
   t0=std::chrono::steady_clock::now();
   const auto low=render(effect,in,d,input,scene.setup);
   const double low_ms=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-t0).count();
   if(low.empty()){std::cerr<<scene.name<<": 1/"<<d<<" render failed\n";return 6;}
   const auto m=compare(low,shrink(full,kW,kH,d));
   const double l1_tol=d==3&&scene.third_l1_tol>=0?scene.third_l1_tol:scene.l1_tol;
   const bool pass=std::fabs(m.energy-1.)<=scene.energy_tol&&m.l1<=l1_tol;
   ok=ok&&pass;
   std::printf("PREVIEW %-20s 1/%d energy_ratio=%.3f rel_L1=%.3f %s"
               " (CPU %.0f ms, Full %.0f ms)\n",
               scene.name,d,m.energy,m.l1,pass?"ok":"MISMATCH",low_ms,full_ms);
  }
 }
 FreeLibrary(dll);
 if(!ok)return 7;
 std::printf("PREVIEW_RESOLUTIONS_MATCH_FULL\n");
 return 0;
}
