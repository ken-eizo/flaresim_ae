#pragma once
#include "AE_Effect.h"
#ifndef PF_USE_NEW_WINDOW_UNION
#define PF_USE_NEW_WINDOW_UNION
#endif
#include "AE_EffectUI.h"
#include "FlareLensCustom.hpp"
namespace fl::custom::ae {
inline constexpr int kEditorWidth=416,kEditorHeight=314;
PF_Err make_default(PF_InData*, PF_ArbitraryH*);
PF_Err arbitrary(PF_InData*,PF_OutData*,PF_ArbParamsExtra*);
PF_Err event(PF_InData*,PF_OutData*,PF_ParamDef*[],int,int,PF_EventExtra*);
bool read(PF_InData*,PF_ArbitraryH,Document&) noexcept;
bool store(PF_InData*,PF_ArbitraryH,const Document&) noexcept;
}
