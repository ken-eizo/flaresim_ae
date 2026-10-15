#include "FlareLensCustomUI.hpp"
#include "FlareLensCatalog.hpp"
#include "AEFX_SuiteHelper.h"
#include "AE_EffectSuites.h"
#include "AE_EffectCBSuites.h"
#include "AEGP_SuiteHandler.h"
#include "adobesdk/DrawbotSuite.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <limits>

namespace fl::custom::ae {
namespace {
constexpr uintptr_t kRefcon=0x464c4355u; // FLCU
constexpr char hex[]="0123456789ABCDEF";
constexpr float kRowsStart=159.f,kRowStep=18.f;
bool init_default(Document& d){
 const auto* original=fl::catalog::lens_by_index(1);
 if(!original)return false;
 d=from_lens(*original);
 return valid(d);
}
PF_Err alloc(PF_InData* in_data, PF_ArbitraryH* dst,const Document& d){
 if(!dst||!valid(d))return PF_Err_BAD_CALLBACK_PARAM;
 PF_Handle h=PF_NEW_HANDLE(sizeof(Document));
 if(!h)return PF_Err_OUT_OF_MEMORY;
 auto* p=static_cast<Document*>(PF_LOCK_HANDLE(h));
 if(!p){PF_DISPOSE_HANDLE(h);return PF_Err_OUT_OF_MEMORY;}
 std::memcpy(p,&d,sizeof(d));PF_UNLOCK_HANDLE(h);
 *dst=h;return PF_Err_NONE;
}
void dirty(PF_InData* in_data,PF_OutData*,PF_EventExtra* e){
 // Paint the ECW region only; Drawbot owns the actual presentation.
 // A missing optional PICA suite is not a render/UI error.
 if(in_data&&in_data->pica_basicP&&
    in_data->pica_basicP->AcquireSuite&&
    in_data->pica_basicP->ReleaseSuite){
  const void* borrowed=nullptr;
  if(!in_data->pica_basicP->AcquireSuite(
       kPFAppSuite,kPFAppSuiteVersion4,&borrowed)){
   const auto* app=static_cast<const PFAppSuite4*>(borrowed);
   if(app&&app->PF_InvalidateRect){
    PF_Rect rect=e->window_union.effect_win.current_frame;
    app->PF_InvalidateRect(e->contextH,&rect);
   }
   in_data->pica_basicP->ReleaseSuite(kPFAppSuite,kPFAppSuiteVersion4);
  }
 }
 e->evt_out_flags|=PF_EO_HANDLED_EVENT|PF_EO_UPDATE_NOW;
}
struct Layout{
 float left=0,top=0,width=0,height=0,x0=0,x1=0,cy=0,ym=1,scale=1,z0=0;
 explicit Layout(const PF_Rect& r,const LensSystem& l){
  left=float(r.left);top=float(r.top);
  width=float(r.right-r.left);height=float(r.bottom-r.top);
  x0=left+14.f;x1=left+width-16.f;cy=top+80.f;
  float minz=0,maxz=std::max(1.f,l.sensor_z),ap=1;
  for(const auto& s:l.surfaces){
   if(std::isfinite(s.z)){minz=std::min(minz,s.z);maxz=std::max(maxz,s.z);}
   if(std::isfinite(s.semi_aperture)&&s.semi_aperture>0&&s.semi_aperture<10000)
    ap=std::max(ap,s.semi_aperture);
  }
  const float dx=std::max(1.f,maxz-minz);
  z0=minz-.035f*dx;scale=(x1-x0)/(dx*1.1f);
  ym=std::max(1.f,ap*1.15f);
 }
 float x(float z)const{return x0+(z-z0)*scale;}
 float y(float h)const{return cy-h*43.f/ym;}
 float track_start()const{return left+108.f;}
 float track_end()const{return left+width-93.f;}
};
float sag(const Surface& s,float h){
 if(std::abs(s.radius)<1.e-6f)return 0;
 float r=s.radius,hh=std::min(std::abs(h),std::abs(r));
 return r-std::copysign(std::sqrt(std::max(0.f,r*r-hh*hh)),r);
}
bool in_range(float v,float lo,float hi){return v>=lo&&v<=hi;}
enum Mode{None=0,Radius=1,Thickness=2,Aperture=3,Ior=4,Abbe=5,Coating=6,
          Position=7,Stop=8,Focal=9};
constexpr Mode kModes[8]={Radius,Thickness,Aperture,Ior,Abbe,Coating,Stop,Focal};
int field_row(float y,const Layout& l){
 if(y<l.top+kRowsStart||y>=l.top+kRowsStart+8.f*kRowStep)return -1;
 return std::clamp(int((y-l.top-kRowsStart)/kRowStep),0,7);
}
Mode property(float x,float y,const Layout& l){
 if(x<l.left+8||x>=l.left+l.width-6)return None;
 const int row=field_row(y,l);
 return row<0?None:kModes[row];
}
// Optical surface parameters have drastically different scales. A
// quasi-logarithmic radius and logarithmic length give usable travel for
// both small and large values, unlike linear 0..1000 sliders.
float track_value(Mode mode,float t){
 t=std::clamp(t,0.f,1.f);
 switch(mode){
 case Radius:return 45.f*std::sinh((2.f*t-1.f)*std::asinh(1000.f/45.f));
 case Thickness:return .005f*std::pow(60000.f,t);
 case Aperture:return .02f*std::pow(10000.f,t);
 case Ior:return 1.f+1.5f*t;
 case Abbe:return 120.f*t;
 case Coating:return std::round(12.f*t);
 case Focal:return 5.f*std::pow(80.f,t);
 default:return t;
 }
}
float track_fraction(Mode mode,const Document& d){
 const auto& s=d.elements[d.selected];
 switch(mode){
 case Radius:return .5f+.5f*std::asinh(s.radius/45.f)/std::asinh(1000.f/45.f);
 case Thickness:return std::log(std::max(.005f,s.thickness)/.005f)/std::log(60000.f);
 case Aperture:return std::log(std::max(.02f,s.semi)/.02f)/std::log(10000.f);
 case Ior:return (s.ior-1.f)/1.5f;
 case Abbe:return s.abbe/120.f;
 case Coating:return float(s.coating)/12.f;
 case Stop:return s.stop?1.f:0.f;
 case Focal:return std::log(std::max(5.f,d.focal)/5.f)/std::log(80.f);
 default:return 0.f;
 }
}
bool apply_track(Document& d,Mode mode,float t){
 Document candidate=d;
 auto& s=candidate.elements[candidate.selected];
 const float v=track_value(mode,t);
 switch(mode){
 case Radius:{
  const float rmin=s.semi;
  s.radius=(std::abs(v)<rmin&&std::abs(v)>1.e-6f)?
    std::copysign(rmin,v):v;
 }break;
 case Thickness:s.thickness=v;break;
 case Aperture:{
  const float maximum=std::abs(s.radius)>1.e-6f?
    std::min(250.f,std::abs(s.radius)):250.f;
  s.semi=std::clamp(v,.01f,maximum);
 }break;
 case Ior:s.ior=v;break;
 case Abbe:s.abbe=v;break;
 case Coating:s.coating=int(v);break;
 case Focal:candidate.focal=v;break;
 default:return false;
 }
 if(!valid(candidate))return false;
 d=candidate;
 return true;
}
int closest(const Document& doc,const Layout& L,float x,float y,Mode& mode){
 auto lens=make_lens(doc);
 int idx=-1;float nearest=1000;
 for(int i=0;i<lens.num_surfaces();i++){
  const auto& s=lens.surfaces[i];
  const float top=L.y(s.semi_aperture),bot=L.y(-s.semi_aperture);
  const float h=std::clamp((L.cy-y)*L.ym/43.f,
                            -s.semi_aperture,s.semi_aperture);
  const float cx=L.x(s.z+sag(s,h));
  const float dy=std::min(std::abs(y-top),std::abs(y-bot));
  float dist=std::abs(x-cx);
  if(y<top-10||y>bot+10)dist+=40;
  if(dist<nearest){
   nearest=dist;idx=i;
   mode=dy<12?Aperture:(std::abs(y-L.cy)<9?Position:Radius);
  }
 }
 return nearest<13?idx:-1;
}
#include "FlareLensCustomDraw.inl"
} // anonymous
bool read(PF_InData* in_data,PF_ArbitraryH h,Document& d) noexcept {
 if(!h||!in_data||!in_data->utils||
    !in_data->utils->host_get_handle_size||
    in_data->utils->host_get_handle_size(h)<sizeof(Document))return false;
 const auto* p=static_cast<const Document*>(PF_LOCK_HANDLE(h));
 if(!p)return false;
 std::memcpy(&d,p,sizeof(d));PF_UNLOCK_HANDLE(h);
 return valid(d);
}
bool store(PF_InData* in_data,PF_ArbitraryH h,const Document& d) noexcept {
 if(!h||!valid(d)||!in_data||!in_data->utils||
    !in_data->utils->host_get_handle_size||
    in_data->utils->host_get_handle_size(h)<sizeof(Document))return false;
 auto* p=static_cast<Document*>(PF_LOCK_HANDLE(h));
 if(!p)return false;
 std::memcpy(p,&d,sizeof(d));PF_UNLOCK_HANDLE(h);
 return true;
}
PF_Err make_default(PF_InData* in_data,PF_ArbitraryH* dst){
 Document d{};if(!init_default(d))return PF_Err_BAD_CALLBACK_PARAM;
 return alloc(in_data,dst,d);
}
PF_Err arbitrary(PF_InData* in_data,PF_OutData* out,PF_ArbParamsExtra* e){
 if(!in_data||!out||!e)return PF_Err_BAD_CALLBACK_PARAM;
 const auto assign=[&](PF_ArbitraryH h,PF_ArbitraryH* dst)->PF_Err{
  Document doc{};if(!read(in_data,h,doc))return PF_Err_BAD_CALLBACK_PARAM;
  return alloc(in_data,dst,doc);
 };
 switch(e->which_function){
 case PF_Arbitrary_NEW_FUNC:return make_default(in_data,e->u.new_func_params.arbPH);
 case PF_Arbitrary_DISPOSE_FUNC:
  if(e->u.dispose_func_params.arbH)PF_DISPOSE_HANDLE(e->u.dispose_func_params.arbH);
  return PF_Err_NONE;
 case PF_Arbitrary_COPY_FUNC:return assign(e->u.copy_func_params.src_arbH,
                                             e->u.copy_func_params.dst_arbPH);
 case PF_Arbitrary_FLAT_SIZE_FUNC:
  *e->u.flat_size_func_params.flat_data_sizePLu=sizeof(Document);
  return PF_Err_NONE;
 case PF_Arbitrary_FLATTEN_FUNC:{
  if(e->u.flatten_func_params.buf_sizeLu!=sizeof(Document))return PF_Err_BAD_CALLBACK_PARAM;
  Document d{};if(!read(in_data,e->u.flatten_func_params.arbH,d))return PF_Err_BAD_CALLBACK_PARAM;
  std::memcpy(e->u.flatten_func_params.flat_dataPV,&d,sizeof(d));return PF_Err_NONE;
 }
 case PF_Arbitrary_UNFLATTEN_FUNC:{
  if(e->u.unflatten_func_params.buf_sizeLu!=sizeof(Document))return PF_Err_BAD_CALLBACK_PARAM;
  Document d{};
  std::memcpy(&d,e->u.unflatten_func_params.flat_dataPV,sizeof(d));
  if(!valid(d))return PF_Err_BAD_CALLBACK_PARAM;
  return alloc(in_data,e->u.unflatten_func_params.arbPH,d);
 }
 case PF_Arbitrary_INTERP_FUNC:{
  // AE owns the keyframe times; we only interpolate the two immutable
  // arbitrary-data snapshots. Never mutate either source handle.
  auto& u=e->u.interp_func_params;
  Document left{},right{};
  if(!read(in_data,u.left_arbH,left)||
     !read(in_data,u.right_arbH,right))return PF_Err_BAD_CALLBACK_PARAM;
  return alloc(in_data,u.interpPH,interpolate(left,right,u.tF));
 }
 case PF_Arbitrary_COMPARE_FUNC:{
  Document a{},b{};
  if(!read(in_data,e->u.compare_func_params.a_arbH,a)||
     !read(in_data,e->u.compare_func_params.b_arbH,b))return PF_Err_BAD_CALLBACK_PARAM;
  const int cmp=std::memcmp(&a,&b,sizeof(a));
  *e->u.compare_func_params.compareP=
   cmp==0?PF_ArbCompare_EQUAL:cmp<0?PF_ArbCompare_LESS:PF_ArbCompare_MORE;
  return PF_Err_NONE;
 }
 case PF_Arbitrary_PRINT_SIZE_FUNC:
  *e->u.print_size_func_params.print_sizePLu=sizeof(Document)*2+8;
  return PF_Err_NONE;
 case PF_Arbitrary_PRINT_FUNC:{
  const auto& u=e->u.print_func_params;
  if(!u.print_bufferPC||u.print_sizeLu<sizeof(Document)*2+8)return PF_Err_BAD_CALLBACK_PARAM;
  Document d{};if(!read(in_data,u.arbH,d))return PF_Err_BAD_CALLBACK_PARAM;
  auto* buf=u.print_bufferPC;
  std::memcpy(buf,"FLENS1:",7);
  const auto* src=reinterpret_cast<const uint8_t*>(&d);
  for(size_t i=0;i<sizeof(Document);i++){
   buf[7+i*2]=hex[src[i]>>4];buf[8+i*2]=hex[src[i]&15];
  }
  buf[7+sizeof(Document)*2]=0;return PF_Err_NONE;
 }
 case PF_Arbitrary_SCAN_FUNC:{
  const auto& u=e->u.scan_func_params;
  const size_t required=7+sizeof(Document)*2;
  if(!u.bufPC||u.bytes_to_scanLu<required||
     std::memcmp(u.bufPC,"FLENS1:",7)!=0)return PF_Err_BAD_CALLBACK_PARAM;
  Document d{};auto* dst=reinterpret_cast<uint8_t*>(&d);
  auto nib=[](char c)->int{
   if(c>='0'&&c<='9')return c-'0';
   if(c>='A'&&c<='F')return c-'A'+10;
   if(c>='a'&&c<='f')return c-'a'+10;
   return -1;
  };
  for(size_t i=0;i<sizeof(Document);i++){
   int a=nib(u.bufPC[7+i*2]),b=nib(u.bufPC[8+i*2]);
   if(a<0||b<0)return PF_Err_BAD_CALLBACK_PARAM;
   dst[i]=uint8_t((a<<4)|b);
  }
  return valid(d)?alloc(in_data,u.arbPH,d):PF_Err_BAD_CALLBACK_PARAM;
 }
 default:return PF_Err_NONE;
 }
}
PF_Err event(PF_InData* in,PF_OutData* out,PF_ParamDef* params[],
             int index,int source_index,PF_EventExtra* e){
 // AE invokes non-paint context events without requiring an edit document.
 if(!in||!out||!e)return PF_Err_BAD_CALLBACK_PARAM;
 if(e->e_type!=PF_Event_DRAW && e->e_type!=PF_Event_ADJUST_CURSOR &&
    e->e_type!=PF_Event_DO_CLICK && e->e_type!=PF_Event_DRAG)
  return PF_Err_NONE;
 if(!e->contextH||!*e->contextH||
    (*e->contextH)->w_type!=PF_Window_EFFECT ||
    e->window_union.effect_win.area!=PF_EA_CONTROL)return PF_Err_NONE;
 if(!params||!params[index]){
  if(e->e_type==PF_Event_DRAW)return draw(in,out,Document{},e);
  return PF_Err_NONE;
 }
 if(e->e_type!=PF_Event_DRAW&&!params[source_index])return PF_Err_NONE;
 Document doc{};
 if(!read(in,params[index]->u.arb_d.value,doc)){
  // Never pass corrupted geometry to Drawbot. Paint a safe placeholder.
  if(e->e_type!=PF_Event_DRAW)return PF_Err_NONE;
  doc={};
 }
 if(e->e_type==PF_Event_DRAW)return draw(in,out,doc,e);
 if(e->e_type==PF_Event_ADJUST_CURSOR){
  e->u.adjust_cursor.set_cursor=PF_Cursor_CROSSHAIRS;return PF_Err_NONE;
 }
 if(e->e_type!=PF_Event_DO_CLICK&&e->e_type!=PF_Event_DRAG)return PF_Err_NONE;
 const auto& frame=e->window_union.effect_win.current_frame;
 auto lens=make_lens(doc);
 Layout L(frame,lens);
 const auto p=e->u.do_click.screen_point;
 const float x=float(p.h),y=float(p.v);
 if(e->e_type==PF_Event_DO_CLICK){
  if(y>=L.top+2.f&&y<L.top+23.f&&
     x>=L.left+L.width-113.f&&x<=L.left+L.width-5.f){
   const float local=x-(L.left+L.width);
   bool mutated=false,selected=false;
   if(local>=-113.f&&local< -88.f){
    doc.selected=(doc.selected+doc.count-1)%doc.count;selected=true;
   }else if(local>=-86.f&&local<-61.f){
    doc.selected=(doc.selected+1)%doc.count;selected=true;
   }else if(local>=-58.f&&local<-33.f){
    mutated=add_after(doc,int(doc.selected));
   }else if(local>=-30.f&&local<-5.f){
    mutated=remove(doc,int(doc.selected));
   }
   if((mutated||selected)&&store(in,params[index]->u.arb_d.value,doc)){
    params[index]->uu.change_flags|=PF_ChangeFlag_CHANGED_VALUE;
    if(mutated){
     params[source_index]->u.pd.value=2;
     params[source_index]->uu.change_flags|=PF_ChangeFlag_CHANGED_VALUE;
     out->out_flags|=PF_OutFlag_FORCE_RERENDER;
    }
    dirty(in,out,e);
    e->evt_out_flags|=mutated?PF_EO_ALWAYS_UPDATE:PF_EO_NEVER_UPDATE;
   }
   return PF_Err_NONE;
  }
  Mode mode=property(x,y,L);
  if(mode==None){
   int hit=closest(doc,L,x,y,mode);
   if(hit<0)return PF_Err_NONE;
   doc.selected=uint32_t(hit);
  }
  // Numeric region scrubs incrementally; only the track sets an absolute
  // value. No surprises from merely selecting a surface.
  const bool track=mode!=None&&mode!=Stop&&
       x>=L.track_start()-3.f&&x<=L.track_end()+3.f;
  const Document before_click=doc;
  if(mode==Stop)doc.elements[doc.selected].stop^=1;
  else if(track){
   if(!apply_track(doc,mode,
       (x-L.track_start())/std::max(1.f,L.track_end()-L.track_start())))
    doc=before_click;
  }
  const bool edited=std::memcmp(&doc,&before_click,sizeof(doc))!=0;
  if(!store(in,params[index]->u.arb_d.value,doc))return PF_Err_BAD_CALLBACK_PARAM;
  if(mode!=None&&mode!=Stop){
   e->u.do_click.continue_refcon[0]=A_intptr_t(doc.selected);
   e->u.do_click.continue_refcon[1]=A_intptr_t(int(mode)|(track?0x100:0));
   e->u.do_click.continue_refcon[2]=A_intptr_t(p.h);
   e->u.do_click.continue_refcon[3]=A_intptr_t(p.v);
   e->u.do_click.send_drag=TRUE;
  }
  // Merely selecting a surface does not switch away from the catalog.
  // Only an optical edit changes the rendered lens.
  params[index]->uu.change_flags|=PF_ChangeFlag_CHANGED_VALUE;
  if(mode==Stop||(track&&edited)){
   params[source_index]->u.pd.value=2;
   params[source_index]->uu.change_flags|=PF_ChangeFlag_CHANGED_VALUE;
  }
  dirty(in,out,e);
  e->evt_out_flags|=(mode==Stop||(track&&edited))?PF_EO_ALWAYS_UPDATE:PF_EO_NEVER_UPDATE;
  return PF_Err_NONE;
 }
 auto& drag=e->u.do_click;
 const int sel=int(drag.continue_refcon[0]);
 const int packed=int(drag.continue_refcon[1]);
 const Mode mode=Mode(packed&0xff);
 const bool absolute=(packed&0x100)!=0;
 const int prevx=int(drag.continue_refcon[2]),prevy=int(drag.continue_refcon[3]);
 if(sel<0||sel>=int(doc.count)||mode<Radius||mode>Focal)return PF_Err_NONE;
 const float dx=float(p.h-prevx),dy=float(p.v-prevy);
 drag.continue_refcon[2]=p.h;drag.continue_refcon[3]=p.v;
 auto& s=doc.elements[sel];
 const Document before=doc;
 if(absolute){
  if(!apply_track(doc,mode,
     (x-L.track_start())/std::max(1.f,L.track_end()-L.track_start())))
   doc=before;
 }else switch(mode){
 case Radius:{
  const float next=s.radius+dx*.5f;
  change_radius(doc,sel,next);
 }break;
 case Thickness:change_thickness(doc,sel,s.thickness+dx/std::max(.01f,L.scale));break;
 case Position:{
  // Move the selected optical vertex by redistributing adjacent gaps.
  // Keep following vertices and the sensor plane fixed.
  const float delta=dx/std::max(.01f,L.scale);
  if(sel>0){
   auto& left=doc.elements[sel-1].thickness;
   auto& right=doc.elements[sel].thickness;
   const float low=std::max(.001f-left,right-1000.f);
   const float high=std::min(1000.f-left,right-.001f);
   if(low<=high){const float d=std::clamp(delta,low,high);
    left+=d;right-=d;}
  }else change_thickness(doc,0,s.thickness+delta);
 }break;
 case Aperture:{
  // The inspector is a horizontal numeric scrubber; the optical rim
  // itself is a vertical direct-manipulation handle.
  const bool inspector=prevy>=L.top+kRowsStart;
  change_aperture(doc,sel,s.semi+
                  (inspector?dx*.12f:-dy*L.ym/43.f));
 }break;
 case Ior:s.ior=std::clamp(s.ior+dx*.002f,.95f,3.5f);break;
 case Abbe:s.abbe=std::clamp(s.abbe+dx*.3f,0.f,150.f);break;
 case Coating:s.coating=std::clamp(s.coating+int(std::round(dx/6.f)),0,32);break;
 case Focal:doc.focal=std::clamp(doc.focal+dx*.2f,.1f,2000.f);break;
 default:break;
 }
 if(valid(doc)&&std::memcmp(&before,&doc,sizeof(doc))!=0&&
    store(in,params[index]->u.arb_d.value,doc)){
  params[index]->uu.change_flags|=PF_ChangeFlag_CHANGED_VALUE;
  params[source_index]->u.pd.value=2;
  params[source_index]->uu.change_flags|=PF_ChangeFlag_CHANGED_VALUE;
  dirty(in,out,e);
 }
 if(drag.last_time){e->evt_out_flags|=PF_EO_ALWAYS_UPDATE;
                    out->out_flags|=PF_OutFlag_FORCE_RERENDER;}
 else e->evt_out_flags|=PF_EO_NEVER_UPDATE;
 return PF_Err_NONE;
}
} // namespace fl::custom::ae
