#include "AEConfig.h"
#include "AE_EffectVers.h"
#ifndef AE_OS_WIN
#include "AE_General.r"
#endif

// Match Name and parameter IDs are identical to Windows v1.0.0.
// Metal GPU render is advertised by both PiPL and PF_Cmd_GLOBAL_SETUP.
resource 'PiPL' (16000) {
 { Kind { AEEffect },
   Name { "FlareSimAE" },
   Category { "Effects" },
#if defined(AE_OS_MAC)
   CodeMacIntel64 { "EffectMain" },
   CodeMacARM64 { "EffectMain" },
#endif
   AE_PiPL_Version { 2, 0 },
   AE_Effect_Spec_Version { PF_PLUG_IN_VERSION, PF_PLUG_IN_SUBVERS },
   AE_Effect_Version { 536064 },
   AE_Effect_Info_Flags { 0 },
   AE_Effect_Global_OutFlags { 0x02008200 },
   AE_Effect_Global_OutFlags_2 { 0x0A001480 },
   AE_Effect_Match_Name { "FlareLensAE_v1" },
   AE_Reserved_Info { 0 }
 }
};
