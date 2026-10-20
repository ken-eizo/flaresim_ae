// Factory-default 8bpc visibility + direct Nuke HDR input source parity.
// Uses actual .aex EffectMain, never starts or controls After Effects.
#define NOMINMAX
#include <windows.h>
#include "AE_Effect.h"
#include "AE_EffectCBSuites.h"
#include "AE_Macros.h"
#include "mock_custom_ui_host.hpp"
#include "SPBasic.h"
#include "FlareLensSourceModel.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>
using Main=PF_Err(*)(PF_Cmd,PF_InData*,PF_OutData*,PF_ParamDef**,PF_LayerDef*,void*);
static PF_WorldSuite2 ws{};
static PF_PixelFormat format=PF_PixelFormat_ARGB128;
static std::vector<PF_ParamDef> controls;
static PF_Err add(PF_ProgPtr,PF_ParamIndex index,PF_ParamDefPtr value){
 if(index!=-1||!value)return PF_Err_BAD_CALLBACK_PARAM;
 controls.push_back(*value);fl_mock::activate(controls.back());return PF_Err_NONE;
}
static PF_Err pixel_fmt(const PF_EffectWorld*,PF_PixelFormat* out){
 if(!out)return PF_Err_BAD_CALLBACK_PARAM;*out=format;return PF_Err_NONE;
}
static SPErr SPAPI acquire(const char* name,int ver,const void** suite){
 if(!name||!suite||std::strcmp(name,kPFWorldSuite)||ver!=kPFWorldSuiteVersion2)
  return 1;*suite=&ws;return 0;
}
static SPErr SPAPI release(const char*,int){return 0;}
static PF_Err abort_ok(PF_ProgPtr){return PF_Err_NONE;}
static std::vector<float> draw(Main effect,PF_InData& host,bool detect,
 bool factory_area,bool floating,int width,int height,int x,int y,int mode=2){
 const size_t count=size_t(width)*height;
 format=floating?PF_PixelFormat_ARGB128:PF_PixelFormat_ARGB32;
 const size_t bpp=floating?sizeof(PF_PixelFloat):sizeof(PF_Pixel);
 std::vector<uint64_t> input((count*bpp+7)/8);
 std::vector<uint64_t> output((count*bpp+7)/8);
 if(detect){
  for(int iy=y;iy<y+4;iy++)for(int ix=x;ix<x+4;ix++){
   auto& p=reinterpret_cast<PF_PixelFloat*>(input.data())[size_t(iy)*width+ix];
   p.red=p.green=p.blue=64.f;p.alpha=1.f;
  }
 }
 PF_LayerDef source{},destination{};
 source.width=destination.width=width;
 source.height=destination.height=height;
 source.rowbytes=destination.rowbytes=int(width*bpp);
 source.data=reinterpret_cast<PF_Pixel*>(input.data());
 destination.data=reinterpret_cast<PF_Pixel*>(output.data());
 host.width=width;host.height=height;
 host.downsample_x.num=host.downsample_y.num=1;
 host.downsample_x.den=host.downsample_y.den=1;
 auto args=controls;
 args[0].u.td.x_value=FLOAT2FIX(x+2.f);
 args[0].u.td.y_value=FLOAT2FIX(y+2.f);
 args[5].u.bd.value=detect?TRUE:FALSE;
 args[10].u.pd.value=mode; // Factory Composite or isolated Flare Only.
 if(!factory_area){args[54].u.fs_d.value=64.;args[57].u.fs_d.value=0.;}
 PF_ParamDef input_def{};
 input_def.param_type=PF_Param_LAYER;input_def.u.ld=source;
 std::vector<PF_ParamDef*> pointers(args.size()+1);
 pointers[0]=&input_def;
 for(size_t i=0;i<args.size();i++)pointers[i+1]=&args[i];
 PF_OutData result{};
 const auto err=effect(PF_Cmd_RENDER,&host,&result,pointers.data(),&destination,nullptr);
 if(err)throw int(err);
 std::vector<float> rgb(count*3);
 for(size_t i=0;i<count;i++){
  if(floating){
   const auto& p=reinterpret_cast<const PF_PixelFloat*>(destination.data)[i];
   rgb[i*3]=p.red;rgb[i*3+1]=p.green;rgb[i*3+2]=p.blue;
  }else{
   const auto& p=reinterpret_cast<const PF_Pixel*>(destination.data)[i];
   rgb[i*3]=p.red/255.f;rgb[i*3+1]=p.green/255.f;rgb[i*3+2]=p.blue/255.f;
   if(mode==1&&(p.red||p.green||p.blue)&&p.alpha==0)
    throw 98; // emitting over transparent input must never be invisible
  }
 }
 return rgb;
}
int main(){
 wchar_t name[MAX_PATH]{};
 if(!GetModuleFileNameW(nullptr,name,MAX_PATH))return 1;
 const auto dll=LoadLibraryW((std::filesystem::path(name).parent_path()/
                             L"FlareLensAE.aex").c_str());
 if(!dll)return 2;
 const auto effect=reinterpret_cast<Main>(GetProcAddress(dll,"EffectMain"));
 if(!effect)return 3;
 ws.PF_GetPixelFormat=&pixel_fmt;
 SPBasicSuite basic{};basic.AcquireSuite=&acquire;basic.ReleaseSuite=&release;
 PF_InData host{};fl_mock::configure(host);host.pica_basicP=&basic;
 host.inter.add_param=&add;host.inter.abort=&abort_ok;
 PF_OutData result{};
 if(effect(PF_Cmd_PARAMS_SETUP,&host,&result,nullptr,nullptr,nullptr)||
    controls.size()!=64)return 4;
 if(controls[4].u.fs_d.dephault!=2.||controls[6].u.sd.dephault!=512||
    controls[54].u.fs_d.dephault!=64.||
    controls[57].u.fs_d.dephault!=96.)return 5;
 const auto radius=96.f*.5f;
 const auto energy=fl::nuke_equivalent_manual_radiance(64.f,96.f,1.f,1.f,4,512);
 const auto expected=64.f*3.14159265358979323846f*radius*radius/(4.f*4.f);
 if(std::abs(energy-expected)/expected>1.e-5f||
    energy<20000.f||energy>30000.f)return 6;
 if(std::abs(fl::nuke_equivalent_manual_radiance(64.f,96.f,.5f,.5f,4,512)-energy)>1e-3f)
  return 11; // avoid 4x dimming during AE adaptive half-res dragging
 // Factory-default emitted flare should be visibly present in 8bpc.
 std::vector<float> normal;
 try{normal=draw(effect,host,false,true,false,960,540,300,180,1);}
 catch(int e){std::cerr<<"8bpc render error="<<e<<"\n";return 7;}
 int nonblack=0;float peak=0;
 for(float v:normal){nonblack+=v>.5f/255.f;peak=std::max(peak,v);}
 std::cout<<"AEX_FACTORY_DEFAULT_8BPC_VISIBLE effective_energy="<<energy
          <<" visible_channels="<<nonblack<<" peak="<<peak<<"\n";
 if(nonblack<50||peak<=2.f/255.f)return 8;
 // Exact direct equivalence, one 4x4 HDR Nuke brightness block versus
 // a RAW AE point emitter of the same 64 scene-linear units. No area boost.
 std::vector<float> detected,manual;
 try{
  detected=draw(effect,host,true,false,true,320,180,120,80);
  manual=draw(effect,host,false,false,true,320,180,120,80);
 }catch(int e){std::cerr<<"float render error="<<e<<"\n";return 9;}
 double error=0,magnitude=0;
 for(size_t i=0;i<detected.size();i++){
  error+=std::abs(double(detected[i])-manual[i]);
  magnitude+=std::abs(double(detected[i]));
 }
 const auto relative=error/std::max(1e-10,magnitude);
 std::cout<<"NUKE_SINGLE_HDR_BLOCK_SOURCE_PARITY relative_L1="<<relative<<"\n";
 if(magnitude<=0||relative>1.e-5)return 10;
 FreeLibrary(dll);
 return 0;
}
