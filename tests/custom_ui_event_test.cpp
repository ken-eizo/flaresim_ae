// Execute production PF_Cmd_EVENT (without running Adobe After Effects).
// Validate actual arbitrary optical data mutations and event flags.
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <vector>
#include <iostream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include "AE_Effect.h"
#include "AE_Macros.h"
#include "SPBasic.h"
#include "mock_custom_ui_host.hpp"
#include "FlareLensCustom.hpp"

using Entry=PF_Err(*)(PF_Cmd,PF_InData*,PF_OutData*,PF_ParamDef**,PF_LayerDef*,void*);
static std::vector<PF_ParamDef> defs;
static PF_Err collect(PF_ProgPtr,PF_ParamIndex i,PF_ParamDefPtr p){
 if(i!=-1||!p)return PF_Err_BAD_CALLBACK_PARAM;
 defs.push_back(*p);fl_mock::activate(defs.back());return PF_Err_NONE;
}
static SPErr SPAPI unavailable(const char*,int32,const void**){return 1;}
static SPErr SPAPI release(const char*,int32){return 0;}
int main(){
 wchar_t filename[MAX_PATH]{};
 if(!GetModuleFileNameW(nullptr,filename,MAX_PATH))return 1;
 const HMODULE module=LoadLibraryW((std::filesystem::path(filename).parent_path()/
                                    L"FlareLensAE.aex").c_str());
 if(!module)return 2;
 const auto effect=reinterpret_cast<Entry>(GetProcAddress(module,"EffectMain"));
 if(!effect)return 3;
 SPBasicSuite basic{};basic.AcquireSuite=&unavailable;basic.ReleaseSuite=&release;
 PF_InData in{};fl_mock::configure(in);in.pica_basicP=&basic;
 in.inter.add_param=&collect;
 PF_OutData out{};
 if(effect(PF_Cmd_PARAMS_SETUP,&in,&out,nullptr,nullptr,nullptr)||
    defs.size()!=63)return 4;
 // Regression: AE may dispatch lifecycle event selectors before an edit
 // document or params[] exists, e.g. when opening the ECW from timeline.
 for(const PF_EventType type:{PF_Event_NEW_CONTEXT,PF_Event_ACTIVATE,
                              PF_Event_DEACTIVATE,PF_Event_CLOSE_CONTEXT}){
  PF_EventExtra life{};life.e_type=type;
  if(effect(PF_Cmd_EVENT,&in,&out,nullptr,nullptr,&life)!=PF_Err_NONE)
   return 14;
 }
 std::vector<PF_ParamDef*> ps(defs.size()+1);
 PF_ParamDef layer{};ps[0]=&layer;
 for(size_t i=0;i<defs.size();++i)ps[i+1]=&defs[i];
 auto* original=static_cast<fl::custom::Document*>(fl_mock::lock(defs[61].u.arb_d.value));
 if(!original||!fl::custom::valid(*original))return 5;
 const unsigned start_count=original->count;
 PF_Context ctx{};ctx.magic=PF_CONTEXT_MAGIC;ctx.w_type=PF_Window_EFFECT;
 PF_ContextPtr ctxptr=&ctx;
 PF_EventExtra evt{};
 evt.contextH=&ctxptr;
 evt.window_union.effect_win.area=PF_EA_CONTROL;
 evt.window_union.effect_win.current_frame={0,0,416,314};
 evt.e_type=PF_Event_DO_CLICK;
 evt.u.do_click.screen_point={369,12}; // plus button at right of compact header
 if(effect(PF_Cmd_EVENT,&in,&out,ps.data(),nullptr,&evt))return 6;
 if(original->count!=start_count+1||defs[58].u.pd.value!=2||
    !(evt.evt_out_flags&PF_EO_HANDLED_EVENT)||
    !(evt.evt_out_flags&PF_EO_ALWAYS_UPDATE))return 7;
 // Selection-only operations should NOT request a composition render.
 defs[58].u.pd.value=1;
 evt={};evt.contextH=&ctxptr;evt.window_union.effect_win.area=PF_EA_CONTROL;
 evt.window_union.effect_win.current_frame={0,0,416,314};
 evt.e_type=PF_Event_DO_CLICK;
 evt.u.do_click.screen_point={312,12}; // previous surface without changing optical mode
 if(effect(PF_Cmd_EVENT,&in,&out,ps.data(),nullptr,&evt))return 8;
 if(defs[58].u.pd.value!=1)return 9;
 // An arbitrary-value lane is a deliberate editing gesture, not an
 // accidental switch caused by browsing the selected optical element.
 evt={};evt.contextH=&ctxptr;evt.window_union.effect_win.area=PF_EA_CONTROL;
 evt.window_union.effect_win.current_frame={0,0,416,314};
 evt.e_type=PF_Event_DO_CLICK;
 evt.u.do_click.screen_point={360,222}; // refractive-index numeric scrub area
 if(effect(PF_Cmd_EVENT,&in,&out,ps.data(),nullptr,&evt))return 10;
 if(!evt.u.do_click.send_drag||defs[58].u.pd.value!=1)return 11;
 const auto index=original->selected;
 const float before=original->elements[index].ior;
 evt.e_type=PF_Event_DRAG;
 evt.u.do_click.screen_point.h+=30;
 evt.u.do_click.last_time=TRUE;
 evt.evt_out_flags=0;
 if(effect(PF_Cmd_EVENT,&in,&out,ps.data(),nullptr,&evt))return 12;
 if(std::abs(original->elements[index].ior-before)<.01f||
    defs[58].u.pd.value!=2||
    !(evt.evt_out_flags&PF_EO_ALWAYS_UPDATE))return 13;
 // Inspector diameter is a horizontal scrub; the lens rim itself
 // remains a vertical geometric drag.
 evt={};evt.contextH=&ctxptr;evt.window_union.effect_win.area=PF_EA_CONTROL;
 evt.window_union.effect_win.current_frame={0,0,416,314};
 evt.e_type=PF_Event_DO_CLICK;
 evt.u.do_click.screen_point={360,200}; // diameter numeric scrub area
 if(effect(PF_Cmd_EVENT,&in,&out,ps.data(),nullptr,&evt)||
    !evt.u.do_click.send_drag)return 15;
 const float diameter_before=original->elements[original->selected].semi;
 evt.e_type=PF_Event_DRAG;evt.u.do_click.screen_point.h+=20;
 evt.u.do_click.last_time=TRUE;
 if(effect(PF_Cmd_EVENT,&in,&out,ps.data(),nullptr,&evt))return 16;
 if(std::abs(original->elements[original->selected].semi-
             diameter_before)<.05f)return 17;
 // Slider-track click assigns an absolute value immediately; dragging
 // the thumb changes it continuously, without requiring a mode switch.
 defs[58].u.pd.value=1;
 evt={};evt.contextH=&ctxptr;evt.window_union.effect_win.area=PF_EA_CONTROL;
 evt.window_union.effect_win.current_frame={0,0,416,314};
 evt.e_type=PF_Event_DO_CLICK;
 evt.u.do_click.screen_point={238,220}; // refractive index, row 4
 const auto sel=original->selected;
 const float previous_index=original->elements[sel].ior;
 if(effect(PF_Cmd_EVENT,&in,&out,ps.data(),nullptr,&evt))return 18;
 if(!evt.u.do_click.send_drag||defs[58].u.pd.value!=2||
    std::abs(original->elements[sel].ior-previous_index)<.03f)return 19;
 const float clicked_index=original->elements[sel].ior;
 evt.e_type=PF_Event_DRAG;evt.u.do_click.screen_point.h=282;
 evt.u.do_click.last_time=TRUE;evt.evt_out_flags=0;
 if(effect(PF_Cmd_EVENT,&in,&out,ps.data(),nullptr,&evt))return 20;
 if(original->elements[sel].ior<=clicked_index||
    !(evt.evt_out_flags&PF_EO_ALWAYS_UPDATE))return 21;
 // Switching the selected surface via the small header buttons is view
 // navigation, never a lens-source switch or full re-render.
 defs[58].u.pd.value=1;
 const auto before_nav=original->selected;
 evt={};evt.contextH=&ctxptr;evt.window_union.effect_win.area=PF_EA_CONTROL;
 evt.window_union.effect_win.current_frame={0,0,416,314};
 evt.e_type=PF_Event_DO_CLICK;evt.u.do_click.screen_point={341,12};
 if(effect(PF_Cmd_EVENT,&in,&out,ps.data(),nullptr,&evt))return 22;
 if(original->selected==(before_nav)||defs[58].u.pd.value!=1)return 23;
 std::cout<<"ACTUAL_AEX_COMPACT_SLIDER_TRACK_SCRUB_NAVIGATION_PASSED\n";
 FreeLibrary(module);
 return 0;
}
