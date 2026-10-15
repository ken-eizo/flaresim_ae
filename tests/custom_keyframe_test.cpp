// Actual AEX arbitrary keyframe interpolation, flatten/unflatten, and
// topology-transition regression. Never launches After Effects.
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <vector>
#include <cmath>
#include <cstring>
#include <iostream>
#include "AE_Effect.h"
#include "AE_Macros.h"
#include "SPBasic.h"
#include "mock_custom_ui_host.hpp"
#include "FlareLensCustom.hpp"
using Entry=PF_Err(*)(PF_Cmd,PF_InData*,PF_OutData*,PF_ParamDef**,PF_LayerDef*,void*);
static std::vector<PF_ParamDef> defs;
static PF_Err collect(PF_ProgPtr,PF_ParamIndex i,PF_ParamDefPtr p) {
 if(i!=-1||!p)return PF_Err_BAD_CALLBACK_PARAM;
 defs.push_back(*p);fl_mock::activate(defs.back());return PF_Err_NONE;
}
static PF_Handle duplicate(const fl::custom::Document& d) {
 auto h=fl_mock::new_handle(sizeof(d));
 if(h)std::memcpy(fl_mock::lock(h),&d,sizeof(d));
 return h;
}
static bool same(const fl::custom::Document& a,const fl::custom::Document& b) {
 return std::memcmp(&a,&b,sizeof(a))==0;
}
int main(){
 wchar_t filename[MAX_PATH]{};
 if(!GetModuleFileNameW(nullptr,filename,MAX_PATH))return 1;
 auto dll=LoadLibraryW((std::filesystem::path(filename).parent_path()/L"FlareLensAE.aex").c_str());
 if(!dll)return 2;
 auto fx=reinterpret_cast<Entry>(GetProcAddress(dll,"EffectMain"));
 if(!fx)return 3;
 PF_InData in{};fl_mock::configure(in);in.inter.add_param=&collect;
 PF_OutData out{};
 if(fx(PF_Cmd_PARAMS_SETUP,&in,&out,nullptr,nullptr,nullptr)||defs.size()!=63)return 4;
 const auto* initial=static_cast<fl::custom::Document*>(fl_mock::lock(defs[61].u.arb_d.value));
 if(!initial||!fl::custom::valid(*initial))return 5;
 auto left=*initial,right=left;
 const auto prev=left.elements[0];
 right.focal+=20;
 right.elements[0].radius+=12;
 right.elements[0].thickness+=2;
 right.elements[0].ior+=.2f;
 right.elements[0].abbe+=7;
 right.elements[0].semi+=1;
 right.elements[0].coating=right.elements[0].coating?0:1;
 right.elements[0].stop=right.elements[0].stop?0:1;
 right.selected=uint32_t(right.count-1);
 if(!fl::custom::valid(right))return 6;
 PF_Handle a=duplicate(left),b=duplicate(right),mid=nullptr;
 PF_ArbParamsExtra arb{};
 arb.which_function=PF_Arbitrary_INTERP_FUNC;
 arb.u.interp_func_params.left_arbH=a;
 arb.u.interp_func_params.right_arbH=b;
 arb.u.interp_func_params.tF=.25f;
 arb.u.interp_func_params.interpPH=&mid;
 if(fx(PF_Cmd_ARBITRARY_CALLBACK,&in,&out,nullptr,nullptr,&arb)||!mid)return 7;
 const auto result=*static_cast<const fl::custom::Document*>(fl_mock::lock(mid));
 if(!fl::custom::valid(result)||result.count!=left.count||result.selected!=left.selected||
    std::fabs(result.focal-(left.focal+5.f))>1e-3f||
    std::fabs(result.elements[0].radius-(prev.radius+3.f))>1e-3f||
    std::fabs(result.elements[0].ior-(prev.ior+.05f))>1e-4f||
    result.elements[0].coating!=prev.coating||
    result.elements[0].stop!=prev.stop)return 8;
 PF_Handle late=nullptr;
 arb.u.interp_func_params.tF=.75f;
 arb.u.interp_func_params.interpPH=&late;
 if(fx(PF_Cmd_ARBITRARY_CALLBACK,&in,&out,nullptr,nullptr,&arb)||!late)return 9;
 const auto result2=*static_cast<const fl::custom::Document*>(fl_mock::lock(late));
 if(result2.elements[0].coating!=right.elements[0].coating||
    result2.elements[0].stop!=right.elements[0].stop||
    result2.selected!=left.selected)return 10;
 auto changed=right;
 changed.count--;changed.selected=0;
 PF_Handle topology=duplicate(changed),held=nullptr;
 arb.u.interp_func_params.right_arbH=topology;
 arb.u.interp_func_params.tF=.8f;
 arb.u.interp_func_params.interpPH=&held;
 if(fx(PF_Cmd_ARBITRARY_CALLBACK,&in,&out,nullptr,nullptr,&arb)||
    !held||!same(*static_cast<const fl::custom::Document*>(fl_mock::lock(held)),left))
    return 11;
 std::vector<unsigned char> buffer(sizeof(fl::custom::Document));
 arb.which_function=PF_Arbitrary_FLATTEN_FUNC;
 arb.u.flatten_func_params.arbH=mid;
 arb.u.flatten_func_params.buf_sizeLu=static_cast<A_u_long>(buffer.size());
 arb.u.flatten_func_params.flat_dataPV=buffer.data();
 if(fx(PF_Cmd_ARBITRARY_CALLBACK,&in,&out,nullptr,nullptr,&arb))return 12;
 PF_Handle unpacked=nullptr;
 arb.which_function=PF_Arbitrary_UNFLATTEN_FUNC;
 arb.u.unflatten_func_params.buf_sizeLu=static_cast<A_u_long>(buffer.size());
 arb.u.unflatten_func_params.flat_dataPV=buffer.data();
 arb.u.unflatten_func_params.arbPH=&unpacked;
 if(fx(PF_Cmd_ARBITRARY_CALLBACK,&in,&out,nullptr,nullptr,&arb)||!unpacked)return 13;
 if(!same(*static_cast<const fl::custom::Document*>(fl_mock::lock(unpacked)),result))return 14;
 for(auto h:{a,b,topology,mid,late,held,unpacked})fl_mock::dispose(h);
 std::cout<<"AEX_KEYFRAME_LINEAR_OPTICS_DISCRETE_TOPOLOGY_AND_PERSISTENCE_PASSED\n";
 FreeLibrary(dll);
 return 0;
}
