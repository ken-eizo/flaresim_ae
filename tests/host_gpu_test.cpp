// CUDA-enabled real AEX entry-point integration regression.
// IMPORTANT: This does NOT launch AE or invoke its GUI. It emulates the
// Adobe SDK callback contracts and executes the production AEX's
// PF_Cmd_GPU_DEVICE_SETUP -> SMART_PRE_RENDER -> SMART_RENDER_GPU selectors.
// Tests include a null input Shape Layer, a REAL nondefault host CUDA stream,
// host-owned BGRA128 device output, Adobe GPUDeviceSuite memory allocation,
// Nuke-comparable RGB and actual emitted alpha.
#define NOMINMAX
#include <windows.h>
#include "AE_Effect.h"
#include "AE_EffectCBSuites.h"
#include "AE_EffectGPUSuites.h"
#include "AE_Macros.h"
#include "mock_custom_ui_host.hpp"
#include "SPBasic.h"
#include "FlareLensCore.hpp"
#include "FlareLensCatalogGenerated.hpp"
#include "FlareLensOptics.hpp"
#include "FlareLensCustom.hpp"
#include "FlareLensComposite.hpp"
#include <cuda_runtime.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <vector>

using Main=PF_Err(*)(PF_Cmd,PF_InData*,PF_OutData*,PF_ParamDef**,PF_LayerDef*,void*);
static PF_WorldSuite2 world_suite{};
static PF_GPUDeviceSuite1 gpu_suite{};
static cudaStream_t host_stream=nullptr;
static PF_EffectWorld* gpu_output_world=nullptr;
static PF_EffectWorld* gpu_input_world=nullptr;
static std::vector<PF_ParamDef> params;
static int allocations=0,free_calls=0,checkouts=0,checkins=0;
static size_t last_allocation=0,max_allocation=0;
static int default_nikkor_id=0;
static PF_Err abort_ok(PF_ProgPtr){return PF_Err_NONE;}
static PF_Err collect(PF_ProgPtr,PF_ParamIndex idx,PF_ParamDefPtr p){
 if(idx!=-1||!p)return PF_Err_BAD_CALLBACK_PARAM;
 params.push_back(*p);fl_mock::activate(params.back());return PF_Err_NONE;
}
static PF_Err gpu_pixel_format(const PF_EffectWorld*,PF_PixelFormat* value){
 if(!value)return PF_Err_BAD_CALLBACK_PARAM;
 *value=PF_PixelFormat_GPU_BGRA128;return PF_Err_NONE;
}
static PF_Err SPAPI device_info(PF_ProgPtr,A_u_long device,PF_GPUDeviceInfo* value){
 if(device!=0||!value)return PF_Err_BAD_CALLBACK_PARAM;
 *value={};
 value->device_framework=PF_GPU_Framework_CUDA;
 value->compatibleB=TRUE;
 value->command_queuePV=reinterpret_cast<void*>(host_stream);
 return PF_Err_NONE;
}
static PF_Err SPAPI alloc_device(PF_ProgPtr,A_u_long device,
                                 size_t size,void** ptr){
 if(device!=0||!size||!ptr)return PF_Err_BAD_CALLBACK_PARAM;
 *ptr=nullptr;
 if(cudaMalloc(ptr,size)!=cudaSuccess)return PF_Err_OUT_OF_MEMORY;
 allocations++;last_allocation=size;max_allocation=std::max(max_allocation,size);
 return PF_Err_NONE;
}
static PF_Err SPAPI free_device(PF_ProgPtr,A_u_long device,void* ptr){
 if(device!=0||!ptr)return PF_Err_BAD_CALLBACK_PARAM;
 if(cudaFree(ptr)!=cudaSuccess)return PF_Err_BAD_CALLBACK_PARAM;
 free_calls++;return PF_Err_NONE;
}
static PF_Err SPAPI gpu_world_data(PF_ProgPtr,PF_EffectWorld* world,void** ptr){
 if(!world||!ptr)return PF_Err_BAD_CALLBACK_PARAM;
 *ptr=world->data;return PF_Err_NONE;
}
static SPErr SPAPI acquire(const char* name,int32 version,const void** suite){
 if(!name||!suite)return 1;
 if(std::strcmp(name,kPFWorldSuite)==0&&version==kPFWorldSuiteVersion2){
  *suite=&world_suite;return 0;
 }
 if(std::strcmp(name,kPFGPUDeviceSuite)==0&&
    version==kPFGPUDeviceSuiteVersion1){
  *suite=&gpu_suite;return 0;
 }
 return 1;
}
static SPErr SPAPI release(const char*,int32){return 0;}
static PF_Err checkout_param(PF_ProgPtr,PF_ParamIndex index,A_long,A_long,
                              A_u_long,PF_ParamDef* value){
 if(!value||index<1||index>int(params.size()))return PF_Err_INVALID_INDEX;
 *value=params[size_t(index)-1];return PF_Err_NONE;
}
static PF_Err checkin_param(PF_ProgPtr,PF_ParamDef*){return PF_Err_NONE;}
static PF_Err checkout_layer(PF_ProgPtr,PF_ParamIndex,A_long,
                             const PF_RenderRequest* request,
                             A_long,A_long,A_u_long,PF_CheckoutResult* result){
 if(!request||!result)return PF_Err_BAD_CALLBACK_PARAM;
 *result={};result->result_rect=request->rect;
 result->max_result_rect=request->rect;return PF_Err_NONE;
}
static PF_Err checkout_null_shape(PF_ProgPtr,A_long,PF_EffectWorld** value){
 if(!value)return PF_Err_BAD_CALLBACK_PARAM;
 *value=gpu_input_world;checkouts++;return PF_Err_NONE;
}
static PF_Err checkin_null_shape(PF_ProgPtr,A_long){
 checkins++;return PF_Err_NONE;
}
static PF_Err checkout_gpu_output(PF_ProgPtr,PF_EffectWorld** value){
 if(!value)return PF_Err_BAD_CALLBACK_PARAM;
 *value=gpu_output_world;return PF_Err_NONE;
}
static int one_render(Main effect,PF_InData& in,int mode,
                      int width,int height,const LensSystem& lens,bool adaptive=false,
                      bool detect=false,bool tile=false,int many=0,
                      int quick_preset=1,int catalog_id=0,bool custom=false){
 const size_t n=size_t(width)*height;
 const size_t bytes=n*16;
 const int roi_x=tile?width/4:0,roi_y=tile?height/4:0;
 const int roi_w=tile?width/2:width,roi_h=tile?height/2:height;
 const size_t output_bytes=size_t(roi_w)*roi_h*16;
 void* device=nullptr;
 if(cudaMalloc(&device,output_bytes)!=cudaSuccess)return 20;
 if(cudaMemsetAsync(device,0xCD,output_bytes,host_stream)!=cudaSuccess)return 21;
 PF_EffectWorld world{};
 world.width=roi_w;world.height=roi_h;world.rowbytes=roi_w*16;
 world.origin_x=roi_x;world.origin_y=roi_y;
 world.data=reinterpret_cast<PF_Pixel*>(device);
 gpu_output_world=&world;
 void* input_device=nullptr;
 PF_EffectWorld image_input{};
 std::vector<float4> image_cpu;
 int block_x=0,block_y=0;
 if(detect){
  block_x=int((width*.72f)/4)*4;
  block_y=int((height*.25f)/4)*4;
  image_cpu.assign(n,make_float4(0,0,0,1));
  if(many>0){
   // Uniform scene-linear HDR blocks distributed across the full sensor.
   // Their 4x4 tile layout is exactly the original Nuke source detector's
   // sampling grid; checking 512 emitters exercises the actual AEX GPU path.
   for(int i=0;i<many;i++){
    const int xx=4*((i*13)%(width/4));
    const int yy=4*((i*17)%(height/4));
    for(int y=yy;y<yy+4;y++)for(int x=xx;x<xx+4;x++)
     image_cpu[size_t(y)*width+x]=make_float4(60,80,100,1);
   }
  }else{
   for(int y=block_y;y<block_y+4;y++)
    for(int x=block_x;x<block_x+4;x++)
     image_cpu[size_t(y)*width+x]=make_float4(60,80,100,1);
  }
  if(cudaMalloc(&input_device,bytes)!=cudaSuccess)return 31;
  if(cudaMemcpyAsync(input_device,image_cpu.data(),bytes,
       cudaMemcpyHostToDevice,host_stream)!=cudaSuccess)return 32;
  image_input.width=width;image_input.height=height;
  image_input.rowbytes=width*16;
  image_input.data=reinterpret_cast<PF_Pixel*>(input_device);
  gpu_input_world=&image_input;
 }
 in.width=width*(adaptive?2:1);in.height=height*(adaptive?2:1);
 in.downsample_x.num=1;in.downsample_x.den=adaptive?2:1;
 in.downsample_y.num=1;in.downsample_y.den=adaptive?2:1;
 params[0].u.td.x_value=FLOAT2FIX(in.width*.72f);
 params[0].u.td.y_value=FLOAT2FIX(in.height*.25f);
 params[2].u.sd.value=many>0?12:64; // 512 multi-source GPU integration
 params[5].u.bd.value=detect?TRUE:FALSE; // actual Nuke HDR detector
 params[8].u.pd.value=quick_preset; // original 12 AE preset indices
 params[58].u.pd.value=custom?2:1; // custom or complete catalog
 params[59].u.sd.value=catalog_id?catalog_id:default_nikkor_id;
 params[10].u.pd.value=mode;
 params[13].u.sd.value=6; // Six blades
 params[15].u.pd.value=many>0?1:3; // 3 RGB for many-source stress
 params[16].u.pd.value=3; // Halton
 params[26].u.fs_d.value=mode==2?0.:.003;
 params[27].u.sd.value=3;
 params[28].u.fs_d.value=(mode==1||mode==5)?1.:0.;
 params[31].u.fs_d.value=0.; // diffraction GPU-unsupported
 params[54].u.fs_d.value=30000.;
 params[57].u.fs_d.value=0.; // regression for legacy point light
 params[55].u.bd.value=FALSE; // No trace log from mock host
 PF_PreRenderCallbacks pre_callbacks{};
 pre_callbacks.checkout_layer=&checkout_layer;
 PF_PreRenderInput pre_input{};
 pre_input.what_gpu=PF_GPU_Framework_CUDA;
 pre_input.device_index=0;
 pre_input.output_request.rect={roi_x,roi_y,roi_x+roi_w,roi_y+roi_h};
 pre_input.output_request.channel_mask=PF_ChannelMask_ARGB;
 PF_PreRenderOutput pre_output{};
 PF_PreRenderExtra pre_extra{&pre_input,&pre_output,&pre_callbacks};
 PF_OutData out{};
 PF_Err err=effect(PF_Cmd_SMART_PRE_RENDER,&in,&out,nullptr,nullptr,&pre_extra);
 if(err||!(pre_output.flags&PF_RenderOutputFlag_GPU_RENDER_POSSIBLE)||
    !pre_output.pre_render_data){
  std::cerr<<"AEX did not authorize GPU for mode "<<mode<<"\n";
  return 22;
 }
 PF_SmartRenderCallbacks render_callbacks{};
 render_callbacks.checkout_layer_pixels=&checkout_null_shape;
 render_callbacks.checkin_layer_pixels=&checkin_null_shape;
 render_callbacks.checkout_output=&checkout_gpu_output;
 PF_SmartRenderInput render_input{};
 render_input.what_gpu=PF_GPU_Framework_CUDA;
 render_input.device_index=0;
 render_input.pre_render_data=pre_output.pre_render_data;
 render_input.bitdepth=32;
 PF_SmartRenderExtra render_extra{&render_input,&render_callbacks};
 allocations=0;free_calls=0;max_allocation=0;last_allocation=0;
 const auto start=std::chrono::steady_clock::now();
 err=effect(PF_Cmd_SMART_RENDER_GPU,&in,&out,nullptr,nullptr,&render_extra);
 const double elapsed=std::chrono::duration<double,std::milli>(
     std::chrono::steady_clock::now()-start).count();
 if(pre_output.delete_pre_render_data_func)
  pre_output.delete_pre_render_data_func(pre_output.pre_render_data);
 if(err||allocations!=free_calls||checkouts!=checkins){
  std::cerr<<"Actual AEX GPU entry failure mode "<<mode
           <<" err="<<err<<" allocations="<<allocations
           <<" frees="<<free_calls<<" checkout="<<checkouts
           <<" checkin="<<checkins<<"\n";
  return 23;
 }
 std::vector<float> pixels(size_t(roi_w)*roi_h*4);
 if(cudaMemcpy2DAsync(pixels.data(),size_t(roi_w)*16,device,
     size_t(roi_w)*16,size_t(roi_w)*16,roi_h,cudaMemcpyDeviceToHost,host_stream)
     !=cudaSuccess)return 24;
 if(cudaStreamSynchronize(host_stream)!=cudaSuccess)return 25;
 cudaFree(device);
 if(input_device)cudaFree(input_device);
 gpu_input_world=nullptr;
 fl::Settings settings;
 settings.grid=adaptive||many>0?12:64;
 settings.spectral_samples=adaptive||many>0?3:7;
 settings.aperture_blades=6;
 settings.pupil_jitter=2;settings.gain=1000;settings.max_pairs=2048;
 // The native AE effect uses ALL active lens pairs, unlike the generic
 // Core Settings default max_pairs=40 used for tiny standalone tests.
 // Match the host's quantized 16.16 Point Control exactly.
 const float sx=FIX_2_FLOAT(params[0].u.td.x_value)/float(in.width);
 const float sy=FIX_2_FLOAT(params[0].u.td.y_value)/float(in.height);
 const std::vector<fl::Source> lights{detect?
  fl::Source{float(block_x+2)/width,float(block_y+2)/height,100,80,60}:
  fl::Source{sx,sy,30000,30000,30000}};
 std::vector<float> expected(n*3,0);
 if(mode!=5&&mode!=3&&many==0){
  expected=fl::render_cpu(lens,lights,settings,width,height).rgb;
  if(expected.size()!=n*3)return 26;
  if(mode!=2){
   const int radius=int(std::round(.003f*std::hypot(float(width),float(height))));
   fl::box_blur_rgb(expected,width,height,radius,3);
  }
 }
 if(mode==3){
  // CPU Sources Preview uses the input RGB at 12% and an opaque cross.
  // Check both manually placed emitters and HDR GPU auto detection.
  if(detect){
   for(size_t i=0;i<n;i++){
    expected[i*3]=image_cpu[i].z*.12f;
    expected[i*3+1]=image_cpu[i].y*.12f;
    expected[i*3+2]=image_cpu[i].x*.12f;
   }
  }
  const int cx=detect?block_x+2:int(std::floor(sx*width));
  const int cy=detect?block_y+2:int(std::floor(sy*height));
  for(int dy=-5;dy<=5;dy++)for(int dx=-5;dx<=5;dx++){
   const int x=cx+dx,y=cy+dy,r2=dx*dx+dy*dy;
   if(x<0||x>=width||y<0||y>=height)continue;
   if(!((r2>=9&&r2<=25)||(dx==0&&r2<=25)||
        (dy==0&&r2<=25)))continue;
   const auto i=(size_t(y)*width+x)*3;
   expected[i]+=1.f;expected[i+1]+=1.f;expected[i+2]+=1.f;
  }
 }
 if((mode==1||mode==5)&&many==0){
  std::vector<float> haze;
  fl::render_haze(haze,lights,width,height,1.f,.15f,3,1);
  if(haze.size()!=n*3)return 27;
  for(size_t i=0;i<expected.size();i++)expected[i]+=haze[i];
 }
 double sum=0,difference=0;
 size_t visible=0,alpha_errors=0;
 for(size_t i=0;i<size_t(roi_w)*roi_h;i++){
  const size_t frame_index=(size_t(roi_y)+i/roi_w)*width+
                            size_t(roi_x)+i%roi_w;
  const float b=pixels[i*4],g=pixels[i*4+1],r=pixels[i*4+2],a=pixels[i*4+3];
  if(!std::isfinite(r)||!std::isfinite(g)||!std::isfinite(b)||
     !std::isfinite(a))return 28;
  const float want_alpha=mode==1?
   fl::alpha_for_emission(0,r,g,b):1.f;
  if(std::fabs(a-want_alpha)>1e-4)alpha_errors++;
  if(std::max({r,g,b})>.5f/255.f&&a>.5f/255.f)visible++;
  const float channels[]={r,g,b};
  for(int c=0;c<3;c++){
   if(many==0){
    const float target=expected[frame_index*3+c];
    difference+=std::fabs(double(channels[c])-target);
    sum+=std::fabs(double(target));
   }else sum+=std::fabs(double(channels[c]));
  }
 }
 const double rel=difference/std::max(sum,1e-10);
 std::cout<<"ACTUAL_AEX_GPU_ENTRY mode="<<mode<<" "<<width<<"x"<<height
          <<" adaptive="<<adaptive<<" detect="<<detect<<" tile="<<tile
          <<" HDR_source_blocks="<<many
          <<" quick="<<quick_preset<<" catalog_id="<<catalog_id
          <<" wall_ms="<<elapsed<<" relative_L1="<<rel
          <<" visible_rgba="<<visible<<" alpha_errors="<<alpha_errors
          <<" gpu_allocations="<<allocations<<" gpu_frees="<<free_calls
          <<" max_scratch_MB="<<double(max_allocation)/1048576<<"\n";
 if(visible<32||alpha_errors||rel>.02||
    (many>0&&elapsed>5000.))return 29;
 // Production AEX must request the tight scratch size, not the old
 // conservative 2-frame allocation used by standalone compatibility API.
 // A tiled request intentionally allocates a full-sensor CUDA output
 // before cropping; the old scratch-memory invariant applies to full frames.
 if(!tile&&!detect&&(mode==2||mode==5)&&
    max_allocation>=size_t(width)*height*4){
  std::cerr<<"AEX allocated unnecessary full-frame CUDA scratch mode="
           <<mode<<" bytes="<<max_allocation<<"\n";
  return 30;
 }
 return 0;
}
int main(){
 std::setvbuf(stdout,nullptr,_IONBF,0);
 int devices=0;
 if(cudaGetDeviceCount(&devices)!=cudaSuccess||!devices)return 77;
 if(cudaStreamCreateWithFlags(&host_stream,cudaStreamNonBlocking)!=cudaSuccess)
  return 1;
 const auto parent=std::filesystem::path(__FILE__).parent_path().parent_path();
 LensSystem lens;
 if(!lens.load((parent/"upstream/lenses/lens_files/AI_Nikkor_50mm_F1.2S.lens")
                .string().c_str()))return 2;
 for(int i=0;i<fl::catalog::kCount;i++)
  if(std::strcmp(fl::catalog::kNames[size_t(i)],
                  "AI_Nikkor_50mm_F1.2S.lens")==0)default_nikkor_id=i+1;
 if(!default_nikkor_id)return 16;
 wchar_t filename[MAX_PATH]{};
 if(!GetModuleFileNameW(nullptr,filename,MAX_PATH))return 3;
 const auto path=std::filesystem::path(filename).parent_path()/L"FlareLensAE.aex";
 const HMODULE dll=LoadLibraryW(path.c_str());
 if(!dll)return 4;
 const auto effect=reinterpret_cast<Main>(GetProcAddress(dll,"EffectMain"));
 if(!effect)return 5;
 world_suite.PF_GetPixelFormat=&gpu_pixel_format;
 gpu_suite.GetDeviceInfo=&device_info;
 gpu_suite.AllocateDeviceMemory=&alloc_device;
 gpu_suite.FreeDeviceMemory=&free_device;
 gpu_suite.GetGPUWorldData=&gpu_world_data;
 SPBasicSuite basic{};
 basic.AcquireSuite=&acquire;basic.ReleaseSuite=&release;
 PF_InData in{};
 fl_mock::configure(in);
 in.pica_basicP=&basic;
 in.inter.add_param=&collect;
 in.inter.abort=&abort_ok;
 in.inter.checkout_param=&checkout_param;
 in.inter.checkin_param=&checkin_param;
 PF_OutData out{};
 if(effect(PF_Cmd_PARAMS_SETUP,&in,&out,nullptr,nullptr,nullptr)!=PF_Err_NONE||
    params.size()!=63)return 6;
 PF_GPUDeviceSetupInput setup_input{PF_GPU_Framework_CUDA,0};
 PF_GPUDeviceSetupOutput setup_output{};
 PF_GPUDeviceSetupExtra setup_extra{&setup_input,&setup_output};
 if(effect(PF_Cmd_GPU_DEVICE_SETUP,&in,&out,nullptr,nullptr,&setup_extra)
    !=PF_Err_NONE)return 7;
 for(int mode:{1,2,3,4,5}){
  const int err=one_render(effect,in,mode,640,360,lens);
  if(err)return err+100*mode;
 }
 // Regression for first-application lag: a manual source must retain CUDA
 // when the AE host asks for a tile instead of the whole composition.
 for(int mode:{1,2,3,4,5}){
  const int err=one_render(effect,in,mode,640,360,lens,
                           false,false,true);
  if(err)return 5000+100*mode+err;
 }
 std::cout<<"MANUAL_LIGHT_CUDA_TILED_ROI_CPU_REFERENCE_PARITY_PASSED\n";
 if(const int err=one_render(effect,in,1,640,360,lens,true))
  return 700+err;
 if(const int err=one_render(effect,in,2,640,360,lens,false,true))
  return 900+err;
 if(const int err=one_render(effect,in,3,640,360,lens,false,true))
  return 1000+err;
 if(const int err=one_render(effect,in,3,640,360,lens,false,true,true))
  return 1050+err;
 if(const int err=one_render(effect,in,2,640,360,lens,false,true,true))
  return 1100+err;
 if(const int err=one_render(effect,in,2,640,360,lens,false,true,false,512))
  return 1300+err;
 if(const int err=one_render(effect,in,2,640,360,lens,false,true,true,512))
  return 1500+err;
 // The actual compiled AEX MUST render Canon's prescription, not the
 // original Nikkor 50mm when a different (embedded) lens is selected.
 const char* canon_file="Canon_51mm_F1.25_(Canon_RF_50mm_F1.2_L_USM).lens";
 int canon_id=0;
 for(int i=0;i<fl::catalog::kCount;i++)
  if(std::strcmp(fl::catalog::kNames[size_t(i)],canon_file)==0)
   canon_id=i+1;
 if(!canon_id)return 9;
 LensSystem canon;
 if(!canon.load((parent/"upstream/lenses/lens_files"/canon_file)
                 .string().c_str()))return 10;
 if(const int err=one_render(effect,in,2,640,360,canon,
                             false,false,false,0,1,canon_id))
  return 1900+err; // Full 1370-file catalog resolves the same Canon
 // Additional optical prescriptions must ALSO change actual CUDA pixels,
 // not just their catalog names or geometry diagnostics. Each invocation
 // compares the AEX's GPU image with independently loaded Nuke CPU optics.
 const char* fuji_file=
  "Fujifilm_36mm_F1.45_(Fujifilm_Fujinon_XF35mm_F1.4_R).lens";
 int fuji_id=0;
 for(int i=0;i<fl::catalog::kCount;i++)
  if(std::strcmp(fl::catalog::kNames[size_t(i)],fuji_file)==0)
   fuji_id=i+1;
 if(!fuji_id)return 11;
 LensSystem fuji;
 if(!fuji.load((parent/"upstream/lenses/lens_files"/fuji_file)
                 .string().c_str()))return 12;
 if(const int err=one_render(effect,in,2,640,360,fuji,
                             false,false,false,0,1,fuji_id))
  return 2300+err;
 // Boundaries matter: the 1,370th entry must be addressable by the
 // actual GPU renderer, with no accidental clamping to an old preset.
 const char* last_file=fl::catalog::kNames[fl::catalog::kCount-1];
 LensSystem last_lens;
 if(!last_lens.load((parent/"upstream/lenses/lens_files"/last_file)
                      .string().c_str()))return 13;
 if(last_lens.num_surfaces()>=2&&std::isfinite(last_lens.focal_length)&&
    last_lens.focal_length>0){
  if(const int err=one_render(effect,in,2,640,360,last_lens,
                     false,false,false,0,1,fl::catalog::kCount))
   return 2500+err;
 }
 // Host GPU selector must receive the exact serialized custom prescription.
 auto* doc=static_cast<fl::custom::Document*>(
      fl_mock::lock(params[61].u.arb_d.value));
 if(!doc||!fl::custom::valid(*doc))return 14;
 LensSystem custom_nikkor=fl::custom::make_lens(*doc);
 if(const int err=one_render(effect,in,2,640,360,custom_nikkor,
                            false,false,false,0,1,0,true))return 2700+err;
 const auto backup=*doc;
 if(!fl::custom::change_aperture(*doc,0,doc->elements[0].semi*.72f))
  return 15;
 LensSystem edited=fl::custom::make_lens(*doc);
 if(const int err=one_render(effect,in,2,640,360,edited,
                            false,false,false,0,1,0,true))return 2900+err;
 *doc=backup;
 std::cout<<"ACTUAL_AEX_CUSTOM_LENS_CUDA_REFERENCE_PARITY_PASSED\n";
 PF_GPUDeviceSetdownInput sd_input{};
 sd_input.what_gpu=PF_GPU_Framework_CUDA;
 sd_input.device_index=0;
 PF_GPUDeviceSetdownExtra sd_extra{&sd_input};
 if(effect(PF_Cmd_GPU_DEVICE_SETDOWN,&in,&out,nullptr,nullptr,&sd_extra)
    !=PF_Err_NONE)return 8;
 FreeLibrary(dll);
 cudaStreamDestroy(host_stream);
 std::cout<<"ACTUAL_AEX_GPU_ENTRYPOINT_MOCK_HOST_PASSED\n";
 return 0;
}
