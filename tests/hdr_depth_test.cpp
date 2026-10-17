// Same native AE effect, three host pixel formats. 32bpc MUST retain
// unclamped scene-linear optical HDR photons as Nuke does. 8/16bpc are
// quantized bounded display formats and cannot reproduce >1 luminance.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>
#include "AE_Effect.h"
#include "AE_EffectCBSuites.h"
#include "AE_Macros.h"
#include "mock_custom_ui_host.hpp"
#include "SPBasic.h"
using Main=PF_Err(*)(PF_Cmd,PF_InData*,PF_OutData*,PF_ParamDef**,PF_LayerDef*,void*);
static PF_WorldSuite2 world_suite{};
static PF_PixelFormat pixel_format=PF_PixelFormat_ARGB128;
static std::vector<PF_ParamDef> params;
static PF_Err collect(PF_ProgPtr,PF_ParamIndex id,PF_ParamDefPtr p){
 if(id!=-1||!p)return PF_Err_BAD_CALLBACK_PARAM;
 params.push_back(*p);fl_mock::activate(params.back());return PF_Err_NONE;
}
static PF_Err world_format(const PF_EffectWorld*,PF_PixelFormat* ptr){
 if(!ptr)return PF_Err_BAD_CALLBACK_PARAM;
 *ptr=pixel_format;return PF_Err_NONE;
}
static SPErr SPAPI acquire(const char* name,int32 ver,const void** ptr){
 if(!name||!ptr||std::strcmp(name,kPFWorldSuite)!=0||
    ver!=kPFWorldSuiteVersion2)return 1;
 *ptr=&world_suite;return 0;
}
static SPErr SPAPI release(const char*,int32){return 0;}
static PF_Err abort_ok(PF_ProgPtr){return PF_Err_NONE;}
static std::vector<float> render(Main effect,PF_InData& in,PF_PixelFormat format,
                                  int width,int height){
 pixel_format=format;
 const size_t n=size_t(width)*height;
 const size_t bpp=format==PF_PixelFormat_ARGB128?
    sizeof(PF_PixelFloat):format==PF_PixelFormat_ARGB64?
    sizeof(PF_Pixel16):sizeof(PF_Pixel);
 std::vector<uint64_t> bytes((n*bpp+7)/8);
 PF_LayerDef input{},output{};
 input.width=output.width=width;
 input.height=output.height=height;
 output.rowbytes=int(width*bpp);
 output.data=reinterpret_cast<PF_Pixel*>(bytes.data());
 in.width=width;in.height=height;
 in.downsample_x.num=in.downsample_y.num=1;
 in.downsample_x.den=in.downsample_y.den=1;
 auto controls=params;
 controls[0].u.td.x_value=FLOAT2FIX(width*.63);
 controls[0].u.td.y_value=FLOAT2FIX(height*.31);
 controls[2].u.sd.value=32;  // quality remains independent of host bpc
 controls[5].u.bd.value=FALSE;
 controls[10].u.pd.value=2;  // Flare Only, no compositing background
 controls[15].u.pd.value=3;  // Seven wavelengths
 controls[16].u.pd.value=3;  // Halton
 controls[54].u.fs_d.value=1000000.; // specifically force scene HDR >1
 controls[57].u.fs_d.value=0.; // exact point-source HDR baseline
 controls[55].u.bd.value=FALSE;
 std::vector<PF_ParamDef*> pointers(controls.size()+1);
 PF_ParamDef input_def{};
 input_def.param_type=PF_Param_LAYER;
 input_def.u.ld=input;
 pointers[0]=&input_def;
 for(size_t i=0;i<controls.size();i++)pointers[i+1]=&controls[i];
 PF_OutData out{};
 const PF_Err err=effect(PF_Cmd_RENDER,&in,&out,pointers.data(),&output,nullptr);
 if(err)throw int(err);
 std::vector<float> values(n*4);
 for(size_t i=0;i<n;i++){
  if(format==PF_PixelFormat_ARGB128){
   const auto& p=reinterpret_cast<PF_PixelFloat*>(output.data)[i];
   values[i*4]=p.red;values[i*4+1]=p.green;
   values[i*4+2]=p.blue;values[i*4+3]=p.alpha;
  }else if(format==PF_PixelFormat_ARGB64){
   const auto& p=reinterpret_cast<PF_Pixel16*>(output.data)[i];
   values[i*4]=p.red/32768.f;values[i*4+1]=p.green/32768.f;
   values[i*4+2]=p.blue/32768.f;values[i*4+3]=p.alpha/32768.f;
  }else{
   const auto& p=reinterpret_cast<PF_Pixel*>(output.data)[i];
   values[i*4]=p.red/255.f;values[i*4+1]=p.green/255.f;
   values[i*4+2]=p.blue/255.f;values[i*4+3]=p.alpha/255.f;
  }
 }
 return values;
}
int main(){
 wchar_t name[MAX_PATH]{};
 if(!GetModuleFileNameW(nullptr,name,MAX_PATH))return 1;
 const auto file=std::filesystem::path(name).parent_path()/L"FlareLensAE.aex";
 const HMODULE dll=LoadLibraryW(file.c_str());
 if(!dll)return 2;
 const auto effect=reinterpret_cast<Main>(GetProcAddress(dll,"EffectMain"));
 if(!effect)return 3;
 world_suite.PF_GetPixelFormat=&world_format;
 SPBasicSuite basic{};
 basic.AcquireSuite=&acquire;basic.ReleaseSuite=&release;
 PF_InData in{};
 fl_mock::configure(in);
 in.pica_basicP=&basic;in.inter.add_param=&collect;in.inter.abort=&abort_ok;
 PF_OutData out{};
 if(effect(PF_Cmd_PARAMS_SETUP,&in,&out,nullptr,nullptr,nullptr)!=PF_Err_NONE)
  return 4;
 if(params.size()!=64)return 5;
 constexpr int w=320,h=180;
 std::vector<float> f32,f16,f8;
 try{
  f32=render(effect,in,PF_PixelFormat_ARGB128,w,h);
  f16=render(effect,in,PF_PixelFormat_ARGB64,w,h);
  f8=render(effect,in,PF_PixelFormat_ARGB32,w,h);
 }catch(int code){std::cerr<<"AEX error "<<code<<"\n";return 6;}
 size_t over_range=0,visible=0;
 float peak=0,quant_error8=0,quant_error16=0;
 for(size_t i=0;i<f32.size();i+=4){
  for(int c=0;c<3;c++){
   const float f=f32[i+c];
   if(!std::isfinite(f)||f<0)return 7;
   over_range+=f>1;
   visible+=f>1.f/255.f;
   peak=std::max(peak,f);
   const auto q8=std::round(std::clamp(f,0.f,1.f)*255.f)/255.f;
   const auto q16=std::round(std::clamp(f,0.f,1.f)*32768.f)/32768.f;
   quant_error8=std::max(quant_error8,std::abs(q8-f8[i+c]));
   quant_error16=std::max(quant_error16,std::abs(q16-f16[i+c]));
  }
  if(f32[i+3]!=1.f||f16[i+3]!=1.f||f8[i+3]!=1.f)return 8;
 }
 std::cout<<"AE_HDR_FLOAT32_UNCLAMPED peak="<<peak
          <<" over_1_pixels="<<over_range<<" visible="<<visible
          <<" max_quant_error8="<<quant_error8
          <<" max_quant_error16="<<quant_error16<<"\n";
 if(over_range<10||visible<100||quant_error8>1.e-6f||
    quant_error16>1.e-6f)return 9;
 FreeLibrary(dll);
 return 0;
}
