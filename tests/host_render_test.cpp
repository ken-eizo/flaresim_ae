// Real AEX entry-point host simulation (no After Effects process).
// Calls PF_Cmd_PARAMS_SETUP and PF_Cmd_RENDER with an SDK-compatible World
// Suite and a fully transparent input, reproducing the reported black
// Shape Layer in all three supported pixel depths. This exercises actual
// production render_worlds(), pixel(), put(), source settings and the RGB
// plus compositing-alpha logic, not a separately reimplemented mock.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <string>
#include <vector>
#include "AE_Effect.h"
#include "AE_EffectCBSuites.h"
#include "AE_Macros.h"
#include "mock_custom_ui_host.hpp"
#include "../src/FlareLensCustom.hpp"
#include "SPBasic.h"

using Main=PF_Err(*)(PF_Cmd,PF_InData*,PF_OutData*,PF_ParamDef**,PF_LayerDef*,void*);
static PF_PixelFormat g_format=PF_PixelFormat_ARGB32;
static PF_WorldSuite2 g_world{};
static std::vector<PF_ParamDef> g_params;
static PF_Err abort_ok(PF_ProgPtr){return PF_Err_NONE;}
static PF_Err collect(PF_ProgPtr,PF_ParamIndex index,PF_ParamDefPtr p){
 if(index!=-1||!p)return PF_Err_BAD_CALLBACK_PARAM;
 g_params.push_back(*p);fl_mock::activate(g_params.back());
 return PF_Err_NONE;
}
static PF_Err world_format(const PF_EffectWorld*,PF_PixelFormat* format){
 if(!format)return PF_Err_BAD_CALLBACK_PARAM;
 *format=g_format;
 return PF_Err_NONE;
}
static SPErr SPAPI acquire(const char* name,int32 version,const void** suite){
 if(!name||!suite||std::strcmp(name,kPFWorldSuite)!=0||
    version!=kPFWorldSuiteVersion2)return 1;
 *suite=&g_world;
 return 0;
}
static SPErr SPAPI release(const char*,int32){return 0;}

static int render(Main effect,PF_InData& in,int w,int h,PF_PixelFormat format,
                  int mode=1,int quick_preset=1,int catalog_id=0,
                  std::vector<float>* snapshot=nullptr,bool custom=false){
 g_format=format;
 const size_t stride=format==PF_PixelFormat_ARGB128?sizeof(PF_PixelFloat):
                     format==PF_PixelFormat_ARGB64?sizeof(PF_Pixel16):sizeof(PF_Pixel);
 const size_t n=size_t(w)*h;
 // The shape layer has no paths: its fully transparent pixels contain zeros.
 std::vector<uint64_t> input_store((n*stride+7)/8);
 std::vector<uint64_t> output_store((n*stride+7)/8);
 PF_LayerDef input{},output{};
 input.width=output.width=w;
 input.height=output.height=h;
 input.rowbytes=output.rowbytes=int(w*stride);
 input.data=reinterpret_cast<PF_Pixel*>(input_store.data());
 output.data=reinterpret_cast<PF_Pixel*>(output_store.data());
 in.width=w;in.height=h;
 in.downsample_x.num=1;in.downsample_x.den=1;
 in.downsample_y.num=1;in.downsample_y.den=1;
 PF_ParamDef input_param{};
 input_param.param_type=PF_Param_LAYER;
 input_param.u.ld=input;
 auto definitions=g_params;
 definitions[0].u.td.x_value=FLOAT2FIX(w*.38f);
 definitions[0].u.td.y_value=FLOAT2FIX(h*.30f);
 // Confirm manual emitter and normal compositing, with no detection.
 definitions[5].u.bd.value=FALSE; // Detect Bright Sources: P_AUTO=6 => capture[5]
 definitions[10].u.pd.value=mode; // Output Mode: P_OUTPUT_MODE=11 => capture[10]
 definitions[54].u.fs_d.value=1000.; // legacy raw point regression
 definitions[57].u.fs_d.value=0.; // optical regression uses point, not area
 definitions[8].u.pd.value=quick_preset;
 definitions[58].u.pd.value=custom?2:1; // custom or complete catalog
 definitions[59].u.sd.value=catalog_id?catalog_id:1;
 if(snapshot){
  // A lightweight, unblurred test image isolates optical lens identity.
  definitions[2].u.sd.value=32;
  definitions[15].u.pd.value=1; // 3 spectral wavelengths
  definitions[26].u.fs_d.value=0.;
  snapshot->clear();snapshot->reserve(n*3);
 }
 std::vector<PF_ParamDef*> params(definitions.size()+1);
 params[0]=&input_param;
 for(size_t i=0;i<definitions.size();i++)params[i+1]=&definitions[i];
 PF_OutData out{};
 const PF_Err err=effect(PF_Cmd_RENDER,&in,&out,params.data(),&output,nullptr);
 if(err){std::cerr<<"AEX transparent legacy render error "<<err<<"\n";return 1;}
 int visible=0,orphan_rgb=0,nonempty_alpha=0;
 double energy=0,max_alpha=0;
 for(size_t i=0;i<n;i++){
  float r=0,g=0,b=0,a=0;
  if(format==PF_PixelFormat_ARGB128){
   const auto& px=reinterpret_cast<const PF_PixelFloat*>(output.data)[i];
   r=px.red;g=px.green;b=px.blue;a=px.alpha;
  }else if(format==PF_PixelFormat_ARGB64){
   const auto& px=reinterpret_cast<const PF_Pixel16*>(output.data)[i];
   r=px.red/32768.f;g=px.green/32768.f;b=px.blue/32768.f;a=px.alpha/32768.f;
  }else{
   const auto& px=reinterpret_cast<const PF_Pixel*>(output.data)[i];
   r=px.red/255.f;g=px.green/255.f;b=px.blue/255.f;a=px.alpha/255.f;
  }
  if(!std::isfinite(r)||!std::isfinite(g)||!std::isfinite(b)||
     !std::isfinite(a)||a<0||a>1)return 2;
  if(snapshot){snapshot->push_back(r);snapshot->push_back(g);snapshot->push_back(b);}
  const float peak=std::max({r,g,b});
  if(peak>0)energy+=r+g+b;
  if(peak>0 && a==0)orphan_rgb++;
  if(a>0)nonempty_alpha++;
  if(peak>0 && a>0)visible++;
  max_alpha=std::max(max_alpha,double(a));
 }
 std::cout<<"HOSTLESS_AEX_RGBA mode="<<mode<<" transparent_input=1 format="
          <<int(format)<<" size="<<w<<"x"<<h
          <<" visible_rgba="<<visible<<" alpha_nonzero="<<nonempty_alpha
          <<" rgb_with_alpha_zero="<<orphan_rgb
          <<" peak_alpha="<<max_alpha<<" energy="<<energy<<"\n";
 if(visible<(snapshot?1:32)||orphan_rgb!=0||nonempty_alpha<visible||energy<=0)return 3;
 return 0;
}


// Simulate the host's SmartFX fast-path for an entirely empty shape layer.
// checkout_layer_pixels() deliberately succeeds but returns a null world.
// Previously smart_render() skipped checkout_output and silently returned
// PF_Err_NONE, leaving the frame black.
static PF_EffectWorld* g_smart_output=nullptr;
static int g_checked_out=0,g_checked_in=0;
static PF_Err checkout_empty(PF_ProgPtr,A_long,PF_EffectWorld** value){
 g_checked_out++;
 if(!value)return PF_Err_BAD_CALLBACK_PARAM;
 *value=nullptr;
 return PF_Err_NONE;
}
static PF_Err checkin_empty(PF_ProgPtr,A_long){
 g_checked_in++;
 return PF_Err_NONE;
}
static PF_Err checkout_final(PF_ProgPtr,PF_EffectWorld** value){
 if(!value)return PF_Err_BAD_CALLBACK_PARAM;
 *value=g_smart_output;
 return PF_Err_NONE;
}
static PF_Err checkout_params(PF_ProgPtr,PF_ParamIndex index,A_long,A_long,
                              A_u_long,PF_ParamDef* def){
 if(!def||index<1||index>int(g_params.size()))return PF_Err_INVALID_INDEX;
 *def=g_params[index-1];
 return PF_Err_NONE;
}
static PF_Err checkin_params(PF_ProgPtr,PF_ParamDef*){return PF_Err_NONE;}
static PF_LRect g_last_input_request{};
static PF_Err checkout_pre_input(PF_ProgPtr,PF_ParamIndex,A_long,
                                const PF_RenderRequest* req,
                                A_long,A_long,A_u_long,PF_CheckoutResult* result){
 if(!req||!result)return PF_Err_BAD_CALLBACK_PARAM;
 g_last_input_request=req->rect;
 *result={};
 result->result_rect=req->rect;
 result->max_result_rect=req->rect;
 return PF_Err_NONE;
}
static int render_smart_empty(Main effect,PF_InData& in){
 constexpr int w=960,h=540;
 std::vector<PF_Pixel> output_pixels(size_t(w)*h);
 PF_EffectWorld output{};
 output.width=w;output.height=h;output.rowbytes=w*sizeof(PF_Pixel);
 output.data=output_pixels.data();
 g_smart_output=&output;
 g_checked_out=g_checked_in=0;
 g_format=PF_PixelFormat_ARGB32;
 in.width=w;in.height=h;
 in.inter.checkout_param=&checkout_params;
 in.inter.checkin_param=&checkin_params;
 g_params[0].u.td.x_value=FLOAT2FIX(w*.38f);
 g_params[0].u.td.y_value=FLOAT2FIX(h*.30f);
 PF_PreRenderInput pre_input{};
 pre_input.output_request.rect={0,0,w,h};
 pre_input.output_request.channel_mask=PF_ChannelMask_ARGB;
 PF_PreRenderOutput pre_output{};
 PF_PreRenderCallbacks pre_callbacks{};
 pre_callbacks.checkout_layer=&checkout_pre_input;
 PF_PreRenderExtra pre_extra{&pre_input,&pre_output,&pre_callbacks};
 PF_OutData out{};
 const auto err1=effect(PF_Cmd_SMART_PRE_RENDER,&in,&out,nullptr,nullptr,&pre_extra);
 if(err1||!pre_output.pre_render_data)return 41;
 if(pre_output.result_rect.right!=w||pre_output.result_rect.bottom!=h||
    pre_output.max_result_rect.right!=w)return 42;
 PF_SmartRenderInput input{};
 input.pre_render_data=pre_output.pre_render_data;
 input.bitdepth=8;
 PF_SmartRenderCallbacks callbacks{};
 callbacks.checkout_layer_pixels=&checkout_empty;
 callbacks.checkin_layer_pixels=&checkin_empty;
 callbacks.checkout_output=&checkout_final;
 PF_SmartRenderExtra extra{&input,&callbacks};
 const PF_Err err=effect(PF_Cmd_SMART_RENDER,&in,&out,nullptr,nullptr,&extra);
 if(pre_output.delete_pre_render_data_func)
  pre_output.delete_pre_render_data_func(pre_output.pre_render_data);
 if(err)return 43;
 if(g_checked_out!=1||g_checked_in!=1)return 44;
 size_t visible=0,orphan=0;
 for(const auto& px:output_pixels){
  const bool rgb=px.red||px.green||px.blue;
  if(rgb&&px.alpha)visible++;
  if(rgb&&!px.alpha)orphan++;
 }
 std::cout<<"HOSTLESS_SMART_RENDER_NULL_INPUT "
          <<"visible="<<visible<<" orphan="<<orphan
          <<" checkouts="<<g_checked_out<<" checkins="<<g_checked_in<<"\n";
 if(visible<32||orphan)return 45;
 return 0;
}


// Same physical source and Nuke optics must give exactly the same pixels
// regardless of AE checkout ROI. Point Control remains full-resolution even
// when the host asks for a half-resolution buffer.
static int render_roi_and_downsample(Main effect,PF_InData& in){
 constexpr int full_w=960,full_h=540;
 auto render_world=[&](int source_w,int source_h,
                       int ds_num,int ds_den,
                       int origin_x,int origin_y,int width,int height,
                       float point_x,float point_y)->std::vector<PF_Pixel>{
  std::vector<PF_Pixel> buffer(size_t(width)*height);
  PF_LayerDef empty_input{};
  empty_input.width=source_w;
  empty_input.height=source_h;
  empty_input.data=nullptr; // input is an empty Shape Layer
  PF_LayerDef output{};
  output.width=width;output.height=height;
  output.origin_x=origin_x;output.origin_y=origin_y;
  output.rowbytes=width*sizeof(PF_Pixel);
  output.data=buffer.data();
  in.width=source_w;in.height=source_h;
  in.downsample_x.num=ds_num;in.downsample_x.den=ds_den;
  in.downsample_y.num=ds_num;in.downsample_y.den=ds_den;
  g_format=PF_PixelFormat_ARGB32;
  auto definitions=g_params;
  definitions[0].u.td.x_value=FLOAT2FIX(point_x);
  definitions[0].u.td.y_value=FLOAT2FIX(point_y);
  definitions[5].u.bd.value=FALSE; // manual emitter
  definitions[10].u.pd.value=1;    // Composite
  definitions[54].u.fs_d.value=1000.;
  definitions[55].u.bd.value=FALSE; // leave no test debug files
  definitions[56].u.bd.value=FALSE; // reference = identical full-quality
  definitions[57].u.fs_d.value=0.; // reference = raw point source
  PF_ParamDef input_def{};
  input_def.param_type=PF_Param_LAYER;
  input_def.u.ld=empty_input;
  std::vector<PF_ParamDef*> params(definitions.size()+1);
  params[0]=&input_def;
  for(size_t i=0;i<definitions.size();i++)params[i+1]=&definitions[i];
  PF_OutData out{};
  const PF_Err err=effect(PF_Cmd_RENDER,&in,&out,params.data(),&output,nullptr);
  if(err)throw int(err);
  return buffer;
 };
 try{
  const auto baseline=render_world(full_w,full_h,1,1,0,0,
                                   full_w,full_h,380,210);
  const int rx=240,ry=120,rw=440,rh=270;
  const auto sub=render_world(full_w,full_h,1,1,rx,ry,
                              rw,rh,380,210);
  for(int y=0;y<rh;y++)for(int x=0;x<rw;x++){
   const auto& a=baseline[size_t(y+ry)*full_w+x+rx];
   const auto& b=sub[size_t(y)*rw+x];
   if(std::memcmp(&a,&b,sizeof(PF_Pixel))!=0){
    std::cerr<<"REAL_AEX_ROI_MISMATCH pixel "<<x<<","<<y<<"\n";
    return 51;
   }
  }
  const auto half=render_world(2*full_w,2*full_h,1,2,0,0,
                               full_w,full_h,760,420);
  if(half.size()!=baseline.size())return 52;
  for(size_t i=0;i<half.size();i++)
   if(std::memcmp(&half[i],&baseline[i],sizeof(PF_Pixel))!=0){
    std::cerr<<"REAL_AEX_HALF_RES_MISMATCH pixel "<<i<<"\n";
    return 53;
   }
  std::cout<<"HOSTLESS_AEX_ROI_AND_DOWNSAMPLE_PIXELS_MATCHED"
           <<" full="<<full_w<<"x"<<full_h
           <<" ROI="<<rw<<"x"<<rh<<"@("<<rx<<","<<ry<<")"
           <<" half_input="<<2*full_w<<"x"<<2*full_h<<"\n";
  return 0;
 }catch(int code){
  std::cerr<<"ROI test AEX error "<<code<<"\n";
  return 54;
 }
}



// SDK SmartFX rule regression: result_rect must be INSIDE the demand rect,
// max_result_rect is the intrinsic support and independent of demand.
// Regression reproduces AE error 25::237 during transform/drag/MFR.
static int validate_requested_rect_contract(Main effect,PF_InData& in){
 in.inter.checkout_param=&checkout_params;
 in.inter.checkin_param=&checkin_params;
 PF_PreRenderCallbacks callback{};
 callback.checkout_layer=&checkout_pre_input;
 struct Case {PF_LRect request,expected;int ds_num,ds_den;bool detect;};
 const std::array<Case,9> cases{{
  {{0,0,960,540},{0,0,960,540},1,1,false},
  // Reproduced from actual After Effects 26.3: expanded host bounds used to
  // disqualify CUDA despite what_gpu=CUDA and a full valid sensor output.
  {{-192,-192,1152,732},{0,0,960,540},1,1,false},
  {{-192,-192,1152,732},{0,0,960,540},1,1,true},
  {{100,50,450,300},{100,50,450,300},1,1,false},
  {{-120,-40,140,180},{0,0,140,180},1,1,false},
  {{900,500,1300,800},{900,500,960,540},1,1,false},
  {{1100,900,1400,1200},{1100,900,1100,900},1,1,false},
  {{100,50,450,300},{100,50,450,300},1,1,true},
  {{0,0,480,270},{0,0,480,270},1,2,false}
 }};
 for(size_t i=0;i<cases.size();i++){
  const auto& t=cases[i];
  in.width=960;in.height=540;
  in.downsample_x.num=t.ds_num;in.downsample_x.den=t.ds_den;
  in.downsample_y.num=t.ds_num;in.downsample_y.den=t.ds_den;
  g_params[5].u.bd.value=t.detect?TRUE:FALSE;
  PF_PreRenderInput request{};
  request.what_gpu=PF_GPU_Framework_CUDA;
  request.output_request.rect=t.request;
  request.output_request.channel_mask=PF_ChannelMask_ARGB;
  PF_PreRenderOutput result{};
  PF_PreRenderExtra extra{&request,&result,&callback};
  PF_OutData out{};
  const PF_Err err=effect(PF_Cmd_SMART_PRE_RENDER,&in,&out,nullptr,nullptr,&extra);
  if(err)return 100+int(i);
  auto same=[](PF_LRect a,PF_LRect b){
    return a.left==b.left&&a.top==b.top&&
           a.right==b.right&&a.bottom==b.bottom;
  };
  const PF_LRect full=t.ds_num==1&&t.ds_den==2?
    PF_LRect{0,0,480,270}:PF_LRect{0,0,960,540};
  const bool contain=result.result_rect.left>=request.output_request.rect.left&&
    result.result_rect.top>=request.output_request.rect.top&&
    result.result_rect.right<=request.output_request.rect.right&&
    result.result_rect.bottom<=request.output_request.rect.bottom;
  if(!same(result.result_rect,t.expected)||!same(result.max_result_rect,full)||
     !contain||(result.flags&PF_RenderOutputFlag_RETURNS_EXTRA_PIXELS)){
    std::cerr<<"AE_25_237_REPRO_FAIL case="<<i<<" result=("
             <<result.result_rect.left<<","<<result.result_rect.top<<","
             <<result.result_rect.right<<","<<result.result_rect.bottom<<")\n";
    return 110+int(i);
  }
  // CUDA eligibility requires full-sensor input for proper sensor geometry
  // even if AE requests a sub-rectangle. Non-CUDA diagnostics keep ROI.
#ifdef FLARELENS_HAVE_CUDA
  if(!same(g_last_input_request,full))return 120+int(i);
  const bool visible=t.expected.right>t.expected.left&&
                     t.expected.bottom>t.expected.top;
  if(bool(result.flags&PF_RenderOutputFlag_GPU_RENDER_POSSIBLE)!=visible)
   return 140+int(i);
#else
  if(!t.detect&&!same(g_last_input_request,t.expected))return 120+int(i);
  if(t.detect&&!same(g_last_input_request,full))return 130+int(i);
#endif
  if(result.delete_pre_render_data_func)
   result.delete_pre_render_data_func(result.pre_render_data);
 }
 g_params[5].u.bd.value=FALSE;
 std::cout<<"AE_PRE_RENDER_REQUEST_BOUNDS_CONTRACT_PASSED "
          <<" cases=9 full_subrect_negative_padded_outside_auto_half\n";
 return 0;
}

#ifdef FLARELENS_HAVE_CUDA
// AE must be told which entire-frame SmartFX requests are safe for CUDA;
// otherwise a compiled CUDA AEX still runs ALL pixels on the CPU.
static int validate_host_gpu_dispatch(Main effect,PF_InData& in){
 constexpr int width=960,height=540;
 in.width=width;in.height=height;
 in.downsample_x.num=1;in.downsample_x.den=1;
 in.downsample_y.num=1;in.downsample_y.den=1;
 in.inter.checkout_param=&checkout_params;
 in.inter.checkin_param=&checkin_params;
 PF_PreRenderCallbacks cb{};
 cb.checkout_layer=&checkout_pre_input;
 const int gpu_modes[]={1,2,3,4,5};
 for(const int mode:gpu_modes){
  g_params[10].u.pd.value=mode;
  g_params[5].u.bd.value=FALSE;
  g_params[31].u.fs_d.value=0;  // Diffraction off
  PF_PreRenderInput req{};
  req.what_gpu=PF_GPU_Framework_CUDA;
  req.output_request.rect={0,0,width,height};
  req.output_request.channel_mask=PF_ChannelMask_ARGB;
  PF_PreRenderOutput output{};
  PF_PreRenderExtra extra{&req,&output,&cb};
  PF_OutData result{};
  const auto err=effect(PF_Cmd_SMART_PRE_RENDER,&in,&result,nullptr,nullptr,&extra);
  if(output.delete_pre_render_data_func)
   output.delete_pre_render_data_func(output.pre_render_data);
  if(err||(output.flags&PF_RenderOutputFlag_GPU_RENDER_POSSIBLE)==0){
   std::cerr<<"NO_CUDA_DISPATCH advertised mode "<<mode<<" err="<<err<<"\n";
   return 70+mode;
  }
 }
 for(const int mode:{6,7,8}){
  g_params[10].u.pd.value=mode;
  PF_PreRenderInput req{};
  req.what_gpu=PF_GPU_Framework_CUDA;
  req.output_request.rect={0,0,width,height};
  req.output_request.channel_mask=PF_ChannelMask_ARGB;
  PF_PreRenderOutput output{};
  PF_PreRenderExtra extra{&req,&output,&cb};
  PF_OutData result{};
  const auto err=effect(PF_Cmd_SMART_PRE_RENDER,&in,&result,nullptr,nullptr,&extra);
  if(output.delete_pre_render_data_func)
   output.delete_pre_render_data_func(output.pre_render_data);
  if(err||(output.flags&PF_RenderOutputFlag_GPU_RENDER_POSSIBLE)){
   std::cerr<<"INCORRECT_CUDA_DISPATCH mode "<<mode<<"\n";return 80+mode;
  }
 }
 // An AE 960x540 adaptive preview of a 1920x1080 layer is a
 // FULL low-resolution sensor. It must remain on CUDA during gizmo drag.
 g_params[10].u.pd.value=1;
 in.width=2*width;in.height=2*height;
 in.downsample_x.num=1;in.downsample_x.den=2;
 in.downsample_y.num=1;in.downsample_y.den=2;
 PF_PreRenderInput adaptive{};
 adaptive.what_gpu=PF_GPU_Framework_CUDA;
 adaptive.output_request.rect={0,0,width,height};
 PF_PreRenderOutput adaptive_out{};
 PF_PreRenderExtra adaptive_extra{&adaptive,&adaptive_out,&cb};
 PF_OutData adaptive_data{};
 const PF_Err adaptive_err=effect(PF_Cmd_SMART_PRE_RENDER,&in,
   &adaptive_data,nullptr,nullptr,&adaptive_extra);
 if(adaptive_out.delete_pre_render_data_func)
  adaptive_out.delete_pre_render_data_func(adaptive_out.pre_render_data);
 if(adaptive_err||
    (adaptive_out.flags&PF_RenderOutputFlag_GPU_RENDER_POSSIBLE)==0||
    adaptive_out.max_result_rect.right!=width||
    adaptive_out.max_result_rect.bottom!=height)return 91;
 // The full optical sensor is computed in CUDA even when AE requests an
 // ROI tile, with a full-frame input checkout and correct output crop.
 in.width=width;in.height=height;
 in.downsample_x.num=1;in.downsample_x.den=1;
 in.downsample_y.num=1;in.downsample_y.den=1;
 g_params[10].u.pd.value=2;
 PF_PreRenderInput partial{};
 partial.what_gpu=PF_GPU_Framework_CUDA;
 partial.output_request.rect={128,0,width,height};
 PF_PreRenderOutput output{};
 PF_PreRenderExtra extra{&partial,&output,&cb};
 PF_OutData result{};
 const auto err=effect(PF_Cmd_SMART_PRE_RENDER,&in,&result,nullptr,nullptr,&extra);
 if(output.delete_pre_render_data_func)
  output.delete_pre_render_data_func(output.pre_render_data);
 if(err||(output.flags&PF_RenderOutputFlag_GPU_RENDER_POSSIBLE)==0||
    g_last_input_request.left!=0||g_last_input_request.top!=0||
    g_last_input_request.right!=width||g_last_input_request.bottom!=height)
    return 90;
 // A request entirely outside the optical sensor must not advertise CUDA.
 PF_PreRenderInput outside{};
 outside.what_gpu=PF_GPU_Framework_CUDA;
 outside.output_request.rect={width+8,height+8,width+32,height+32};
 PF_PreRenderOutput outside_out{};
 PF_PreRenderExtra outside_extra{&outside,&outside_out,&cb};
 PF_OutData outside_data{};
 const auto outside_err=effect(PF_Cmd_SMART_PRE_RENDER,&in,
   &outside_data,nullptr,nullptr,&outside_extra);
 if(outside_out.delete_pre_render_data_func)
  outside_out.delete_pre_render_data_func(outside_out.pre_render_data);
 if(outside_err||(outside_out.flags&PF_RenderOutputFlag_GPU_RENDER_POSSIBLE))
  return 92;
 std::cout<<"REAL_AEX_CUDA_PRE_RENDER_DISPATCH_VALIDATED modes=1,2,3,4,5 plus_manual_ROI\n";
 return 0;
}
#endif

int main(){
 wchar_t exe[MAX_PATH]{};
 if(!GetModuleFileNameW(nullptr,exe,MAX_PATH))return 1;
 const auto path=std::filesystem::path(exe).parent_path()/L"FlareLensAE.aex";
 const auto dll=LoadLibraryW(path.c_str());
 if(!dll){std::cerr<<"Missing AEX to simulate "<<path.string()<<"\n";return 2;}
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
 g_params.reserve(63);
 PF_OutData out{};
 if(effect(PF_Cmd_PARAMS_SETUP,&in,&out,nullptr,nullptr,nullptr)!=PF_Err_NONE||
    g_params.size()!=63||out.num_params!=64)return 4;
 g_params[55].u.bd.value=TRUE; // exercise optional real-host telemetry
 if(const int e=render(effect,in,960,540,PF_PixelFormat_ARGB32))return 10+e;
 if(const int e=render(effect,in,640,360,PF_PixelFormat_ARGB64))return 20+e;
 if(const int e=render(effect,in,640,360,PF_PixelFormat_ARGB128))return 30+e;
 if(const int e=render_smart_empty(effect,in))return e;
 if(const int e=render_roi_and_downsample(effect,in))return e;
 if(const int e=validate_requested_rect_contract(effect,in))return e;
#ifdef FLARELENS_HAVE_CUDA
 if(const int e=validate_host_gpu_dispatch(effect,in))return e;
#endif
 // Rendering-path traces are required to diagnose actual AE checkouts, not
 // just to demonstrate Sources Preview in an isolated unit fixture.
 wchar_t temporary[MAX_PATH]={};
 if(!GetTempPathW(MAX_PATH,temporary))return 55;
 wchar_t filename[MAX_PATH]={};
 std::swprintf(filename,MAX_PATH,L"%lsFlareSimAE_host_%lu.log",
               temporary,GetCurrentProcessId());
 std::ifstream trace(std::filesystem::path(filename),std::ios::binary);
 if(!trace)return 56;
 const std::string records((std::istreambuf_iterator<char>(trace)),
                            std::istreambuf_iterator<char>());
 if(records.find("version=1.0.4")==std::string::npos||
    records.find("CPU_BEGIN")==std::string::npos||
    records.find("CPU_GHOST")==std::string::npos||
    records.find("PRE_RENDER")==std::string::npos)return 57;
 // Cover every render branch, not just Sources Preview or opaque Composite.
 for(int mode:{2,3,4,7,8})
  if(const int e=render(effect,in,640,360,PF_PixelFormat_ARGB32,mode))
   return 60+mode*4+e;
 // Optical identity must be controlled exclusively by the complete catalog.
 std::vector<float> nikkor,canon,fuji,last;
 const auto lens_pixels=[&](int id,std::vector<float>& pixels){
  return render(effect,in,320,180,PF_PixelFormat_ARGB128,
                2,1,id,&pixels)==0;
 };
 if(!lens_pixels(1,nikkor)||!lens_pixels(215,canon)||
    !lens_pixels(327,fuji)||!lens_pixels(1370,last))return 201;
 if(nikkor==canon||nikkor==fuji||nikkor==last||
    canon==fuji||canon==last||fuji==last){
  std::cerr<<"CPU_DISTINCT_LENS_PIXELS_COLLAPSED\n";return 203;
 }
 std::cout<<"ACTUAL_AEX_CPU_OPTICAL_LENS_IDENTITY_PASSED "
          <<"Catalog_Nikkor_Canon_Fuji_final_ID_1370\n";
 // Same prescription -> identical pixels; edited optical shape -> different
 // pixel field. This calls production AEX render with actual ARB data handle.
 if(g_params.size()!=63)return 204;
 auto* doc=static_cast<fl::custom::Document*>(
      fl_mock::lock(g_params[61].u.arb_d.value));
 if(!doc||!fl::custom::valid(*doc))return 205;
 std::vector<float> custom_original,custom_edited;
 if(render(effect,in,320,180,PF_PixelFormat_ARGB128,
           2,1,0,&custom_original,true)!=0)return 206;
 if(custom_original!=nikkor)return 207;
 const auto backup=*doc;
 if(!fl::custom::change_aperture(*doc,0,doc->elements[0].semi*.72f))
  return 208;
 if(render(effect,in,320,180,PF_PixelFormat_ARGB128,
           2,1,0,&custom_edited,true)!=0)return 209;
 *doc=backup;
 if(custom_edited==custom_original){
  std::cerr<<"ACTUAL_AEX_CUSTOM_LENS_EDIT_NO_PIXEL_CHANGE\n";return 210;
 }
 std::cout<<"ACTUAL_AEX_CUSTOM_LENS_CPU_PARITY_AND_EDIT_PASSED\n";
 std::cout<<"HOSTLESS_AEX_TRANSPARENT_LAYER_PASSED\n";
 FreeLibrary(dll);
 return 0;
}
