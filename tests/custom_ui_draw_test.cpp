// Standalone Drawbot protocol simulation around the production .aex EntryPoint.
// Does not run Adobe After Effects or operate an AE window/project.
#define NOMINMAX
#define PF_USE_NEW_WINDOW_UNION
#include <windows.h>
#include <filesystem>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "AE_Effect.h"
#include "AE_EffectSuites.h"
#include "AE_Macros.h"
#include "SPBasic.h"
#include "adobesdk/DrawbotSuite.h"
#include "mock_custom_ui_host.hpp"
#include "FlareLensCustom.hpp"
using Entry=PF_Err(*)(PF_Cmd,PF_InData*,PF_OutData*,PF_ParamDef**,PF_LayerDef*,void*);
namespace {
std::vector<PF_ParamDef> defs;
int made=0,freed=0,strokes=0,texts=0,paints=0;
bool finite(float a){return std::isfinite(a)&&std::abs(a)<100000;}
template<class T> T token(){made++;return reinterpret_cast<T>(new unsigned(0x123456u));}
DRAWBOT_DrawbotSuite1 ds{};
DRAWBOT_SupplierSuite1 ss{};
DRAWBOT_SurfaceSuite2 surface{};
DRAWBOT_PathSuite1 path{};
DRAWBOT_PenSuite1 pen{};
DRAWBOT_ImageSuite1 image{};
PF_EffectCustomUISuite1 ecw{};
PF_Err collect(PF_ProgPtr,PF_ParamIndex idx,PF_ParamDefPtr p){
 if(idx!=-1||!p)return PF_Err_BAD_CALLBACK_PARAM;
 defs.push_back(*p);fl_mock::activate(defs.back());return PF_Err_NONE;
}
SPErr SPAPI get_supplier(DRAWBOT_DrawRef r,DRAWBOT_SupplierRef* p){
 if(!r||!p)return 1;*p=reinterpret_cast<DRAWBOT_SupplierRef>(0x100);return 0;
}
SPErr SPAPI get_surface(DRAWBOT_DrawRef r,DRAWBOT_SurfaceRef* p){
 if(!r||!p)return 1;*p=reinterpret_cast<DRAWBOT_SurfaceRef>(0x200);return 0;
}
SPErr new_pen(DRAWBOT_SupplierRef,const DRAWBOT_ColorRGBA* c,float w,DRAWBOT_PenRef* p){
 if(!c||!finite(w)||w<=0)return 1;*p=token<DRAWBOT_PenRef>();return 0;
}
SPErr new_brush(DRAWBOT_SupplierRef,const DRAWBOT_ColorRGBA* c,DRAWBOT_BrushRef* p){
 if(!c)return 1;*p=token<DRAWBOT_BrushRef>();return 0;
}
SPErr new_font(DRAWBOT_SupplierRef,float size,DRAWBOT_FontRef* p){
 if(!finite(size)||size<=0)return 1;*p=token<DRAWBOT_FontRef>();return 0;
}
SPErr new_path(DRAWBOT_SupplierRef,DRAWBOT_PathRef* p){
 *p=token<DRAWBOT_PathRef>();return 0;
}
SPErr destroy(DRAWBOT_ObjectRef ref){if(!ref)return 1;delete reinterpret_cast<unsigned*>(ref);freed++;return 0;}
SPErr stroke(DRAWBOT_SurfaceRef,DRAWBOT_PenRef p,DRAWBOT_PathRef path){
 if(!p||!path)return 1;strokes++;return 0;
}
SPErr paint(DRAWBOT_SurfaceRef,const DRAWBOT_ColorRGBA* c,const DRAWBOT_RectF32* rect){
 if(!c||!rect||!finite(rect->left)||!finite(rect->top)||
  !finite(rect->width)||!finite(rect->height))return 1;
 paints++;return 0;
}
SPErr moveto(DRAWBOT_PathRef,float x,float y){return finite(x)&&finite(y)?0:1;}
SPErr lineto(DRAWBOT_PathRef,float x,float y){return finite(x)&&finite(y)?0:1;}
SPErr close_path(DRAWBOT_PathRef){return 0;}
SPErr fill(DRAWBOT_SurfaceRef,DRAWBOT_BrushRef b,DRAWBOT_PathRef p,DRAWBOT_FillType){return (b&&p)?0:1;}
SPErr draw_text(DRAWBOT_SurfaceRef,DRAWBOT_BrushRef,DRAWBOT_FontRef,
 const DRAWBOT_UTF16Char* chars,const DRAWBOT_PointF32* pos,
 DRAWBOT_TextAlignment,DRAWBOT_TextTruncation,float){
 if(!chars||!pos||!finite(pos->x)||!finite(pos->y))return 1;
 texts++;return 0;
}
PF_Err SPAPI drawing(const PF_ContextH ctx,DRAWBOT_DrawRef* r){
 if(!ctx||!*ctx||!r)return PF_Err_BAD_CALLBACK_PARAM;
 *r=reinterpret_cast<DRAWBOT_DrawRef>(0x300);
 return PF_Err_NONE;
}
SPErr SPAPI acquire(const char* name,int32 version,const void** p){
 if(!name||!p)return 1;
 #define GIVE(N,V,T) if(!std::strcmp(name,N) && version==V){*p=&T;return 0;}
 GIVE(kDRAWBOT_DrawSuite,kDRAWBOT_DrawSuite_Version1,ds)
 GIVE(kDRAWBOT_SupplierSuite,kDRAWBOT_SupplierSuite_Version1,ss)
 GIVE(kDRAWBOT_SurfaceSuite,kDRAWBOT_SurfaceSuite_Version2,surface)
 GIVE(kDRAWBOT_PathSuite,kDRAWBOT_PathSuite_Version1,path)
 GIVE(kDRAWBOT_PenSuite,kDRAWBOT_PenSuite_Version1,pen)
 GIVE(kDRAWBOT_ImageSuite,kDRAWBOT_ImageSuite_Version1,image)
 GIVE(kPFEffectCustomUISuite,kPFEffectCustomUISuiteVersion1,ecw)
 #undef GIVE
 return 1;
}
SPErr SPAPI release(const char*,int32){return 0;}
}
int main(){
 ds.GetSupplier=&get_supplier;ds.GetSurface=&get_surface;
 ss.NewPen=&new_pen;ss.NewBrush=&new_brush;
 ss.NewDefaultFont=&new_font;ss.NewPath=&new_path;ss.ReleaseObject=&destroy;
 surface.PaintRect=&paint;surface.StrokePath=&stroke;surface.DrawString=&draw_text;
 surface.FillPath=&fill;
 path.MoveTo=&moveto;path.LineTo=&lineto;path.Close=&close_path;
 ecw.PF_GetDrawingReference=&drawing;
 wchar_t pathbuf[MAX_PATH]{};
 if(!GetModuleFileNameW(nullptr,pathbuf,MAX_PATH))return 1;
 const auto mod=LoadLibraryW((std::filesystem::path(pathbuf).parent_path()/
                              L"FlareLensAE.aex").c_str());
 if(!mod)return 2;
 auto effect=reinterpret_cast<Entry>(GetProcAddress(mod,"EffectMain"));
 if(!effect)return 3;
 SPBasicSuite basic{};basic.AcquireSuite=&acquire;basic.ReleaseSuite=&release;
 PF_InData in{};fl_mock::configure(in);in.pica_basicP=&basic;
 in.inter.add_param=&collect;
 PF_OutData out{};
 if(effect(PF_Cmd_PARAMS_SETUP,&in,&out,nullptr,nullptr,nullptr)||
  defs.size()!=63)return 4;
 std::vector<PF_ParamDef*> ps(defs.size()+1);
 PF_ParamDef inparam{};ps[0]=&inparam;
 for(size_t i=0;i<defs.size();i++)ps[i+1]=&defs[i];
 PF_Context context{};context.magic=PF_CONTEXT_MAGIC;
 context.w_type=PF_Window_EFFECT;
 PF_ContextPtr cp=&context;
 PF_EventExtra e{};
 e.e_type=PF_Event_DRAW;e.contextH=&cp;
 e.window_union.effect_win.area=PF_EA_CONTROL;
 e.window_union.effect_win.current_frame={0,0,416,314};
 const PF_Err error=effect(PF_Cmd_EVENT,&in,&out,ps.data(),nullptr,&e);
 if(error||!texts||!strokes||paints<3||made!=freed||
  !(e.evt_out_flags&PF_EO_HANDLED_EVENT)){
  std::fprintf(stderr,
    "DRAWBOT_MOCK_FAILED err=%ld paints=%d strokes=%d texts=%d made=%d freed=%d flags=%ld\n",
    long(error),paints,strokes,texts,made,freed,long(e.evt_out_flags));
  return 5;
 }
 std::printf("DRAWBOT_AEX_EVENT_PARITY_PASSED paints=%d strokes=%d texts=%d objects=%d\n",
  paints,strokes,texts,made);
 // Damaged/unavailable user data must still produce a bounded background
 // paint, not a crash, and must never submit invalid lens geometry.
 auto* doc=static_cast<fl::custom::Document*>(
   fl_mock::lock(defs[61].u.arb_d.value));
 if(!doc)return 6;
 const auto magic=doc->magic;
 doc->magic=0;
 const int previous_paints=paints,previous_strokes=strokes;
 e.evt_out_flags=0;
 if(effect(PF_Cmd_EVENT,&in,&out,ps.data(),nullptr,&e)||
    paints<=previous_paints||strokes!=previous_strokes)return 7;
 doc->magic=magic;
 FreeLibrary(mod);
 return 0;
}
