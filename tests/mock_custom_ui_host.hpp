#pragma once
// SDK callback emulation for AEX tests. Never starts or interacts with AE.
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#ifndef PF_USE_NEW_WINDOW_UNION
#define PF_USE_NEW_WINDOW_UNION
#endif
#include "AE_EffectUI.h"
#include <cstdlib>
#include <cstring>
#include <cstdint>
namespace fl_mock {
struct Handle {void* data;std::uint64_t size;};
inline PF_Handle new_handle(A_u_longlong size){
 if(size==0||size>(1u<<20))return nullptr;
 auto* h=static_cast<Handle*>(std::malloc(sizeof(Handle)));
 if(!h)return nullptr;
 h->data=std::calloc(1,size_t(size));h->size=size;
 if(!h->data){std::free(h);return nullptr;}
 return reinterpret_cast<PF_Handle>(h);
}
inline void* lock(PF_Handle h){
 return h?reinterpret_cast<Handle*>(h)->data:nullptr;
}
inline void unlock(PF_Handle){}
inline A_u_longlong size(PF_Handle h){
 return h?reinterpret_cast<Handle*>(h)->size:0;
}
inline void dispose(PF_Handle h){
 if(h){std::free(reinterpret_cast<Handle*>(h)->data);std::free(h);}
}
inline PF_Err register_ui(PF_ProgPtr,PF_CustomUIInfo* info){
 return info&&(info->events&PF_CustomEFlag_EFFECT)?
   PF_Err_NONE:PF_Err_BAD_CALLBACK_PARAM;
}
inline PF_UtilCallbacks callbacks{};
inline void configure(PF_InData& in){
 callbacks.host_new_handle=&new_handle;
 callbacks.host_lock_handle=&lock;
 callbacks.host_unlock_handle=&unlock;
 callbacks.host_get_handle_size=&size;
 callbacks.host_dispose_handle=&dispose;
 in.utils=&callbacks;
 in.inter.register_ui=&register_ui;
}
inline void activate(PF_ParamDef& d){
 if(d.param_type==PF_Param_ARBITRARY_DATA)
  d.u.arb_d.value=d.u.arb_d.dephault;
}
} // namespace fl_mock
