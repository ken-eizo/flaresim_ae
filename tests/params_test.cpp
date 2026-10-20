#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>
#include "AE_Effect.h"
#include "AE_Macros.h"
#include "mock_custom_ui_host.hpp"
#include "../src/FlareLensCustom.hpp"
#include "FlareLensCatalog.hpp"
#include <cstring>
// Older After Effects SDK headers expose these members directly.
#ifndef PF_DEF_NAME
#define PF_DEF_NAME name
#endif
#ifndef PF_DEF_NAMESPTR
#define PF_DEF_NAMESPTR namesptr
#endif
using Main=PF_Err(*)(PF_Cmd,PF_InData*,PF_OutData*,PF_ParamDef**,PF_LayerDef*,void*);
static std::vector<PF_ParamDef> capture;
static PF_Err collect(PF_ProgPtr,PF_ParamIndex idx,PF_ParamDefPtr p){
 if(idx!=-1||!p)return PF_Err_BAD_CALLBACK_PARAM;
 capture.push_back(*p);fl_mock::activate(capture.back());
 return PF_Err_NONE;
}
int main(){
 wchar_t exe[32768]{};
 if(!GetModuleFileNameW(nullptr,exe,32768))return 1;
 const auto dll=std::filesystem::path(exe).parent_path()/L"FlareLensAE.aex";
 HMODULE module=LoadLibraryW(dll.c_str());
 if(!module){std::cerr<<"Cannot load plugin\n";return 2;}
 const auto effect=reinterpret_cast<Main>(GetProcAddress(module,"EffectMain"));
 if(!effect)return 3;
 PF_InData in{};fl_mock::configure(in);in.inter.add_param=&collect;
 PF_OutData out{};
 if(effect(PF_Cmd_PARAMS_SETUP,&in,&out,nullptr,nullptr,nullptr))return 4;
 if(out.num_params!=65||capture.size()!=64){
  std::cerr<<"Count mismatch: out "<<out.num_params<<" callbacks "<<capture.size()<<"\n";
  return 5;
 }
 int stack=0;
 for(size_t i=0;i<capture.size();i++){
  const auto& p=capture[i];
  if(p.uu.id!=int(i)+1){std::cerr<<"Unstable param ID "<<p.uu.id<<" expected "<<i+1<<"\n";return 6;}
  if(p.param_type==PF_Param_GROUP_START)stack++;
  if(p.param_type==PF_Param_GROUP_END && --stack<0)return 7;
  if(p.param_type==PF_Param_FLOAT_SLIDER){
   const auto& d=p.u.fs_d;
   if(d.valid_min>d.slider_min||d.slider_min>d.slider_max||
      d.slider_max>d.valid_max||d.value<d.valid_min||d.value>d.valid_max){
    std::cerr<<"Invalid float control "<<p.PF_DEF_NAME<<"\n";return 8;
   }
  }
  if(p.param_type==PF_Param_SLIDER){
   const auto& d=p.u.sd;
   if(d.valid_min>d.slider_min||d.slider_min>d.slider_max||
      d.slider_max>d.valid_max||d.value<d.valid_min||d.value>d.valid_max){
    std::cerr<<"Invalid integer slider "<<p.PF_DEF_NAME<<"\n";return 9;
   }
  }
  if(p.param_type==PF_Param_POPUP){
   if(p.u.pd.num_choices<1 || p.u.pd.dephault<1 ||
       p.u.pd.dephault>p.u.pd.num_choices || !p.u.pd.u.PF_DEF_NAMESPTR)return 10;
   const std::string choices=p.u.pd.u.PF_DEF_NAMESPTR;
   if(int(std::count(choices.begin(),choices.end(),'|'))+1!=p.u.pd.num_choices)
    return 11;
  }
 }
 if(stack!=0)return 12;
 if(capture[0].param_type!=PF_Param_POINT||capture[0].u.td.restrict_bounds||
    capture[0].u.td.x_dephault!=FLOAT2FIX(50.f)||
    capture[0].u.td.y_dephault!=FLOAT2FIX(50.f))return 13;
 if(capture[8].param_type!=PF_Param_POPUP||capture[8].u.pd.num_choices!=1||
    !(capture[8].ui_flags&PF_PUI_INVISIBLE))return 14;
 if(capture[9].param_type!=PF_Param_COLOR)return 15;
 if(capture[4].param_type!=PF_Param_FLOAT_SLIDER||
    std::fabs(capture[4].u.fs_d.dephault-2.)>1e-5)return 17;
 if(capture[1].param_type!=PF_Param_FLOAT_SLIDER||
    std::fabs(capture[1].u.fs_d.dephault-1000.)>1e-5)return 18;
 if(capture[54].param_type!=PF_Param_FLOAT_SLIDER||
    std::fabs(capture[54].u.fs_d.dephault-64.)>1e-5)return 19;
 if(capture[2].param_type!=PF_Param_SLIDER||
    capture[2].u.sd.dephault!=64)return 20;
 if(capture[26].param_type!=PF_Param_FLOAT_SLIDER||
    std::fabs(capture[26].u.fs_d.dephault-.0008)>1e-5)return 21;
 if(capture[10].param_type!=PF_Param_POPUP ||
    capture[10].u.pd.num_choices!=8 || capture[10].u.pd.dephault!=1)return 16;
 if(capture[55].param_type!=PF_Param_CHECKBOX||
    capture[55].u.bd.dephault!=FALSE)return 22;
 if(capture[56].param_type!=PF_Param_CHECKBOX||
    capture[56].u.bd.dephault!=TRUE)return 23;
 if(capture[2].u.sd.valid_max!=2048)return 24;
 if(capture[16].param_type!=PF_Param_POPUP ||
    capture[16].u.pd.num_choices!=4 ||
    capture[16].u.pd.dephault!=4)return 43;
 if(capture[6].u.sd.dephault!=512||capture[6].u.sd.valid_max<512)return 25;
 if(capture[57].param_type!=PF_Param_FLOAT_SLIDER||
    std::fabs(capture[57].u.fs_d.dephault-96.)>1e-5)return 26;
 // The 12 established quick-preset indices stay immutable; ALL original
 // Nuke prescriptions are also addressable with a persistent project ID.
 if(capture[58].param_type!=PF_Param_POPUP||
    capture[58].u.pd.num_choices!=2||capture[58].u.pd.dephault!=1)
    return 27;
 if(capture[59].param_type!=PF_Param_SLIDER||
    capture[59].u.sd.valid_min!=1||
    capture[59].u.sd.valid_max!=1370||
    capture[59].u.sd.dephault!=1)return 28;
 if(capture[60].param_type!=PF_Param_BUTTON||
    !(capture[60].flags&PF_ParamFlag_SUPERVISE))return 29;
 if(capture[61].param_type!=PF_Param_ARBITRARY_DATA||
    !(capture[61].ui_flags&PF_PUI_CONTROL)||
    capture[62].param_type!=PF_Param_BUTTON)return 30;
 // The new parameter is append-only and OFF by default. AE 2023-era
 // projects must continue to render with the exact old Nuke weighting.
 if(capture.size()!=64||
    capture[63].param_type!=PF_Param_CHECKBOX||
    capture[63].u.bd.dephault!=FALSE)return 44;
 // AE owns custom optical data. Exercise actual exported AEX arbitrary
 // callbacks including binary project serialization and text copy/paste.
 auto document=capture[61].u.arb_d.value;
 if(!document||fl_mock::size(document)!=sizeof(fl::custom::Document))return 31;
 const auto* original=static_cast<const fl::custom::Document*>(fl_mock::lock(document));
 if(!original||!fl::custom::valid(*original))return 32;
 const fl::custom::Document before_clone=*original;
 PF_ArbParamsExtra arb{};
 A_u_long n=0;
 arb.which_function=PF_Arbitrary_FLAT_SIZE_FUNC;
 arb.u.flat_size_func_params.arbH=document;
 arb.u.flat_size_func_params.flat_data_sizePLu=&n;
 if(effect(PF_Cmd_ARBITRARY_CALLBACK,&in,&out,nullptr,nullptr,&arb)||
    n!=sizeof(fl::custom::Document))return 33;
 std::vector<unsigned char> bytes(n);
 arb={};arb.which_function=PF_Arbitrary_FLATTEN_FUNC;
 arb.u.flatten_func_params.arbH=document;
 arb.u.flatten_func_params.buf_sizeLu=n;
 arb.u.flatten_func_params.flat_dataPV=bytes.data();
 if(effect(PF_Cmd_ARBITRARY_CALLBACK,&in,&out,nullptr,nullptr,&arb))return 34;
 PF_ArbitraryH restored=nullptr;
 arb={};arb.which_function=PF_Arbitrary_UNFLATTEN_FUNC;
 arb.u.unflatten_func_params.buf_sizeLu=n;
 arb.u.unflatten_func_params.flat_dataPV=bytes.data();
 arb.u.unflatten_func_params.arbPH=&restored;
 if(effect(PF_Cmd_ARBITRARY_CALLBACK,&in,&out,nullptr,nullptr,&arb)||
    !restored||std::memcmp(original,fl_mock::lock(restored),n))return 35;
 // Reject malicious or damaged project blobs without reading beyond data.
 bytes[0]^=1;
 PF_ArbitraryH broken=nullptr;
 arb.u.unflatten_func_params.arbPH=&broken;
 if(effect(PF_Cmd_ARBITRARY_CALLBACK,&in,&out,nullptr,nullptr,&arb)==PF_Err_NONE||
    broken)return 36;
 bytes[0]^=1;
 PF_ArbitraryH duplicate=nullptr;
 arb={};arb.which_function=PF_Arbitrary_COPY_FUNC;
 arb.u.copy_func_params.src_arbH=document;
 arb.u.copy_func_params.dst_arbPH=&duplicate;
 if(effect(PF_Cmd_ARBITRARY_CALLBACK,&in,&out,nullptr,nullptr,&arb)||
    !duplicate||std::memcmp(fl_mock::lock(duplicate),original,n))return 37;
 A_u_long print_len=0;
 arb={};arb.which_function=PF_Arbitrary_PRINT_SIZE_FUNC;
 arb.u.print_size_func_params.arbH=document;
 arb.u.print_size_func_params.print_sizePLu=&print_len;
 if(effect(PF_Cmd_ARBITRARY_CALLBACK,&in,&out,nullptr,nullptr,&arb)||
    print_len<7+2*n)return 38;
 std::vector<char> encoded(print_len);
 arb={};arb.which_function=PF_Arbitrary_PRINT_FUNC;
 arb.u.print_func_params.arbH=document;
 arb.u.print_func_params.print_sizeLu=print_len;
 arb.u.print_func_params.print_bufferPC=encoded.data();
 if(effect(PF_Cmd_ARBITRARY_CALLBACK,&in,&out,nullptr,nullptr,&arb))return 39;
 PF_ArbitraryH scanned=nullptr;
 arb={};arb.which_function=PF_Arbitrary_SCAN_FUNC;
 arb.u.scan_func_params.bufPC=encoded.data();
 arb.u.scan_func_params.bytes_to_scanLu=print_len;
 arb.u.scan_func_params.arbPH=&scanned;
 if(effect(PF_Cmd_ARBITRARY_CALLBACK,&in,&out,nullptr,nullptr,&arb)||
    !scanned||std::memcmp(fl_mock::lock(scanned),original,n))return 40;
 fl_mock::dispose(restored);
 fl_mock::dispose(duplicate);
 fl_mock::dispose(scanned);
 // The explicit Copy Catalog Lens to Custom button must write the project
 // property (not merely alter a process-global cache or selected label).
 std::vector<PF_ParamDef*> parameters(capture.size()+1);
 parameters[0]=nullptr;
 for(size_t i=0;i<capture.size();i++)parameters[i+1]=&capture[i];
 capture[59].u.sd.value=215;
 PF_UserChangedParamExtra user{};
 user.param_index=63; // custom clone button
 if(effect(PF_Cmd_USER_CHANGED_PARAM,&in,&out,parameters.data(),nullptr,&user))
  return 41;
 const auto* copied=static_cast<const fl::custom::Document*>(
     fl_mock::lock(capture[61].u.arb_d.value));
 if(capture[58].u.pd.value!=2||!copied||!fl::custom::valid(*copied)||
    std::memcmp(copied,&before_clone,n)==0)return 42;
 std::cout<<"ACTUAL_AEX_CUSTOM_LENS_ARBITRARY_PROJECT_AND_TEXT_ROUNDTRIP_PASSED\n";
 std::cout<<"ACTUAL_AEX_CATALOG_TO_CUSTOM_CLONE_BUTTON_PASSED\n";
 // Every renderable embedded lens up to the 64-surface CUDA contract must
 // be cloneable into the same serialized custom model: no Nikkor fallback.
 using Inspect=int(*)(int,fl::catalog::Report*);
 auto inspect=reinterpret_cast<Inspect>(GetProcAddress(module,"FlareLensInspectCatalog"));
 if(!inspect)return 43;
 int clones=0,excluded=0;
 for(int i=1;i<=fl::catalog::kCount;i++){
  fl::catalog::Report report{};
  if(!inspect(i,&report))return 44;
  if(!report.renderable||report.surface_count>fl::custom::kMaxSurfaces){
   ++excluded;continue;
  }
  capture[59].u.sd.value=i;
  if(effect(PF_Cmd_USER_CHANGED_PARAM,&in,&out,parameters.data(),nullptr,&user)){
   std::cerr<<"Custom clone rejected "<<i<<" "<<report.filename<<"\n";
   return 45;
  }
  const auto* current=static_cast<const fl::custom::Document*>(
      fl_mock::lock(capture[61].u.arb_d.value));
  if(!current||!fl::custom::valid(*current)||
     current->count!=unsigned(report.surface_count))return 46;
  ++clones;
 }
 std::cout<<"ACTUAL_AEX_CUSTOM_CLONES_FROM_EMBEDDED_CATALOG="
          <<clones<<" excluded_unrenderable_or_over64="<<excluded<<"\n";
 std::cout<<"Verified 63 AE-native UI parameters, 7 balanced groups, "
          <<"limits, defaults, popup items and off-frame Light Position\n";
 FreeLibrary(module);
 return 0;
}
