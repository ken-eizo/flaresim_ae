#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <cstdint>
#include <cstring>
#include "AE_Effect.h"
using Main = PF_Err (*)(PF_Cmd,PF_InData*,PF_OutData*,PF_ParamDef**,PF_LayerDef*,void*);
int main(){
 wchar_t buf[32768]{};
 if(!GetModuleFileNameW(nullptr,buf,32768))return 1;
 auto dll=std::filesystem::path(buf).parent_path()/L"FlareLensAE.aex";
 const HMODULE m=LoadLibraryW(dll.c_str());
 if(!m){std::wcerr<<L"LoadLibrary failed "<<GetLastError()<<L" at "<<dll<<L"\n";return 2;}
 auto f=reinterpret_cast<Main>(GetProcAddress(m,"EffectMain"));
 if(!f){std::cerr<<"EffectMain is not exported\n";return 3;}
 const auto r=FindResourceW(m,MAKEINTRESOURCEW(16000),L"PiPL");
 if(!r||!SizeofResource(m,r)){std::cerr<<"No valid PiPL resource\n";return 4;}
 PF_InData in{};PF_OutData out{};
 if(f(PF_Cmd_GLOBAL_SETUP,&in,&out,nullptr,nullptr,nullptr)!=PF_Err_NONE)return 5;
 PF_OutData about{};
 if(f(PF_Cmd_ABOUT,&in,&about,nullptr,nullptr,nullptr)!=PF_Err_NONE||
    std::strstr(about.return_msg,"1.0.4")==nullptr){
  std::cerr<<"Build revision unavailable via AE About selector\n";
  return 14;
 }
 const auto required=PF_OutFlag2_FLOAT_COLOR_AWARE|
                     PF_OutFlag2_SUPPORTS_SMART_RENDER|
                     PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
 if((out.out_flags2&required)!=required){std::cerr<<"Inconsistent SmartFX flags\n";return 6;}
 // Emission from an empty Shape Layer MUST work: zero-alpha pixels may
 // reveal light and the output may expand beyond the input content bbox.
 if(!(out.out_flags&PF_OutFlag_CUSTOM_UI)||
    !(out.out_flags&PF_OutFlag_I_EXPAND_BUFFER)||
    !(out.out_flags2&PF_OutFlag2_REVEALS_ZERO_ALPHA)){
  std::cerr<<"AE output flags missing transparent emitter support\n";
  return 13;
 }
 const auto resource=LoadResource(m,r);
 const auto* raw=static_cast<const unsigned char*>(LockResource(resource));
 const auto total=SizeofResource(m,r);
 bool version_seen=false;
 for(size_t i=0;raw&&i+16<=total;i++){
  uint32_t tag=0,len=0,version=0;
  std::memcpy(&tag,raw+i,4);
  if(tag!=0x65564552u)continue;
  std::memcpy(&len,raw+i+8,4);
  std::memcpy(&version,raw+i+12,4);
  if(len==4){
   version_seen=true;
   std::cout<<"PiPL version="<<version<<" Code version="<<out.my_version<<"\n";
   if(version!=out.my_version){std::cerr<<"PiPL/code version mismatch\n";return 9;}
  }
 }
 if(!version_seen){std::cerr<<"PiPL version property missing\n";return 10;}
 // Adobe validates both the PiPL and runtime flags; exercise that contract
 // before the user launches AE, not after showing a host verification dialog.
 auto pip_value=[&](uint32_t key,uint32_t& value)->bool{
  for(size_t j=0;raw&&j+16<=total;j++){
   uint32_t tag=0,len=0;
   std::memcpy(&tag,raw+j,4);
   if(tag!=key)continue;
   std::memcpy(&len,raw+j+8,4);
   if(len!=4)continue;
   std::memcpy(&value,raw+j+12,4);
   return true;
  }
  return false;
 };
 uint32_t flags=0,flags2=0;
 if(!pip_value(0x65474c4fu,flags)||!pip_value(0x65474c32u,flags2)){
  std::cerr<<"PiPL out_flag property missing\n";return 11;
 }
 if(flags!=uint32_t(out.out_flags)||flags2!=uint32_t(out.out_flags2)){
  std::cerr<<"PiPL/code flag mismatch: "
           <<flags<<"/"<<out.out_flags<<" and "<<flags2<<"/"<<out.out_flags2<<"\n";
  return 12;
 }
#ifdef FLARELENS_HAVE_CUDA
 if(!(out.out_flags2&PF_OutFlag2_SUPPORTS_GPU_RENDER_F32)){
  std::cerr<<"GPU global flag missing\n";return 7;
 }
 PF_GPUDeviceSetupInput di{};di.what_gpu=PF_GPU_Framework_CUDA;
 PF_GPUDeviceSetupExtra de{};de.input=&di;
 PF_OutData gpu{};
 if(f(PF_Cmd_GPU_DEVICE_SETUP,&in,&gpu,nullptr,nullptr,&de)!=PF_Err_NONE ||
    !(gpu.out_flags2&PF_OutFlag2_SUPPORTS_GPU_RENDER_F32))return 8;
#endif
 std::cout<<"PiPL present, entry exported, GlobalSetup flags="<<out.out_flags2
          <<", version="<<out.my_version<<"\n";
 FreeLibrary(m);
 return 0;
}
