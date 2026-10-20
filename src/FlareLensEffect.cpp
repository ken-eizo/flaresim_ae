#define NOMINMAX
#if defined(_WIN32)
#include <windows.h>
#else
#define FLARELENS_MAC 1
#endif
#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <queue>
#include <string>
#include <vector>
#include "AEConfig.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#define PF_USE_NEW_WINDOW_UNION
#include "AE_EffectUI.h"
#include "AE_EffectSuites.h"
#include "AE_EffectGPUSuites.h"
#include "AEFX_SuiteHelper.h"
#include "FlareLensCore.hpp"
#include "FlareLensCatalog.hpp"
#include "FlareLensCatalogPicker.hpp"
#include "FlareLensCustomUI.hpp"
#include "FlareLensCpuParallel.hpp"
#include "FlareLensComposite.hpp"
#include "FlareLensSourceModel.hpp"
#include "FlareLensHostGeometry.hpp"
#include "FlareLensHostTrace.hpp"
#include "FlareLensOptics.hpp"
#ifdef FLARELENS_HAVE_CUDA
#include "FlareLensGpuBridge.hpp"
#include "FlareLensGpuDetection.hpp"
#include <cuda_runtime_api.h>
#elif defined(FLARELENS_HAVE_METAL)
#include "FlareLensMetal.hpp"
#endif

namespace {
// Existing parameter indices (0..7) are append-only for AE project compatibility.
enum { P_INPUT=0,P_LIGHT,P_GAIN,P_GRID,P_FOV,P_THRESHOLD,P_AUTO,P_MAXSOURCES,
       P_ADVANCED_GROUP,P_LENS_PRESET,P_LIGHT_TINT,P_OUTPUT_MODE,P_ADVANCED_END,
       P_OPTICS_START,P_APERTURE,P_ROTATION,P_SPECTRAL,P_JITTER,P_SEED,P_AUTOSEED,P_OPTICS_END,
       P_SOURCE_START,P_SOURCE_CAP,P_SOURCE_STEP,P_CLUSTER,P_SOURCE_END,
       P_POST_START,P_GHOST_BLUR,P_BLUR_PASSES,P_HAZE_GAIN,P_HAZE_RADIUS,P_HAZE_PASSES,
       P_STAR_GAIN,P_STAR_SCALE,P_POST_END,
       P_PREVIEW_START,P_PREVIEW,P_PREVIEW_GRID,P_PREVIEW_MAX,P_PREVIEW_STEP,P_PREVIEW_SPEC,P_PREVIEW_END,
       P_CAMERA_START,P_FOV_VERTICAL,P_USE_SENSOR,P_SENSOR_WIDTH,P_SENSOR_HEIGHT,P_FOCAL_LENGTH,P_CAMERA_END,
       P_PAIR_START,P_PAIR_MODE,P_PAIR_A,P_PAIR_B,P_PAIR_END,P_SOURCE_ALPHA,
       P_MANUAL_RADIANCE,P_HOST_TRACE,P_ADAPTIVE_PREVIEW,P_MANUAL_DIAMETER,
       P_LENS_SOURCE,P_CATALOG_INDEX,P_CATALOG_BROWSE,
       P_CUSTOM_LENS,P_CUSTOM_CLONE,P_FHD_STABLE_BRIGHTNESS,P_COUNT };
constexpr auto kVersion=PF_VERSION(1,0,5,PF_Stage_RELEASE,0);
#if defined(FLARELENS_HAVE_CUDA)
constexpr auto kNativeGpuFramework=PF_GPU_Framework_CUDA;
#elif defined(FLARELENS_HAVE_METAL)
constexpr auto kNativeGpuFramework=PF_GPU_Framework_METAL;
#endif
constexpr A_long kInputCheckout=1;

struct RGB {float r=0,g=0,b=0,a=0;};
struct FrameArgs {
 float light_x=0,light_y=0,gain=0,fov=40,threshold=2.f;
 float tint_r=1,tint_g=1,tint_b=1,manual_radiance=64.f;
 float manual_diameter_px=96.f;
 int grid=64,max_sources=512,output_mode=1;
 int lens_source=1,catalog_index=1;
 fl::custom::Document custom_lens{};
 int aperture_blades=0,spectral=3,jitter=2,seed=0,pair_mode=0,pair_a=1,pair_b=2;
 float aperture_rotation=0,source_cap=0;
 int scan_step=4,cluster_radius=0;
 float ghost_blur=.0008f,haze_gain=0,haze_radius=.15f,star_gain=0,star_scale=.15f;
 int ghost_blur_passes=3,haze_passes=3;
 bool preview=false,auto_seed=true,auto_detect=false,use_sensor=false,source_alpha=false;
 bool adaptive_preview=true;
 bool fhd_stable_brightness=false; // experimental, existing projects unchanged
 bool host_trace=true;
 int preview_grid=12,preview_max=128,preview_step=8,preview_spectral=3;
 float fov_vertical=0,sensor_w=36,sensor_h=24,camera_focal=50;
};

// Catalog and per-effect custom prescription are intentionally exclusive.
// Never use process-global mutable lens state: AE MFR renders in parallel.
int active_catalog_id(const FrameArgs& args){
 return args.lens_source==1?args.catalog_index:0;
}
const LensSystem* selected_lens(const FrameArgs& args,LensSystem& local_custom){
 const LensSystem* lens=nullptr;
 if(args.lens_source==1)lens=fl::catalog::lens_by_index(args.catalog_index);
 else if(args.lens_source==2){
  if(!fl::custom::valid(args.custom_lens))return nullptr;
  local_custom=fl::custom::make_lens(args.custom_lens);
  lens=&local_custom;
 }
 if(!lens||lens->num_surfaces()<2||
    !std::isfinite(lens->focal_length)||lens->focal_length<=0.f)return nullptr;
 return lens;
}

RGB pixel(const PF_LayerDef* world,int x,int y,int depth){
 if(!world||!world->data||x<0||x>=world->width||y<0||y>=world->height)return {};
 const auto* row=reinterpret_cast<const uint8_t*>(world->data)+size_t(y)*world->rowbytes;
 if(depth==32){const auto& p=reinterpret_cast<const PF_PixelFloat*>(row)[x];return {p.red,p.green,p.blue,p.alpha};}
 if(depth==16){const auto& p=reinterpret_cast<const PF_Pixel16*>(row)[x];constexpr float k=1.f/32768.f;return {p.red*k,p.green*k,p.blue*k,p.alpha*k};}
 const auto& p=reinterpret_cast<const PF_Pixel*>(row)[x];constexpr float k=1.f/255.f;
 return {p.red*k,p.green*k,p.blue*k,p.alpha*k};
}
void put(PF_LayerDef* world,int x,int y,int depth,RGB v){
 auto* row=reinterpret_cast<uint8_t*>(world->data)+size_t(y)*world->rowbytes;
 if(depth==32){auto& p=reinterpret_cast<PF_PixelFloat*>(row)[x];p.red=v.r;p.green=v.g;p.blue=v.b;p.alpha=v.a;return;}
 auto quant=[](float a,float scale){return std::isfinite(a)?int(std::lround(std::clamp(a,0.f,1.f)*scale)):0;};
 if(depth==16){auto& p=reinterpret_cast<PF_Pixel16*>(row)[x];p.red=quant(v.r,32768);p.green=quant(v.g,32768);p.blue=quant(v.b,32768);p.alpha=quant(v.a,32768);return;}
 auto& p=reinterpret_cast<PF_Pixel*>(row)[x];p.red=quant(v.r,255);p.green=quant(v.g,255);p.blue=quant(v.b,255);p.alpha=quant(v.a,255);
}

PF_Err params_setup(PF_InData* in,PF_OutData* out){
 PF_InData* in_data=in;
 PF_ParamDef def{};
 PF_ADD_POINT("Light Position",50,50,FALSE,1); // AE point gizmo allows off-frame lights
 AEFX_CLR_STRUCT(def);
 PF_ADD_FLOAT_EXPONENTIAL_SLIDER("Flare Gain",0,100000,0,5000,0,1000,2,PF_ValueDisplayFlag_NONE,FALSE,2.0,2);
 AEFX_CLR_STRUCT(def);
 PF_ADD_SLIDER("Ray Grid",2,2048,2,1024,64,3);
 AEFX_CLR_STRUCT(def);
 PF_ADD_FLOAT_SLIDER("Horizontal FOV",1,150,1,150,0,40,1,0,0,4);
 AEFX_CLR_STRUCT(def);
 PF_ADD_FLOAT_SLIDER("Source Threshold",0,1000,0,10,0,2,2,0,0,5);
 AEFX_CLR_STRUCT(def);
 PF_ADD_CHECKBOX("Detect Bright Sources","Enable",FALSE,0,6);
 AEFX_CLR_STRUCT(def);
 PF_ADD_SLIDER("Maximum Sources",1,2048,1,512,512,7);
 // Append-only AE-native controls; old comp parameter indices remain stable.
 AEFX_CLR_STRUCT(def);
 PF_ADD_TOPICX("Lens and Output",PF_ParamFlag_START_COLLAPSED,8);
 // Reserved nonvisual parameter slot: removed Quick Presets from the UI.
 // Keeping this invisible slot avoids accidental changes to unrelated ids.
 AEFX_CLR_STRUCT(def);
 def.ui_flags=PF_PUI_INVISIBLE;
 PF_ADD_POPUP("Reserved",1,1,"-",9);
 AEFX_CLR_STRUCT(def);
 PF_ADD_COLOR("Light Tint",255,255,255,10);
 AEFX_CLR_STRUCT(def);
 PF_ADD_POPUP("Output Mode",8,1,"Composite|Flare Only|Sources Preview|Ghost Only|Haze Only|Starburst Only|Ray Hits Preview|Optical Diagnostics",11);
 AEFX_CLR_STRUCT(def);
 PF_END_TOPIC(12);
 AEFX_CLR_STRUCT(def); PF_ADD_TOPICX("Optical System",PF_ParamFlag_START_COLLAPSED,13);
 AEFX_CLR_STRUCT(def); PF_ADD_SLIDER("Aperture Blades",0,16,0,16,0,14);
 AEFX_CLR_STRUCT(def); PF_ADD_ANGLE("Aperture Rotation",0,15);
 AEFX_CLR_STRUCT(def); PF_ADD_POPUP("Spectral Samples",5,1,"3 RGB|5 Wavelengths|7 Wavelengths|9 Wavelengths|11 Wavelengths",16);
 AEFX_CLR_STRUCT(def); PF_ADD_POPUP("Pupil Sampling",4,4,"Regular|Stratified|Halton|Golden Angle",17);
 AEFX_CLR_STRUCT(def); PF_ADD_SLIDER("Jitter Seed",0,65535,0,1000,0,18);
 AEFX_CLR_STRUCT(def); PF_ADD_CHECKBOX("Auto Jitter Seed","On",TRUE,0,19);
 AEFX_CLR_STRUCT(def); PF_END_TOPIC(20);
 AEFX_CLR_STRUCT(def); PF_ADD_TOPICX("Source Detection",PF_ParamFlag_START_COLLAPSED,21);
 AEFX_CLR_STRUCT(def); PF_ADD_FLOAT_SLIDER("Source Cap",0,100000,0,100,0,0,2,0,0,22);
 AEFX_CLR_STRUCT(def); PF_ADD_SLIDER("Scan Step",1,32,1,16,4,23);
 AEFX_CLR_STRUCT(def); PF_ADD_SLIDER("Cluster Radius",0,500,0,150,0,24);
 AEFX_CLR_STRUCT(def); PF_END_TOPIC(25);
 AEFX_CLR_STRUCT(def); PF_ADD_TOPICX("Optical Post-Processing",PF_ParamFlag_START_COLLAPSED,26);
 AEFX_CLR_STRUCT(def); PF_ADD_FLOAT_SLIDER("Ghost Blur Radius",0,1,0,.1f,0,.0008f,4,0,0,27);
 AEFX_CLR_STRUCT(def); PF_ADD_SLIDER("Ghost Blur Passes",1,6,1,6,2,28);
 AEFX_CLR_STRUCT(def); PF_ADD_FLOAT_SLIDER("Veiling Haze Gain",0,10000,0,1000,0,0,2,0,0,29);
 AEFX_CLR_STRUCT(def); PF_ADD_FLOAT_SLIDER("Haze Radius",0,1,0,.5f,0,.15f,3,0,0,30);
 AEFX_CLR_STRUCT(def); PF_ADD_SLIDER("Haze Blur Passes",1,6,1,6,3,31);
 AEFX_CLR_STRUCT(def); PF_ADD_FLOAT_SLIDER("Diffraction Gain",0,10000,0,1000,0,0,2,0,0,32);
 AEFX_CLR_STRUCT(def); PF_ADD_FLOAT_SLIDER("Diffraction Scale",.001,1,.001,.5,0,.15,3,0,0,33);
 AEFX_CLR_STRUCT(def); PF_END_TOPIC(34);
 AEFX_CLR_STRUCT(def); PF_ADD_TOPICX("Interactive Preview",PF_ParamFlag_START_COLLAPSED,35);
 AEFX_CLR_STRUCT(def); PF_ADD_CHECKBOX("Use Preview Quality","Enable",FALSE,0,36);
 AEFX_CLR_STRUCT(def); PF_ADD_SLIDER("Preview Ray Grid",2,128,2,64,12,37);
 AEFX_CLR_STRUCT(def); PF_ADD_SLIDER("Preview Max Sources",1,256,1,128,128,38);
 AEFX_CLR_STRUCT(def); PF_ADD_SLIDER("Preview Scan Step",1,32,1,16,8,39);
 AEFX_CLR_STRUCT(def); PF_ADD_POPUP("Preview Spectral",5,1,"3 RGB|5 Wavelengths|7 Wavelengths|9 Wavelengths|11 Wavelengths",40);
 AEFX_CLR_STRUCT(def); PF_END_TOPIC(41);
 AEFX_CLR_STRUCT(def); PF_ADD_TOPICX("Camera and Field of View",PF_ParamFlag_START_COLLAPSED,42);
 AEFX_CLR_STRUCT(def); PF_ADD_FLOAT_SLIDER("Vertical FOV Override",0,150,0,100,0,0,2,0,0,43);
 AEFX_CLR_STRUCT(def); PF_ADD_CHECKBOX("Use Sensor Dimensions","Enable",FALSE,0,44);
 AEFX_CLR_STRUCT(def); PF_ADD_FLOAT_SLIDER("Sensor Width mm",1,100,1,70,0,36,2,0,0,45);
 AEFX_CLR_STRUCT(def); PF_ADD_FLOAT_SLIDER("Sensor Height mm",1,100,1,70,0,24,2,0,0,46);
 AEFX_CLR_STRUCT(def); PF_ADD_FLOAT_SLIDER("Camera Focal Length mm",1,2000,1,200,0,50,2,0,0,47);
 AEFX_CLR_STRUCT(def); PF_END_TOPIC(48);
 AEFX_CLR_STRUCT(def); PF_ADD_TOPICX("Ghost Pair Selection",PF_ParamFlag_START_COLLAPSED,49);
 AEFX_CLR_STRUCT(def); PF_ADD_POPUP("Pair Mode",3,1,"All Pairs|Only Selected Pair|Exclude Selected Pair",50);
 AEFX_CLR_STRUCT(def); PF_ADD_SLIDER("Front Surface Index",1,64,1,32,1,51);
 AEFX_CLR_STRUCT(def); PF_ADD_SLIDER("Rear Surface Index",1,64,1,32,2,52);
 AEFX_CLR_STRUCT(def); PF_END_TOPIC(53);
 AEFX_CLR_STRUCT(def); PF_ADD_CHECKBOX("Gate Detection with Input Alpha","Enable",FALSE,0,54);
 // Manual light is an HDR scene-linear emitter, not an LDR color swatch.
 // The upstream Nuke node samples scene-linear HDR source RGB from the input.
 // A single manual source with RGB=1 usually quantizes to black at 8bpc.
 AEFX_CLR_STRUCT(def);
 PF_ADD_FLOAT_EXPONENTIAL_SLIDER("Manual Source Radiance",0,1000000,0,1000,0,64,2,
                                 PF_ValueDisplayFlag_NONE,FALSE,2.0,55);
 AEFX_CLR_STRUCT(def);
 PF_ADD_CHECKBOX("Development Host Trace","On",FALSE,0,56);
 AEFX_CLR_STRUCT(def);
 PF_ADD_CHECKBOX("Adaptive Quality for Reduced Resolution","Enable",TRUE,0,57);
 AEFX_CLR_STRUCT(def);
 // The 96 px luminous disk, at scene-linear 64 RGB and Nuke's 4x4 scan,
 // represents ~452 HDR blocks: ~28,953 source energy without a
 // misleading manually typed 30,000-radiance point source.
 PF_ADD_FLOAT_SLIDER("Manual Source Diameter (px)",0,4096,0,512,0,96,1,0,0,58);
 AEFX_CLR_STRUCT(def);
 PF_ADD_POPUP("Lens Selection",2,1,"Complete Nuke Catalog|Custom Lens",59);
 AEFX_CLR_STRUCT(def);
 PF_ADD_SLIDER("Nuke Lens Catalog ID",1,fl::catalog::kCount,
               1,fl::catalog::kCount,1,60);
 AEFX_CLR_STRUCT(def);
 PF_ADD_BUTTON("Browse All Nuke Lenses","Search 1370 Lenses...",
               0,PF_ParamFlag_SUPERVISE,61);
 // AE-owned Drawbot UI + serialized optical document. No Vulkan surface or
 // AE worker-thread UI ownership, dynamic allocation per UI DRAW is bounded.
 AEFX_CLR_STRUCT(def);
 PF_ArbitraryH initial=nullptr;
 PF_Err err=fl::custom::ae::make_default(in,&initial);
 if(err)return err;
 PF_ADD_ARBITRARY2("Custom Lens",fl::custom::ae::kEditorWidth,
   fl::custom::ae::kEditorHeight,PF_ParamFlag_SUPERVISE,PF_PUI_CONTROL,
   initial,62,(void*)0x464c4355u);
 AEFX_CLR_STRUCT(def);
 PF_ADD_BUTTON("Initialize Custom Lens","Copy Catalog Lens to Custom",
               0,PF_ParamFlag_SUPERVISE,63);
 // APPEND-ONLY: older AE projects retain their original parameter IDs.
 // Off preserves exact Nuke energy scatter and published v1.0.4 appearance.
 AEFX_CLR_STRUCT(def);
 PF_ADD_CHECKBOX("Match FHD Flare Brightness","Enable (Manual Light)",FALSE,0,64);
 PF_CustomUIInfo ci{};
 ci.events=PF_CustomEFlag_EFFECT;
 ci.comp_ui_width=ci.comp_ui_height=0;
 ci.layer_ui_width=ci.layer_ui_height=0;
 ci.preview_ui_width=ci.preview_ui_height=0;
 ci.comp_ui_alignment=ci.layer_ui_alignment=ci.preview_ui_alignment=PF_UIAlignment_NONE;
 err=(*in->inter.register_ui)(in->effect_ref,&ci);
 if(err)return err;
 out->num_params=P_COUNT;
 return PF_Err_NONE;
}

FrameArgs from_legacy(PF_InData* in_data,PF_ParamDef* params[]){
 FrameArgs a;
 a.light_x=FIX_2_FLOAT(params[P_LIGHT]->u.td.x_value);
 a.light_y=FIX_2_FLOAT(params[P_LIGHT]->u.td.y_value);
 a.gain=float(params[P_GAIN]->u.fs_d.value);
 a.grid=params[P_GRID]->u.sd.value;
 a.fov=float(params[P_FOV]->u.fs_d.value);
 a.threshold=float(params[P_THRESHOLD]->u.fs_d.value);
 a.auto_detect=params[P_AUTO]->u.bd.value!=0;
 a.max_sources=params[P_MAXSOURCES]->u.sd.value;
 a.lens_source=params[P_LENS_SOURCE]->u.pd.value;
 a.catalog_index=params[P_CATALOG_INDEX]->u.sd.value;
 // Never touch AE-owned Arbitrary Data on catalog-mode render workers.
 if(a.lens_source==2 && params[P_CUSTOM_LENS] &&
    !fl::custom::ae::read(in_data,params[P_CUSTOM_LENS]->u.arb_d.value,a.custom_lens))
   a.lens_source=0; // invalid custom prescription: fail closed; no fallback.
 a.output_mode=params[P_OUTPUT_MODE]->u.pd.value;
 a.tint_r=params[P_LIGHT_TINT]->u.cd.value.red/255.f;
 a.tint_g=params[P_LIGHT_TINT]->u.cd.value.green/255.f;
 a.tint_b=params[P_LIGHT_TINT]->u.cd.value.blue/255.f;
 a.manual_radiance=float(params[P_MANUAL_RADIANCE]->u.fs_d.value);
 a.manual_diameter_px=float(params[P_MANUAL_DIAMETER]->u.fs_d.value);
 a.host_trace=params[P_HOST_TRACE]->u.bd.value!=0;
 a.aperture_blades=params[P_APERTURE]->u.sd.value;
 a.aperture_rotation=FIX_2_FLOAT(params[P_ROTATION]->u.ad.value);
 a.spectral=2*std::clamp<int>(params[P_SPECTRAL]->u.pd.value,1,5)+1;
 a.jitter=params[P_JITTER]->u.pd.value-1;
 a.seed=params[P_SEED]->u.sd.value;
 a.auto_seed=params[P_AUTOSEED]->u.bd.value!=0;
 a.source_cap=float(params[P_SOURCE_CAP]->u.fs_d.value);
 a.scan_step=params[P_SOURCE_STEP]->u.sd.value;
 a.cluster_radius=params[P_CLUSTER]->u.sd.value;
 a.ghost_blur=float(params[P_GHOST_BLUR]->u.fs_d.value);
 a.ghost_blur_passes=params[P_BLUR_PASSES]->u.sd.value;
 a.haze_gain=float(params[P_HAZE_GAIN]->u.fs_d.value);
 a.haze_radius=float(params[P_HAZE_RADIUS]->u.fs_d.value);
 a.haze_passes=params[P_HAZE_PASSES]->u.sd.value;
 a.star_gain=float(params[P_STAR_GAIN]->u.fs_d.value);
 a.star_scale=float(params[P_STAR_SCALE]->u.fs_d.value);
 a.preview=params[P_PREVIEW]->u.bd.value!=0;
 a.preview_grid=params[P_PREVIEW_GRID]->u.sd.value;
 a.preview_max=params[P_PREVIEW_MAX]->u.sd.value;
 a.preview_step=params[P_PREVIEW_STEP]->u.sd.value;
 a.preview_spectral=2*std::clamp<int>(params[P_PREVIEW_SPEC]->u.pd.value,1,5)+1;
 a.fov_vertical=float(params[P_FOV_VERTICAL]->u.fs_d.value);
 a.use_sensor=params[P_USE_SENSOR]->u.bd.value!=0;
 a.sensor_w=float(params[P_SENSOR_WIDTH]->u.fs_d.value);
 a.sensor_h=float(params[P_SENSOR_HEIGHT]->u.fs_d.value);
 a.camera_focal=float(params[P_FOCAL_LENGTH]->u.fs_d.value);
 a.pair_mode=params[P_PAIR_MODE]->u.pd.value-1;
 a.pair_a=params[P_PAIR_A]->u.sd.value;
 a.pair_b=params[P_PAIR_B]->u.sd.value;
 a.source_alpha=params[P_SOURCE_ALPHA]->u.bd.value!=0;
 a.adaptive_preview=params[P_ADAPTIVE_PREVIEW]->u.bd.value!=0;
 a.fhd_stable_brightness=params[P_FHD_STABLE_BRIGHTNESS]->u.bd.value!=0;
 return a;
}

PF_Err checkout_args(PF_InData* in,FrameArgs& a){
 PF_ParamDef p{};
 auto load=[&](int index,auto setter)->PF_Err{
  AEFX_CLR_STRUCT(p);
  const PF_Err err=PF_CHECKOUT_PARAM(in,index,in->current_time,in->time_step,in->time_scale,&p);
  if(err)return err;
  setter(p);
  return PF_CHECKIN_PARAM(in,&p);
 };
 PF_Err e=PF_Err_NONE;
 if((e=load(P_LIGHT,[&](const PF_ParamDef& v){a.light_x=FIX_2_FLOAT(v.u.td.x_value);a.light_y=FIX_2_FLOAT(v.u.td.y_value);})))return e;
 if((e=load(P_GAIN,[&](const PF_ParamDef& v){a.gain=float(v.u.fs_d.value);})))return e;
 if((e=load(P_GRID,[&](const PF_ParamDef& v){a.grid=v.u.sd.value;})))return e;
 if((e=load(P_FOV,[&](const PF_ParamDef& v){a.fov=float(v.u.fs_d.value);})))return e;
 if((e=load(P_THRESHOLD,[&](const PF_ParamDef& v){a.threshold=float(v.u.fs_d.value);})))return e;
 if((e=load(P_AUTO,[&](const PF_ParamDef& v){a.auto_detect=v.u.bd.value!=0;})))return e;
 if((e=load(P_MAXSOURCES,[&](const PF_ParamDef& v){a.max_sources=v.u.sd.value;})))return e;
 
 if((e=load(P_LENS_SOURCE,[&](const PF_ParamDef& v){a.lens_source=v.u.pd.value;})))return e;
 if((e=load(P_CATALOG_INDEX,[&](const PF_ParamDef& v){a.catalog_index=v.u.sd.value;})))return e;
 // Only checkout a custom prescription when the user selected Custom Lens.
 // This avoids unnecessary AE Handle calls during catalog SmartFX/MFR.
 if(a.lens_source==2){
  bool document_valid=false;
  if((e=load(P_CUSTOM_LENS,[&](const PF_ParamDef& v){
   document_valid=fl::custom::ae::read(in,v.u.arb_d.value,a.custom_lens);
  })))return e;
  if(!document_valid)return PF_Err_BAD_CALLBACK_PARAM;
 }
 if((e=load(P_OUTPUT_MODE,[&](const PF_ParamDef& v){a.output_mode=v.u.pd.value;})))return e;
 if((e=load(P_LIGHT_TINT,[&](const PF_ParamDef& v){
  a.tint_r=v.u.cd.value.red/255.f;
  a.tint_g=v.u.cd.value.green/255.f;
  a.tint_b=v.u.cd.value.blue/255.f;
 })))return e;
 if((e=load(P_APERTURE,[&](const PF_ParamDef& v){a.aperture_blades=v.u.sd.value;})))return e;
 if((e=load(P_ROTATION,[&](const PF_ParamDef& v){a.aperture_rotation=FIX_2_FLOAT(v.u.ad.value);})))return e;
 if((e=load(P_SPECTRAL,[&](const PF_ParamDef& v){a.spectral=2*std::clamp<int>(v.u.pd.value,1,5)+1;})))return e;
 if((e=load(P_JITTER,[&](const PF_ParamDef& v){a.jitter=v.u.pd.value-1;})))return e;
 if((e=load(P_SEED,[&](const PF_ParamDef& v){a.seed=v.u.sd.value;})))return e;
 if((e=load(P_AUTOSEED,[&](const PF_ParamDef& v){a.auto_seed=v.u.bd.value!=0;})))return e;
 if((e=load(P_SOURCE_CAP,[&](const PF_ParamDef& v){a.source_cap=float(v.u.fs_d.value);})))return e;
 if((e=load(P_SOURCE_STEP,[&](const PF_ParamDef& v){a.scan_step=v.u.sd.value;})))return e;
 if((e=load(P_CLUSTER,[&](const PF_ParamDef& v){a.cluster_radius=v.u.sd.value;})))return e;
 if((e=load(P_GHOST_BLUR,[&](const PF_ParamDef& v){a.ghost_blur=float(v.u.fs_d.value);})))return e;
 if((e=load(P_BLUR_PASSES,[&](const PF_ParamDef& v){a.ghost_blur_passes=v.u.sd.value;})))return e;
 if((e=load(P_HAZE_GAIN,[&](const PF_ParamDef& v){a.haze_gain=float(v.u.fs_d.value);})))return e;
 if((e=load(P_HAZE_RADIUS,[&](const PF_ParamDef& v){a.haze_radius=float(v.u.fs_d.value);})))return e;
 if((e=load(P_HAZE_PASSES,[&](const PF_ParamDef& v){a.haze_passes=v.u.sd.value;})))return e;
 if((e=load(P_STAR_GAIN,[&](const PF_ParamDef& v){a.star_gain=float(v.u.fs_d.value);})))return e;
 if((e=load(P_STAR_SCALE,[&](const PF_ParamDef& v){a.star_scale=float(v.u.fs_d.value);})))return e;
 if((e=load(P_PREVIEW,[&](const PF_ParamDef& v){a.preview=v.u.bd.value!=0;})))return e;
 if((e=load(P_PREVIEW_GRID,[&](const PF_ParamDef& v){a.preview_grid=v.u.sd.value;})))return e;
 if((e=load(P_PREVIEW_MAX,[&](const PF_ParamDef& v){a.preview_max=v.u.sd.value;})))return e;
 if((e=load(P_PREVIEW_STEP,[&](const PF_ParamDef& v){a.preview_step=v.u.sd.value;})))return e;
 if((e=load(P_PREVIEW_SPEC,[&](const PF_ParamDef& v){a.preview_spectral=2*std::clamp<int>(v.u.pd.value,1,5)+1;})))return e;
 if((e=load(P_FOV_VERTICAL,[&](const PF_ParamDef& v){a.fov_vertical=float(v.u.fs_d.value);})))return e;
 if((e=load(P_USE_SENSOR,[&](const PF_ParamDef& v){a.use_sensor=v.u.bd.value!=0;})))return e;
 if((e=load(P_SENSOR_WIDTH,[&](const PF_ParamDef& v){a.sensor_w=float(v.u.fs_d.value);})))return e;
 if((e=load(P_SENSOR_HEIGHT,[&](const PF_ParamDef& v){a.sensor_h=float(v.u.fs_d.value);})))return e;
 if((e=load(P_FOCAL_LENGTH,[&](const PF_ParamDef& v){a.camera_focal=float(v.u.fs_d.value);})))return e;
 if((e=load(P_PAIR_MODE,[&](const PF_ParamDef& v){a.pair_mode=v.u.pd.value-1;})))return e;
 if((e=load(P_PAIR_A,[&](const PF_ParamDef& v){a.pair_a=v.u.sd.value;})))return e;
 if((e=load(P_PAIR_B,[&](const PF_ParamDef& v){a.pair_b=v.u.sd.value;})))return e;
 if((e=load(P_SOURCE_ALPHA,[&](const PF_ParamDef& v){a.source_alpha=v.u.bd.value!=0;})))return e;
 if((e=load(P_MANUAL_RADIANCE,[&](const PF_ParamDef& v){a.manual_radiance=float(v.u.fs_d.value);})))return e;
 if((e=load(P_MANUAL_DIAMETER,[&](const PF_ParamDef& v){a.manual_diameter_px=float(v.u.fs_d.value);})))return e;
 if((e=load(P_HOST_TRACE,[&](const PF_ParamDef& v){a.host_trace=v.u.bd.value!=0;})))return e;
 if((e=load(P_ADAPTIVE_PREVIEW,[&](const PF_ParamDef& v){a.adaptive_preview=v.u.bd.value!=0;})))return e;
 if((e=load(P_FHD_STABLE_BRIGHTNESS,[&](const PF_ParamDef& v){a.fhd_stable_brightness=v.u.bd.value!=0;})))return e;
 return PF_Err_NONE;
}

// cluster_radius is in rendered pixels (the slider times the downsample).
std::vector<fl::Source> find_sources(const PF_LayerDef* input,int depth,
                                     const FrameArgs& a,int width,int height,
                                     const fl::HostGeometry& geometry,
                                     float cluster_radius){
 std::vector<fl::Source> sources;
 if(width<=0||height<=0)return sources;
 // AE's point control arrives in rendered pixels and supports off-frame lights.
 const float px=std::isfinite(a.light_x)?geometry.normalized_x(a.light_x):0.5f;
 const float py=std::isfinite(a.light_y)?geometry.normalized_y(a.light_y):0.5f;
 if(!a.auto_detect){
  // Nuke normally has one source per *image block*, not one per luminous
  // object. Compress a constant-radiance circular HDR patch into its
  // corresponding Nuke block-source sum without changing Fresnel or gain.
  float r=fl::nuke_equivalent_manual_radiance(
      a.manual_radiance,a.manual_diameter_px,geometry.downsample_x,
      geometry.downsample_y,a.scan_step,a.max_sources);
  if(a.fhd_stable_brightness)
    r*=fl::fhd_reference_emission_scale(width,height);
  sources.push_back({px,py,r*a.tint_r,r*a.tint_g,r*a.tint_b});
  return sources;
 }
 const int limit=std::clamp(a.preview?a.preview_max:a.max_sources,1,2048);
 const int candidate_limit=cluster_radius>0?std::min(8192,limit*4):limit;
 // A bounded priority queue retains the best K samples, not the entire frame.
 struct Ranked {float score;int index;fl::Source src;};
 auto better=[](const Ranked& x,const Ranked& y){
  return x.score>y.score||(x.score==y.score&&x.index<y.index);
 };
 std::priority_queue<Ranked,std::vector<Ranked>,decltype(better)> best(better);
 const int stride=std::clamp(a.preview?a.preview_step:a.scan_step,1,32);
 // Match upstream Nuke's exact integer binning: the final floor(w/ds)
 // bucket also contains right/bottom remainder pixels. A ceil grid would
 // silently add extra emitters and change flare brightness on odd formats.
 const int dw=std::max(1,width/stride),dh=std::max(1,height/stride);
 // No intermediate downsample image or second full-frame allocation.
 for(int by=0;by<dh;by++)for(int bx=0;bx<dw;bx++){
  const int x=bx*stride,y=by*stride;
  const int end_x=(bx==dw-1)?width:std::min(width,x+stride);
  const int end_y=(by==dh-1)?height:std::min(height,y+stride);
  double sum_r=0,sum_g=0,sum_b=0,sum_a=0;
  int count=0;
  for(int yy=y;yy<end_y;yy++)for(int xx=x;xx<end_x;xx++){
   const auto p=pixel(input,xx-(input?input->origin_x:0),
                            yy-(input?input->origin_y:0),depth);
   if(!std::isfinite(p.r)||!std::isfinite(p.g)||!std::isfinite(p.b))continue;
   sum_r+=p.r;sum_g+=p.g;sum_b+=p.b;
   sum_a+=a.source_alpha?std::clamp(p.a,0.f,1.f):1.f;
   count++;
  }
  if(count==0)continue;
  float r=float(sum_r/count),g=float(sum_g/count),b=float(sum_b/count);
  float score=.2126f*r+.7152f*g+.0722f*b;
  if(!std::isfinite(score))continue;
  // Preserve Nuke's sequence: cap before mask, then threshold the masked luma.
  if(a.source_cap>0&&score>a.source_cap){
   const float k=a.source_cap/score;
   r*=k;g*=k;b*=k;score=a.source_cap;
  }
  const float mask=float(sum_a/count);
  r*=mask;g*=mask;b*=mask;score*=mask;
  if(score<a.threshold)continue;
  // Nuke reports the nominal block centre, not the centroid of the
  // remainder pixels included in the final (right/bottom) bucket.
  const float cx=.5f*(x+std::min(x+stride,width));
  const float cy=.5f*(y+std::min(y+stride,height));
  const Ranked cand{score,by*dw+bx,{cx/width,cy/height,
                         r*a.tint_r,g*a.tint_g,b*a.tint_b}};
  if(int(best.size())<candidate_limit)best.push(cand);
  else if(better(cand,best.top())){best.pop();best.push(cand);}
 }
 std::vector<Ranked> ranked;
 ranked.reserve(best.size());
 while(!best.empty()){ranked.push_back(best.top());best.pop();}
 std::sort(ranked.begin(),ranked.end(),better);
 for(const auto& s:ranked)sources.push_back(s.src);
 if(cluster_radius>0&&sources.size()>1){
  const float radius2=cluster_radius*cluster_radius;
  std::vector<fl::Source> clustered;
  std::vector<uint8_t> consumed(sources.size());
  for(size_t i=0;i<sources.size();i++){
   if(consumed[i])continue;
   auto sum=sources[i];
   float weight=std::max(0.f,.2126f*sum.r+.7152f*sum.g+.0722f*sum.b);
   float ax=sum.x*weight,ay=sum.y*weight;
   for(size_t j=i+1;j<sources.size();j++){
    if(consumed[j])continue;
    const float dx=(sources[j].x-sources[i].x)*width;
    const float dy=(sources[j].y-sources[i].y)*height;
    if(dx*dx+dy*dy>radius2)continue;
    const float wj=std::max(0.f,.2126f*sources[j].r+.7152f*sources[j].g+.0722f*sources[j].b);
    ax+=sources[j].x*wj;ay+=sources[j].y*wj;weight+=wj;
    sum.r+=sources[j].r;sum.g+=sources[j].g;sum.b+=sources[j].b;
    consumed[j]=1;
   }
   if(weight>0){sum.x=ax/weight;sum.y=ay/weight;}
   clustered.push_back(sum);
  }
  sources.swap(clustered);
  if(sources.size()>size_t(limit)){
   std::stable_sort(sources.begin(),sources.end(),[](const auto& x,const auto& y){
    return .2126f*x.r+.7152f*x.g+.0722f*x.b > .2126f*y.r+.7152f*y.g+.0722f*y.b;
   });
   sources.resize(limit);
  }
 }
 return sources;
}


fl::Settings optical_settings(const FrameArgs& a,const PF_InData* in){
 fl::Settings s{};
 s.gain=std::isfinite(a.gain)?std::max(0.f,a.gain):0.f;
 // During AE adaptive-resolution interaction, reduce only the optical
 // sampling density. The full-resolution final render is unaffected.
 // This toggle can be disabled for identical high-density half-res exports.
 const bool reduced=in&&a.adaptive_preview&&
   ((in->downsample_x.num>0&&in->downsample_x.den>0&&
     in->downsample_x.num<in->downsample_x.den)||
    (in->downsample_y.num>0&&in->downsample_y.den>0&&
     in->downsample_y.num<in->downsample_y.den));
 s.grid=std::clamp(a.preview?a.preview_grid:
     reduced?std::min(a.grid,std::max(2,a.preview_grid)):a.grid,2,2048);
 s.max_pairs=2048;
 s.fov_h_deg=std::isfinite(a.fov)?std::clamp(a.fov,1.f,150.f):40.f;
 s.fov_v_deg=std::isfinite(a.fov_vertical)?std::clamp(a.fov_vertical,0.f,150.f):0.f;
 if(a.use_sensor&&a.sensor_w>0&&a.sensor_h>0&&a.camera_focal>0){
  s.fov_h_deg=2.f*std::atan(a.sensor_w/(2.f*a.camera_focal))*57.2957795f;
  s.fov_v_deg=2.f*std::atan(a.sensor_h/(2.f*a.camera_focal))*57.2957795f;
 }
 s.aperture_blades=std::clamp(a.aperture_blades,0,16);
 s.aperture_rotation_deg=a.aperture_rotation;
 s.spectral_samples=(a.preview||reduced)?a.preview_spectral:a.spectral;
 // Nuke's preview compensates reduced HDR source tile density by the
 // *square* of the scan-step ratio. AE adaptive downsampling also reduces
 // the pixel count, so account for the actual world-to-layer area scale.
 // Keep full-resolution/Nuke-exact rendering unchanged.
 if(a.auto_detect&&(a.preview||reduced)){
  const float base_step=float(std::clamp(a.scan_step,1,32));
  const float preview_step=float(std::clamp(a.preview_step,1,32));
  const float ratio=preview_step/base_step;
  float pixel_area=1.f;
  if(in&&in->downsample_x.num>0&&in->downsample_y.num>0)
    pixel_area=std::clamp(float(
      double(in->downsample_x.den)/in->downsample_x.num*
      double(in->downsample_y.den)/in->downsample_y.num),1.f,256.f);
  s.gain*=ratio*ratio*pixel_area;
 }
 s.pupil_jitter=std::clamp(a.jitter,0,3);
 s.jitter_seed=a.auto_seed&&in&&in->time_step>0?
     int(in->current_time/in->time_step):a.seed;
 s.pair_mode=std::clamp(a.pair_mode,0,2);
 s.pair_surface_a=a.pair_a-1;
 s.pair_surface_b=a.pair_b-1;
 return s;
}

PF_Err render_worlds(PF_InData* in,PF_OutData* out,const PF_LayerDef* input,
                     PF_LayerDef* output,const FrameArgs& args){
 // AE may checkout a NULL input world for an entirely empty Shape Layer.
 // This effect can still synthesize a manual optical emitter without pixels.
 // pixel(nullptr,...) already supplies transparent black safely.
 if(!output||!output->data)return PF_Err_BAD_CALLBACK_PARAM;
 PF_WorldSuite2* ws=nullptr;
 PF_PixelFormat format=PF_PixelFormat_INVALID;
 PF_Err e=AEFX_AcquireSuite(in,out,kPFWorldSuite,kPFWorldSuiteVersion2,nullptr,(void**)&ws);
 PF_PixelFormat source_format=PF_PixelFormat_INVALID;
 if(!e&&ws)e=ws->PF_GetPixelFormat(output,&format);
 if(!e&&ws&&input&&input->data)e=ws->PF_GetPixelFormat(input,&source_format);
 if(ws)AEFX_ReleaseSuite(in,out,kPFWorldSuite,kPFWorldSuiteVersion2,nullptr);
 if(e)return e;
 if(source_format==PF_PixelFormat_INVALID)source_format=format;
 if(format!=PF_PixelFormat_ARGB32&&format!=PF_PixelFormat_ARGB64&&format!=PF_PixelFormat_ARGB128)
  return PF_Err_UNRECOGNIZED_PARAM_TYPE;
 const int depth=format==PF_PixelFormat_ARGB128?32:format==PF_PixelFormat_ARGB64?16:8;
 if(source_format!=PF_PixelFormat_ARGB32&&
    source_format!=PF_PixelFormat_ARGB64&&
    source_format!=PF_PixelFormat_ARGB128)return PF_Err_UNRECOGNIZED_PARAM_TYPE;
 const int source_depth=source_format==PF_PixelFormat_ARGB128?32:
                        source_format==PF_PixelFormat_ARGB64?16:8;
 const int w=output->width,h=output->height;
 if(w<=0||h<=0)return PF_Err_NONE;
 const fl::HostGeometry geometry=fl::host_geometry(
  in?in->width:0,in?in->height:0,
  in?in->downsample_x.num:0,in?int(in->downsample_x.den):0,
  in?in->downsample_y.num:0,in?int(in->downsample_y.den):0,
  output->origin_x,output->origin_y,w,h);
 if(!geometry.valid())return PF_Err_OUT_OF_MEMORY;
 const int frame_w=geometry.full_w,frame_h=geometry.full_h;
 char render_begin[750]={};
 std::snprintf(render_begin,sizeof(render_begin),
  "in_wh=%ldx%ld downsample=%ld/%lu,%ld/%lu input_buf=%ldx%ld@%ld,%ld input_null=%d"
  " output_buf=%dx%d@%d,%d frame=%dx%d fmt=%ld src_fmt=%ld"
  " mode=%d auto=%d light=(%.3f,%.3f) gain=%.3f radiance=%.3f"
  " tint=(%.3f,%.3f,%.3f) grid=%d blur=%g lens=%d",
  in?long(in->width):0,in?long(in->height):0,
  in?long(in->downsample_x.num):0,in?static_cast<unsigned long>(in->downsample_x.den):0,
  in?long(in->downsample_y.num):0,in?static_cast<unsigned long>(in->downsample_y.den):0,
  input?long(input->width):0,input?long(input->height):0,
  input?long(input->origin_x):0,input?long(input->origin_y):0,
  int(!input||!input->data),w,h,geometry.output_x,geometry.output_y,
  frame_w,frame_h,long(format),long(source_format),args.output_mode,
  int(args.auto_detect),args.light_x,args.light_y,args.gain,
  args.manual_radiance,args.tint_r,args.tint_g,args.tint_b,args.grid,
  double(args.ghost_blur),active_catalog_id(args));
 fl::host_trace(args.host_trace,"CPU_BEGIN",render_begin);
 fl::Settings settings=optical_settings(args,in);
 // The manual position is a fraction of the whole sensor, never of an AE
 // checkout ROI. For detected sources we must sample the full requested
 // frame; the source scanner uses input origin for ROI mapping in pixel().
 // Cluster Radius is a layer-pixel distance, so it scales with the world.
 const float pixel_scale=std::sqrt(geometry.downsample_x*geometry.downsample_y);
 const auto sources=find_sources(input,source_depth,args,frame_w,frame_h,
                                 geometry,float(args.cluster_radius)*pixel_scale);
 // A ray-hit diagnostic shows valid optical geometry independently of
 // Fresnel energy, exposure and 8/16/32bpc output quantization.
 if(std::clamp(args.output_mode,1,8)==7)settings.debug_ray_density=true;
 // Nuke's auxiliary named channels become explicit, useful AE output modes.
 const int mode=std::clamp(args.output_mode,1,8);
 const bool render_ghost=(mode<=2||mode==4||mode==7||mode==8);
 const bool render_haze_pass=(mode<=2||mode==5);
 const bool render_star_pass=(mode<=2||mode==6);
 LensSystem local_custom;
 const LensSystem* lens=selected_lens(args,local_custom);
 if(!lens){
  char facts[340]{};
  const int id=active_catalog_id(args);
  std::snprintf(facts,sizeof(facts),"Invalid embedded Nuke lens id=%d file=%s",
     id,fl::catalog::filename(id)?fl::catalog::filename(id):"(unknown)");
  fl::host_trace(true,"LENS_MISSING",facts);
  return PF_Err_BAD_CALLBACK_PARAM;
 }
 if(lens->num_surfaces()<2||!std::isfinite(lens->focal_length)||
    lens->focal_length<=0){
  fl::host_trace(true,"LENS_NOT_RENDERABLE",
     "The original Nuke data has fewer than two optical surfaces");
  return PF_Err_BAD_CALLBACK_PARAM;
 }
 auto ghost=!render_ghost?fl::Image{}:fl::render_sources_parallel(
    *lens,sources,settings,frame_w,frame_h,
    {[](void* ctx)->int{return int(PF_ABORT(static_cast<PF_InData*>(ctx)));},in},
    3);
 if(ghost.abort_error)return static_cast<PF_Err>(ghost.abort_error);
 if(args.host_trace){
  char facts[600]={};
  std::snprintf(facts,sizeof(facts),
   "mode=%d lens_surfaces=%d sources=%zu first_source=(%.6f,%.6f)"
   " src_radiance=(%.3f,%.3f,%.3f) pair_count=%d rays=%llu valid=%llu"
   " hits=%llu ghost_peak=%g ghost_sum=%g ghost_lit=%d",
   mode,lens->num_surfaces(),sources.size(),
   sources.empty()?0:sources[0].x,sources.empty()?0:sources[0].y,
   sources.empty()?0:sources[0].r,sources.empty()?0:sources[0].g,
   sources.empty()?0:sources[0].b,ghost.diagnostics.active_pairs,
   static_cast<unsigned long long>(ghost.diagnostics.traced_rays),
   static_cast<unsigned long long>(ghost.diagnostics.valid_rays),
   static_cast<unsigned long long>(ghost.diagnostics.sensor_hits),
   double(ghost.diagnostics.max_luminance),
   ghost.diagnostics.sum_luminance,ghost.diagnostics.screen_samples);
  fl::host_trace(args.host_trace,"CPU_GHOST",facts);
 }
 if(!ghost.rgb.empty()&&args.ghost_blur>0){
  const int radius=std::max(1,int(std::round(args.ghost_blur*std::hypot(float(frame_w),float(frame_h)))));
  fl::box_blur_rgb(ghost.rgb,frame_w,frame_h,radius,args.ghost_blur_passes);
 }
 // Accumulate post-process passes into a single full-frame RGB image instead
 // of retaining three large images until the host has been composited.
 std::vector<float> post;
 const auto merge_post=[&](){
  if(post.empty())return;
  if(ghost.rgb.empty()){ghost.rgb.swap(post);return;}
  for(size_t i=0;i<ghost.rgb.size();i++)ghost.rgb[i]+=post[i];
 };
 if(render_haze_pass&&args.haze_gain>0){
  fl::render_haze(post,sources,frame_w,frame_h,args.haze_gain,args.haze_radius,args.haze_passes,
      args.auto_detect?std::clamp(args.preview?args.preview_step:args.scan_step,1,32):1);
  merge_post();
 }
 if(render_star_pass&&args.star_gain>0){
  fl::render_diffraction(post,sources,frame_w,frame_h,settings.aperture_blades,
       settings.aperture_rotation_deg,args.star_gain,args.star_scale);
  merge_post();
 }
 float optical_diagnostic_peak=0.f;
 if((mode==8||args.host_trace)&&!ghost.rgb.empty()){
  for(const float v:ghost.rgb)
   if(std::isfinite(v))optical_diagnostic_peak=std::max(optical_diagnostic_peak,v);
 }
 std::vector<uint8_t> source_marks;
 if(mode==3){
  source_marks.resize(size_t(w)*h);
  for(const auto& src:sources){
   const int sx=int(std::floor(src.x*frame_w))-geometry.output_x;
   const int sy=int(std::floor(src.y*frame_h))-geometry.output_y;
   if(sx < -6 || sx>=w+6 || sy < -6 || sy>=h+6)continue;
   for(int dy=-5;dy<=5;dy++)for(int dx=-5;dx<=5;dx++){
    const int xx=sx+dx,yy=sy+dy;
    if(xx<0||xx>=w||yy<0||yy>=h)continue;
    const int r2=dx*dx+dy*dy;
    if((r2>=9&&r2<=25)||(dx==0&&r2<=25)||(dy==0&&r2<=25))
     source_marks[size_t(yy)*w+xx]=1;
   }
  }
 }
 for(int y=0;y<h;y++){
  if((y&31)==0){const PF_Err cancelled=PF_ABORT(in);if(cancelled)return cancelled;}
  for(int x=0;x<w;x++){
   const int fx=geometry.global_x(x),fy=geometry.global_y(y);
   auto color=pixel(input,fx-(input?input->origin_x:0),
                          fy-(input?input->origin_y:0),source_depth);
   if(mode==8){
    // Deliberately non-photometric debugging: normalized optical radiance
    // beneath an unmistakable host-side 8-stage status strip.
    // This is NOT a production Nuke output mode or an exposure adjustment.
    color={0,0,0,1};
    if(fx>=0&&fy>=0&&fx<frame_w&&fy<frame_h&&!ghost.rgb.empty()){
     const size_t k=(size_t(fy)*frame_w+fx)*3;
     const float scale=optical_diagnostic_peak>0?
       0.85f/optical_diagnostic_peak:0.f;
     color.r=std::clamp(ghost.rgb[k]*scale,0.f,1.f);
     color.g=std::clamp(ghost.rgb[k+1]*scale,0.f,1.f);
     color.b=std::clamp(ghost.rgb[k+2]*scale,0.f,1.f);
    }
    if(y<26){
     const int stage=std::clamp(8*x/std::max(1,w),0,7);
     const bool pass[8]={
      !sources.empty(),
      !sources.empty()&&std::max({sources[0].r,sources[0].g,sources[0].b})>0,
      lens->num_surfaces()>=2,
      ghost.diagnostics.active_pairs>0,
      ghost.diagnostics.valid_rays>0,
      ghost.diagnostics.sensor_hits>0,
      ghost.diagnostics.max_luminance>0,
      optical_diagnostic_peak>1.f/255.f
     };
     color.r=pass[stage]?.12f:1.f;
     color.g=pass[stage]?1.f:.04f;
     color.b=stage%2?0.22f:0.04f;
     if(x%std::max(1,w/8)<2)color={1,1,1,1};
    }
   }else if(mode==3){
    const float mark=source_marks[size_t(y)*w+x]?1.f:0.f;
    color.r=color.r*.12f+mark;
    color.g=color.g*.12f+mark;
    color.b=color.b*.12f+mark;
    color.a=1.f;
   }else{
    if(mode!=1){color.r=0;color.g=0;color.b=0;color.a=1.f;}
    if(mode==7 && ghost.diagnostics.screen_samples==0){
     // Red means the selected optical rays did not hit this camera sensor.
     color.r=.25f;
    }
    const size_t i=(size_t(std::clamp(fy,0,frame_h-1))*frame_w+
                    size_t(std::clamp(fx,0,frame_w-1)))*3;
    if(!ghost.rgb.empty()&&fx>=0&&fy>=0&&fx<frame_w&&fy<frame_h){
     const float emit_r=ghost.rgb[i],emit_g=ghost.rgb[i+1],emit_b=ghost.rgb[i+2];
     // AE receives premultiplied RGBA. Input alpha may be ZERO on an
     // otherwise empty Shape Layer, even when optical emitters are visible.
     // Preserve upstream physical RGB, but make the created light compositable.
     if(mode==1)color.a=fl::alpha_for_emission(color.a,emit_r,emit_g,emit_b);
     color.r+=emit_r;color.g+=emit_g;color.b+=emit_b;
    }

   }
   put(output,x,y,depth,color);
  }
 }
 if(args.host_trace){
  char status[500]={};
  std::snprintf(status,sizeof(status),
   "output_buf=%dx%d@%d,%d optical_peak_after_post=%g "
   "source_count=%zu valid_ray=%llu hits=%llu alpha_mode=%s",
   w,h,geometry.output_x,geometry.output_y,
   double(optical_diagnostic_peak),sources.size(),
   static_cast<unsigned long long>(ghost.diagnostics.valid_rays),
   static_cast<unsigned long long>(ghost.diagnostics.sensor_hits),
   mode==1?"Composite-derived":"Opaque-component");
  fl::host_trace(true,"CPU_DONE",status);
 }
 return PF_Err_NONE;
}

PF_Err pre_render(PF_InData* in,PF_PreRenderExtra* extra){
 if(!extra||!extra->input||!extra->output||!extra->cb)return PF_Err_BAD_CALLBACK_PARAM;
 FrameArgs args;
 PF_Err err=checkout_args(in,args);
 if(err)return err;
 PF_RenderRequest request=extra->input->output_request;
 if(args.host_trace){
  char facts[530]={};
  std::snprintf(facts,sizeof(facts),
   "pre_wh=%ldx%ld request=(%ld,%ld,%ld,%ld) mode=%d"
   " gpu=%d auto=%d gain=%.3f radiance=%.3f light=(%.3f,%.3f)",
   long(in->width),long(in->height),
   long(request.rect.left),long(request.rect.top),
   long(request.rect.right),long(request.rect.bottom),
   args.output_mode,int(extra->input->what_gpu),int(args.auto_detect),
   args.gain,args.manual_radiance,args.light_x,args.light_y);
  fl::host_trace(true,"PRE_RENDER",facts);
 }
 // SDK invariant: result_rect is ALWAYS a subset of output_request.rect,
 // whereas max_result_rect is the full intrinsic support and must NOT
 // depend on the current request. AE can demand small, negative-origin or
 // downsampled rectangles during gizmo dragging, masks and MFR.
 const auto sensor=fl::host_geometry(in->width,in->height,
    in->downsample_x.num,int(in->downsample_x.den),
    in->downsample_y.num,int(in->downsample_y.den),0,0,0,0);
 const PF_LRect full={0,0,sensor.full_w,sensor.full_h};
 const PF_LRect wanted=extra->input->output_request.rect;
 PF_LRect visible{};
 visible.left=std::max(full.left,wanted.left);
 visible.top=std::max(full.top,wanted.top);
 visible.right=std::max(visible.left,std::min(full.right,wanted.right));
 visible.bottom=std::max(visible.top,std::min(full.bottom,wanted.bottom));
 // CUDA always evaluates a complete optical sensor, even for a tiny AE
 // requested tile. The composited input plate must therefore be checked out
 // at full-sensor resolution. Otherwise AE silently chooses the costly CPU
 // path for ordinary manual-light ROI requests.
 // CPU rendering still checks out only its actual ROI for manual lights.
#if defined(FLARELENS_HAVE_CUDA) || defined(FLARELENS_HAVE_METAL)
 const bool native_gpu_input_eligible=extra->input->what_gpu==kNativeGpuFramework&&
   (args.output_mode==1||args.output_mode==2||
    args.output_mode==3||args.output_mode==4||
    args.output_mode==5)&&args.star_gain<=0;
 request.rect=(args.auto_detect||native_gpu_input_eligible)?full:visible;
#else
 request.rect=args.auto_detect?full:visible;
#endif
 request.channel_mask=PF_ChannelMask_ARGB;
 PF_CheckoutResult input_result{};
 err=extra->cb->checkout_layer(in->effect_ref,P_INPUT,kInputCheckout,&request,
                             in->current_time,in->time_step,in->time_scale,&input_result);
 if(err)return err;
 auto* retained=static_cast<FrameArgs*>(std::malloc(sizeof(FrameArgs)));
 if(!retained)return PF_Err_OUT_OF_MEMORY;
 *retained=args;
 extra->output->pre_render_data=retained;
 extra->output->delete_pre_render_data_func=[](void* p){std::free(p);};
 // A manually placed light can emit flare even on a transparent input.
 extra->output->result_rect=visible;
 extra->output->max_result_rect=full;
 // Do not set RETURNS_EXTRA_PIXELS: we are not returning a larger output
 // than AE requested, even though the optical calculation is full-frame.
 // Advertise CUDA for an in-frame ROI as well as full sensor requests:
 // the GPU optical renderer computes the FULL sensor and crops to the
 // requested AE tile, preserving physical source/sensor coordinates.
 // CPU remains the explicit fallback when the host provides no CUDA.
#if defined(FLARELENS_HAVE_CUDA) || defined(FLARELENS_HAVE_METAL)
 const auto& output_rect=extra->input->output_request.rect;
 const bool valid_scale=in->downsample_x.num>0&&in->downsample_x.den>0&&
   in->downsample_y.num>0&&in->downsample_y.den>0;
 // Adobe can request a 960x640 FULL preview of a 1920x1280 comp during
 // dragging. Treat that as a full low-resolution sensor, not a tile.
 const bool full_rect=output_rect.left==0&&output_rect.top==0&&
   output_rect.right==full.right&&output_rect.bottom==full.bottom;
 const bool gpu_output=args.output_mode==1||args.output_mode==2||
                       args.output_mode==3||args.output_mode==4||
                       args.output_mode==5;
 const bool scaled_preview=args.adaptive_preview&&
  ((in->downsample_x.num>0&&in->downsample_x.num<in->downsample_x.den)||
   (in->downsample_y.num>0&&in->downsample_y.num<in->downsample_y.den));
 const int scan_step=args.preview||scaled_preview?
   args.preview_step:args.scan_step;
#ifdef FLARELENS_HAVE_CUDA
 const bool detectable=!args.auto_detect||
  fl::gpu::detection_scratch_bytes(full.right,full.bottom,
                                   std::clamp(scan_step,1,32))>0;
#else
 const bool detectable=!args.auto_detect||
  fl::gpu::metal_detection_scratch_bytes(full.right,full.bottom,
                                         std::clamp(scan_step,1,32))>0;
#endif
 // AE routinely expands the output request BEYOND the layer bounds (e.g.
 // -192,-192..2112,2112 for a 1920-square composition). The actual
 // emitted optical image is bounded by result_rect = requested ∩ sensor.
 // Restrict GPU eligibility by the nonempty INTERSECTION, not by whether
 // the entire host request is inside the sensor. The old test forced CPU
 // for every normal expanded request in AE even when CUDA was available.
 const bool gpu_geometry=visible.right>visible.left&&
                         visible.bottom>visible.top;
#ifdef FLARELENS_HAVE_METAL
 // The Metal scratch allocator deliberately rejects >1GiB per full image.
 // Never advertise GPU rendering for a frame the backend will reject:
 // otherwise AE dispatches a doomed GPU render instead of its CPU path.
 const bool gpu_memory_eligible=full.right>0&&full.bottom>0&&
   size_t(full.right)*size_t(full.bottom)<=
     (size_t(1024)*1024*1024/16);
#else
 const bool gpu_memory_eligible=true;
#endif
 const bool can_gpu=extra->input->what_gpu==kNativeGpuFramework&&
   detectable&&gpu_output&&args.star_gain<=0&&gpu_memory_eligible&&
   valid_scale&&gpu_geometry&&full.right>0&&full.bottom>0&&
   in->width<=16384&&in->height<=16384;
 if(can_gpu)extra->output->flags|=PF_RenderOutputFlag_GPU_RENDER_POSSIBLE;
 if(args.host_trace){
  char facts[230]{};
  std::snprintf(facts,sizeof(facts),
     "can_gpu=%d cuda_framework=%d full_rect=%d valid_scale=%d "
     "mode=%d star_gain=%g blur=%g haze=%g",
     int(can_gpu),int(extra->input->what_gpu),int(full_rect),
     int(valid_scale),args.output_mode,double(args.star_gain),
     double(args.ghost_blur),double(args.haze_gain));
  fl::host_trace(true,"GPU_ELIGIBILITY",facts);
 }
#endif
 return PF_Err_NONE;
}

PF_Err smart_render(PF_InData* in,PF_OutData* out,PF_SmartRenderExtra* extra){
 if(!extra||!extra->input||!extra->cb||!extra->input->pre_render_data)
  return PF_Err_BAD_CALLBACK_PARAM;
 PF_EffectWorld* input=nullptr;
 PF_EffectWorld* output=nullptr;
 PF_Err err=extra->cb->checkout_layer_pixels(in->effect_ref,kInputCheckout,&input);
 if(err)return err;
 // Even with no input pixels, the output is still a full-frame emitter.
 err=extra->cb->checkout_output(in->effect_ref,&output);
 if(!err&&output)err=render_worlds(in,out,input,output,
                      *static_cast<const FrameArgs*>(extra->input->pre_render_data));
 else if(!err)err=PF_Err_BAD_CALLBACK_PARAM;
 const PF_Err checkin=extra->cb->checkin_layer_pixels(in->effect_ref,kInputCheckout);
 return err?err:checkin;
}

#if defined(FLARELENS_HAVE_CUDA) || defined(FLARELENS_HAVE_METAL)
PF_Err smart_render_gpu(PF_InData* in,PF_OutData* out,PF_SmartRenderExtra* extra){
 if(!extra||!extra->input||!extra->cb||!extra->input->pre_render_data||
    extra->input->what_gpu!=kNativeGpuFramework)return PF_Err_BAD_CALLBACK_PARAM;
 const auto& args=*static_cast<const FrameArgs*>(extra->input->pre_render_data);
 fl::host_trace(args.host_trace,"GPU_ENTER","host GPU selector requested");
 const bool supported_mode=args.output_mode==1||args.output_mode==2||
                           args.output_mode==3||args.output_mode==4||
                           args.output_mode==5;
 if(!supported_mode||args.star_gain>0)
  return PF_Err_BAD_CALLBACK_PARAM;
 PF_EffectWorld* input=nullptr;
 PF_EffectWorld* output=nullptr;
 PF_Err err=extra->cb->checkout_layer_pixels(in->effect_ref,kInputCheckout,&input);
 if(err)return err;
 err=extra->cb->checkout_output(in->effect_ref,&output);
 if(err||!output){
  extra->cb->checkin_layer_pixels(in->effect_ref,kInputCheckout);
  return err?err:PF_Err_BAD_CALLBACK_PARAM;
 }
 PF_WorldSuite2* ws=nullptr;
 PF_GPUDeviceSuite1* gs=nullptr;
 PF_PixelFormat fmt=PF_PixelFormat_INVALID;
 err=AEFX_AcquireSuite(in,out,kPFWorldSuite,kPFWorldSuiteVersion2,nullptr,(void**)&ws);
 if(!err&&ws)err=ws->PF_GetPixelFormat(output,&fmt);
 if(ws)AEFX_ReleaseSuite(in,out,kPFWorldSuite,kPFWorldSuiteVersion2,nullptr);
 if(!err&&fmt!=PF_PixelFormat_GPU_BGRA128)err=PF_Err_UNRECOGNIZED_PARAM_TYPE;
 if(!err)err=AEFX_AcquireSuite(in,out,kPFGPUDeviceSuite,kPFGPUDeviceSuiteVersion1,
                              nullptr,(void**)&gs);
 if(!err&&gs){
  PF_GPUDeviceInfo device_info{};
  err=gs->GetDeviceInfo(in->effect_ref,extra->input->device_index,&device_info);
  if(!err&&(device_info.device_framework!=kNativeGpuFramework||
            !device_info.compatibleB||!device_info.command_queuePV))
    err=PF_Err_BAD_CALLBACK_PARAM;
#ifdef FLARELENS_HAVE_CUDA
  const auto render_stream=reinterpret_cast<cudaStream_t>(device_info.command_queuePV);
#endif
  void* src=nullptr;
  void* dst=nullptr;
  void* empty_input=nullptr;
  void* full_gpu_output=nullptr;
  const auto geometry=fl::host_geometry(in->width,in->height,
      in->downsample_x.num,int(in->downsample_x.den),
      in->downsample_y.num,int(in->downsample_y.den),
      output->origin_x,output->origin_y,output->width,output->height);
  const int full_w=geometry.full_w,full_h=geometry.full_h;
  const bool tiled=output->width!=full_w||output->height!=full_h||
                   output->origin_x!=0||output->origin_y!=0;
  if(!geometry.valid()||full_w<1||full_h<1||full_w>16384||full_h>16384)
    err=PF_Err_BAD_CALLBACK_PARAM;
  // The host may supply either the clipped result_rect, or an output
  // world with padding beyond the optical sensor. Generate the full
  // optical sensor in CUDA, then copy only the actual intersection.
  // Explicit 64-bit endpoint arithmetic avoids signed origin overflow.
  const int64_t output_right=int64_t(output->origin_x)+output->width;
  const int64_t output_bottom=int64_t(output->origin_y)+output->height;
  const int copy_left=std::max(0,output->origin_x);
  const int copy_top=std::max(0,output->origin_y);
  const int copy_right=int(std::min(int64_t(full_w),output_right));
  const int copy_bottom=int(std::min(int64_t(full_h),output_bottom));
  const bool output_padded=output->origin_x<0||output->origin_y<0||
                         output_right>full_w||output_bottom>full_h;
  if(!err&&(output->width<=0||output->height<=0||
    output->rowbytes<int64_t(output->width)*16||
    copy_right<=copy_left||copy_bottom<=copy_top))
     err=PF_Err_BAD_CALLBACK_PARAM;
  if(!err)err=gs->GetGPUWorldData(in->effect_ref,output,&dst);
  if(!err&&input)err=gs->GetGPUWorldData(in->effect_ref,input,&src);
  // Empty transparent source may have no checkout world at all. In that
  // case synthesize a zero-BGRA128 device image for the manual emitter.
  // Flare/Ghost/Haze-only draw independent plates: only Composite needs
  // the source pixels for RGB carry-through and alpha determination.
  if(!err&&!input&&(args.output_mode==1||args.output_mode==3)){
   const size_t bytes=tiled?size_t(full_w)*full_h*16:
      size_t(output->rowbytes)*size_t(output->height);
   if(!bytes)err=PF_Err_BAD_CALLBACK_PARAM;
   if(!err)err=gs->AllocateDeviceMemory(in->effect_ref,extra->input->device_index,
                                       bytes,&empty_input);
#ifdef FLARELENS_HAVE_CUDA
   if(!err&&cudaMemsetAsync(empty_input,0,bytes,render_stream)!=cudaSuccess)
    err=PF_Err_INTERNAL_STRUCT_DAMAGED;
#else
   if(!err&&!fl::gpu::metal_zero(empty_input,bytes,device_info.command_queuePV))
    err=PF_Err_INTERNAL_STRUCT_DAMAGED;
#endif
   if(!err)src=empty_input;
  }
  if(!err&&dst&&(src||args.output_mode!=1)){
   if(input&&(!tiled?
      (input->width!=output->width||input->height!=output->height||
       input->origin_x!=output->origin_x||
       input->origin_y!=output->origin_y):
      (input->width!=full_w||input->height!=full_h||
       input->origin_x!=0||input->origin_y!=0)))
     err=PF_Err_BAD_CALLBACK_PARAM;
   const fl::Settings settings=optical_settings(args,in);
   // Even with a tiny AE output tile, detection and physical ray scattering
   // refer to the SAME complete optical sensor.
   const int w=full_w,h=full_h;
   const float lx=std::isfinite(args.light_x)?
      geometry.normalized_x(args.light_x):.5f;
   const float ly=std::isfinite(args.light_y)?
      geometry.normalized_y(args.light_y):.5f;
   float radiance=fl::nuke_equivalent_manual_radiance(
     args.manual_radiance,args.manual_diameter_px,
     geometry.downsample_x,geometry.downsample_y,
     args.scan_step,args.max_sources);
   if(args.fhd_stable_brightness)
     radiance*=fl::fhd_reference_emission_scale(w,h);
   std::vector<fl::Source> sources;
   if(args.auto_detect){
    // The CPU stage receives tile metadata only. Input RGB remains on the
    // host-managed CUDA world; source extraction and ghost rays share the
    // exact AE CUDA stream for correct MFR ordering.
    const bool scaled_preview=args.adaptive_preview&&
      ((in->downsample_x.num>0&&in->downsample_x.num<in->downsample_x.den)||
       (in->downsample_y.num>0&&in->downsample_y.num<in->downsample_y.den));
    const bool reduced=args.preview||scaled_preview;
    fl::gpu::DetectionSettings detect{};
    detect.width=w;detect.height=h;
    detect.input_pitch_bytes=input?input->rowbytes:0;
    detect.scan_step=std::clamp(reduced?args.preview_step:args.scan_step,1,32);
    detect.max_sources=std::clamp(reduced?args.preview_max:args.max_sources,1,2048);
    // Cluster Radius is a layer-pixel distance, so it scales with the world.
    detect.cluster_radius=float(args.cluster_radius)*
       std::sqrt(geometry.downsample_x*geometry.downsample_y);
    detect.source_threshold=args.threshold;
    detect.source_cap=args.source_cap;
    detect.tint_r=args.tint_r;detect.tint_g=args.tint_g;detect.tint_b=args.tint_b;
    detect.gate_alpha=args.source_alpha;
    if(input&&src){
     void* staging=nullptr;
#ifdef FLARELENS_HAVE_CUDA
     const size_t reduction_bytes=fl::gpu::detection_scratch_bytes(
        w,h,detect.scan_step);
#else
     const size_t reduction_bytes=fl::gpu::metal_detection_scratch_bytes(
        w,h,detect.scan_step);
#endif
     if(!reduction_bytes)err=PF_Err_BAD_CALLBACK_PARAM;
     if(!err)err=gs->AllocateDeviceMemory(in->effect_ref,
       extra->input->device_index,reduction_bytes,&staging);
     const auto detection_start=std::chrono::steady_clock::now();
#ifdef FLARELENS_HAVE_CUDA
     if(!err&&!fl::gpu::detect_sources_cuda(src,staging,reduction_bytes,
        device_info.command_queuePV,detect,sources))
       err=PF_Err_INTERNAL_STRUCT_DAMAGED;
#else
     if(!err&&!fl::gpu::detect_sources_metal(src,staging,reduction_bytes,
        device_info.command_queuePV,detect,sources))
       err=PF_Err_INTERNAL_STRUCT_DAMAGED;
#endif
     if(staging){
      const auto e2=gs->FreeDeviceMemory(in->effect_ref,
                             extra->input->device_index,staging);
      if(!err)err=e2;
     }
     if(args.host_trace){
      const double ms=std::chrono::duration<double,std::milli>(
       std::chrono::steady_clock::now()-detection_start).count();
      char report[220]{};
      std::snprintf(report,sizeof(report),
       "input=%dx%d step=%d scan_mb=%.2f sources=%zu ms=%.3f err=%ld",
       w,h,detect.scan_step,double(reduction_bytes)/1048576.,
       sources.size(),ms,long(err));
      fl::host_trace(true,"GPU_DETECT",report);
     }
    }
   }else{
    sources.push_back({lx,ly,radiance*args.tint_r,
                       radiance*args.tint_g,radiance*args.tint_b});
   }
   LensSystem local_custom;
 const LensSystem* lens=selected_lens(args,local_custom);
   if(!lens||lens->num_surfaces()<2||
      !std::isfinite(lens->focal_length)||lens->focal_length<=0){
    err=PF_Err_BAD_CALLBACK_PARAM;
    fl::host_trace(true,"LENS_NOT_RENDERABLE",
       "Missing original Nuke lens or fewer than two optical surfaces");
   }
   const bool need_ghost_blur=args.output_mode!=5&&
      args.ghost_blur>0&&args.ghost_blur_passes>0;
   const bool haze_active=(args.output_mode==1||args.output_mode==2||
       args.output_mode==5)&&args.haze_gain>0;
   const bool reduced_haze=args.preview||
     (args.adaptive_preview&&((in->downsample_x.num>0&&
      in->downsample_x.num<in->downsample_x.den)||
      (in->downsample_y.num>0&&
       in->downsample_y.num<in->downsample_y.den)));
   const int haze_footprint=args.auto_detect?
      std::clamp(reduced_haze?args.preview_step:args.scan_step,1,32):1;
   const bool dense_haze=haze_active&&
      (sources.size()>8||haze_footprint>1);
#ifdef FLARELENS_HAVE_CUDA
   const size_t scratch_bytes=(sources.empty()||!lens)?0:
      fl::gpu::scratch_bytes(*lens,int(sources.size()),settings.grid,w,h,
       args.output_mode,need_ghost_blur,dense_haze,
       haze_active&&!dense_haze);
#else
   const size_t scratch_bytes=(sources.empty()||!lens)?0:
      fl::gpu::metal_scratch_bytes(*lens,int(sources.size()),settings.grid,w,h,
       args.output_mode,need_ghost_blur,dense_haze,
       haze_active&&!dense_haze);
#endif
   void* scratch=nullptr;
   if(!err&&!sources.empty()&&!scratch_bytes)err=PF_Err_BAD_CALLBACK_PARAM;
   if(!err&&scratch_bytes)
    err=gs->AllocateDeviceMemory(in->effect_ref,extra->input->device_index,
                                 scratch_bytes,&scratch);
   const size_t fullbytes=size_t(full_w)*size_t(full_h)*16;
   if(!err&&tiled)
    err=gs->AllocateDeviceMemory(in->effect_ref,
               extra->input->device_index,fullbytes,&full_gpu_output);
   if(!err){
    const int input_pitch=input?input->rowbytes:
       tiled?full_w*16:output->rowbytes;
    fl::gpu::View view{src,tiled?full_gpu_output:dst,w,h,
                        input_pitch,tiled?full_w*16:output->rowbytes};
    view.output_mode=args.output_mode;
    view.ghost_blur=args.ghost_blur;
    view.ghost_blur_passes=args.ghost_blur_passes;
    view.haze_gain=args.haze_gain;
    view.haze_radius=args.haze_radius;
    view.haze_passes=args.haze_passes;
    view.haze_footprint=haze_footprint;
    view.command_stream=device_info.command_queuePV;
    view.check_abort=[](void* context)->int{
      return int(PF_ABORT(static_cast<PF_InData*>(context)));
    };
    view.abort_context=in;
    const auto start=std::chrono::steady_clock::now();
#ifdef FLARELENS_HAVE_CUDA
    const bool composed=fl::gpu::cuda_compose(*lens,sources,settings,view,
                                               scratch,scratch_bytes);
#else
    const bool composed=fl::gpu::metal_compose(*lens,sources,settings,view,
                                                scratch,scratch_bytes);
#endif
    if(!composed){
     const PF_Err cancel=PF_ABORT(in);
     err=cancel?cancel:PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    if(!err&&tiled){
     const auto* full=static_cast<const uint8_t*>(full_gpu_output);
     auto* clipped=static_cast<uint8_t*>(dst);
     const size_t source_offset=
       (size_t(copy_top)*size_t(full_w)+size_t(copy_left))*16;
     const size_t destination_offset=
       size_t(copy_top-output->origin_y)*size_t(output->rowbytes)+
       size_t(copy_left-output->origin_x)*16;
     // Pixels outside result_rect are transparent black, never a
     // partially uninitialized GPU World or an out-of-range memcpy.
#ifdef FLARELENS_HAVE_CUDA
     if(output_padded&&cudaMemset2DAsync(
          dst,size_t(output->rowbytes),0,
          size_t(output->width)*16,size_t(output->height),
          render_stream)!=cudaSuccess)
       err=PF_Err_INTERNAL_STRUCT_DAMAGED;
     if(!err&&cudaMemcpy2DAsync(clipped+destination_offset,
          size_t(output->rowbytes),full+source_offset,
          size_t(full_w)*16,size_t(copy_right-copy_left)*16,
          size_t(copy_bottom-copy_top),cudaMemcpyDeviceToDevice,
          render_stream)!=cudaSuccess)
       err=PF_Err_INTERNAL_STRUCT_DAMAGED;
     if(!err&&cudaStreamSynchronize(render_stream)!=cudaSuccess)
       err=PF_Err_INTERNAL_STRUCT_DAMAGED;
#else
     if(!fl::gpu::metal_copy_region(full_gpu_output,dst,
       device_info.command_queuePV,size_t(full_w)*16,
       size_t(output->rowbytes),copy_left,copy_top,copy_right,copy_bottom,
       output->origin_x,output->origin_y,output->width,output->height,
       output_padded))
       err=PF_Err_INTERNAL_STRUCT_DAMAGED;
#endif
    }
    if(args.host_trace){
     const auto elapsed=std::chrono::duration<double,std::milli>(
       std::chrono::steady_clock::now()-start).count();
     char facts[350]{};
     std::snprintf(facts,sizeof(facts),
       "gpu_ms=%.3f w=%d h=%d rays_grid=%d bands=%d lens=%d "
       "mode=%d blur=%g passes=%d haze=%g scratch_bytes=%zu err=%ld",
       elapsed,w,h,settings.grid,settings.spectral_samples,
       active_catalog_id(args),args.output_mode,double(args.ghost_blur),
       args.ghost_blur_passes,double(args.haze_gain),scratch_bytes,long(err));
     fl::host_trace(true,"GPU_DONE",facts);
    }
   }
   if(full_gpu_output){
    const PF_Err e2=gs->FreeDeviceMemory(in->effect_ref,
                   extra->input->device_index,full_gpu_output);
    if(!err)err=e2;
   }
   if(scratch){
    const PF_Err free_err=gs->FreeDeviceMemory(in->effect_ref,
                                             extra->input->device_index,scratch);
    if(!err)err=free_err;
   }
  }else if(!err)err=PF_Err_BAD_CALLBACK_PARAM;
  if(empty_input){
   const PF_Err free_err=gs->FreeDeviceMemory(in->effect_ref,
                                             extra->input->device_index,empty_input);
   if(!err)err=free_err;
  }
 }
 if(gs)AEFX_ReleaseSuite(in,out,kPFGPUDeviceSuite,kPFGPUDeviceSuiteVersion1,nullptr);
 const PF_Err checkin=extra->cb->checkin_layer_pixels(in->effect_ref,kInputCheckout);
 return err?err:checkin;
}
#endif
} // namespace

extern "C" FLARELENS_EXPORT
PF_Err EffectMain(PF_Cmd cmd,PF_InData* in,PF_OutData* out,
                  PF_ParamDef* params[],PF_LayerDef* output,void* extra){
 try{
  switch(cmd){
   case PF_Cmd_ABOUT:
    std::snprintf(out->return_msg,sizeof(out->return_msg),
      "FlareSimAE 1.0.5 / native CUDA/Metal SmartFX optical rendering");
    return PF_Err_NONE;
   case PF_Cmd_GLOBAL_SETUP:
    out->my_version=kVersion;
    // Emission is computed for the full frame and can appear outside both
    // the original content bounds and the input zero-alpha extent.
    out->out_flags=PF_OutFlag_DEEP_COLOR_AWARE|PF_OutFlag_I_EXPAND_BUFFER|
                   PF_OutFlag_CUSTOM_UI;
    out->out_flags2=PF_OutFlag2_FLOAT_COLOR_AWARE|
                    PF_OutFlag2_REVEALS_ZERO_ALPHA|
                    PF_OutFlag2_SUPPORTS_SMART_RENDER|
                    PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
#if defined(FLARELENS_HAVE_CUDA) || defined(FLARELENS_HAVE_METAL)
    // AE 25.6 header: this flag is required in GLOBAL_SETUP as well as device setup.
    out->out_flags2|=PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;
#endif
    return PF_Err_NONE;
   case PF_Cmd_PARAMS_SETUP:return params_setup(in,out);
   case PF_Cmd_USER_CHANGED_PARAM:{
    if(!params||!extra)return PF_Err_BAD_CALLBACK_PARAM;
    const auto* change=static_cast<const PF_UserChangedParamExtra*>(extra);
    if(change->param_index==P_CUSTOM_CLONE){
     const auto* source=fl::catalog::lens_by_index(params[P_CATALOG_INDEX]->u.sd.value);
     if(!source||source->num_surfaces()<2||source->num_surfaces()>fl::custom::kMaxSurfaces)
      return PF_Err_BAD_CALLBACK_PARAM;
     const auto doc=fl::custom::from_lens(*source);
     if(!fl::custom::valid(doc)||
        !fl::custom::ae::store(in,params[P_CUSTOM_LENS]->u.arb_d.value,doc))
      return PF_Err_BAD_CALLBACK_PARAM;
     params[P_CUSTOM_LENS]->uu.change_flags|=PF_ChangeFlag_CHANGED_VALUE;
     params[P_LENS_SOURCE]->u.pd.value=2;
     params[P_LENS_SOURCE]->uu.change_flags|=PF_ChangeFlag_CHANGED_VALUE;
     out->out_flags|=PF_OutFlag_REFRESH_UI|PF_OutFlag_FORCE_RERENDER;
    }
    if(change->param_index==P_CATALOG_BROWSE){
     int chosen=params[P_CATALOG_INDEX]->u.sd.value;
     // Modal browser is opened ONLY by an explicit user click on the
     // native AE parameter button (never by rendering, MFR or startup).
     if(fl::catalog::choose_embedded_lens(
#if defined(_WIN32)
      GetActiveWindow(),
#else
      nullptr,
#endif
      chosen,chosen)){
      params[P_CATALOG_INDEX]->u.sd.value=chosen;
      params[P_CATALOG_INDEX]->uu.change_flags=PF_ChangeFlag_CHANGED_VALUE;
      params[P_LENS_SOURCE]->u.pd.value=1;
      params[P_LENS_SOURCE]->uu.change_flags=PF_ChangeFlag_CHANGED_VALUE;
      out->out_flags|=PF_OutFlag_REFRESH_UI|PF_OutFlag_FORCE_RERENDER;
     }
    }
    return PF_Err_NONE;
   }
   case PF_Cmd_EVENT:return fl::custom::ae::event(in,out,params,P_CUSTOM_LENS,P_LENS_SOURCE,
                                                    static_cast<PF_EventExtra*>(extra));
   case PF_Cmd_ARBITRARY_CALLBACK:return fl::custom::ae::arbitrary(in,out,
                                             static_cast<PF_ArbParamsExtra*>(extra));
   case PF_Cmd_RENDER:return render_worlds(in,out,&params[P_INPUT]->u.ld,output,from_legacy(in,params));
   case PF_Cmd_SMART_PRE_RENDER:return pre_render(in,(PF_PreRenderExtra*)extra);
   case PF_Cmd_SMART_RENDER:return smart_render(in,out,(PF_SmartRenderExtra*)extra);
#if defined(FLARELENS_HAVE_CUDA) || defined(FLARELENS_HAVE_METAL)
   case PF_Cmd_GPU_DEVICE_SETUP:
    if(extra&&((PF_GPUDeviceSetupExtra*)extra)->input->what_gpu==kNativeGpuFramework)
     out->out_flags2|=PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;
    return PF_Err_NONE;
   case PF_Cmd_GPU_DEVICE_SETDOWN:return PF_Err_NONE;
   case PF_Cmd_SMART_RENDER_GPU:return smart_render_gpu(in,out,(PF_SmartRenderExtra*)extra);
#endif
   default:return PF_Err_NONE;
  }
 }catch(const std::bad_alloc&){return PF_Err_OUT_OF_MEMORY;}
 catch(...){return PF_Err_INTERNAL_STRUCT_DAMAGED;}
}
