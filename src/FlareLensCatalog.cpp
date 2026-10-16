#define NOMINMAX
#include "FlareLensCatalog.hpp"
#if defined(_WIN32)
#include <windows.h>
#else
#include "FlareLensEmbeddedMac.hpp"
#endif
#include <array>
#include <mutex>
#include <string>
#include <cstring>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace fl::catalog {
namespace {
constexpr std::array<const char*,12> kPresets={{
 "AI_Nikkor_50mm_F1.2S.lens",
 "AI_Nikkor_28mm_F1.4D.lens",
 "AI_Nikkor_35mm_F1.4S.lens",
 "Angenieux_35mm_f2.5_(Angenieux_35mm_f2.5_R1).lens",
 "Canon_51mm_F1.25_(Canon_RF_50mm_F1.2_L_USM).lens",
 "Canon_34mm_F2.05_OS_(Canon_EF35mm_f2_IS_USM).lens",
 "Canon_85mm_F1.24.lens",
 "Canon_20mm_F2.8_(Canon_EF20mm_f2.8_USM).lens",
 "Fujifilm_36mm_F1.45_(Fujifilm_Fujinon_XF35mm_F1.4_R).lens",
 "Fujifilm_24mm_F1.44_IF_(Fujifilm_Fujinon_XF23mm_F1.4_R_LM_WR).lens",
 "Cosina_49mm_F1.23_(Cosina_Nokton_50mm_F1.2).lens",
 "Hasselblad_120mm_F3.5_macro_(Hasselblad_XCD_3.5_120_Macro).lens"
}};
struct CacheEntry {
 std::once_flag once;
 LensSystem lens;
 bool valid=false;
};
std::array<CacheEntry,kCount> cache{};
#if defined(_WIN32)
HMODULE self_module() noexcept{
 HMODULE module=nullptr;
 // Resolve resource from *this AEX*, never from the AE host executable.
 const BOOL ok=GetModuleHandleExA(
  GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
  reinterpret_cast<LPCSTR>(&self_module),&module);
 return ok?module:nullptr;
}
#endif
bool valid_lens(const LensSystem& system){
 // Auxiliary converters can have focal_length: inf or an unspecified
 // focal length in the original Nuke asset. Keep the authentic data in
 // the catalog even if it cannot independently form sensor ghost pairs.
 if(system.num_surfaces()<1||system.num_surfaces()>64||
    !std::isfinite(system.sensor_z))return false;
 for(const auto& surface:system.surfaces)
  if(!std::isfinite(surface.radius)||!std::isfinite(surface.ior)||
     !std::isfinite(surface.thickness)||!std::isfinite(surface.z)||
     !std::isfinite(surface.semi_aperture)||surface.semi_aperture<=0.f)
   return false;
 return true;
}
void load_entry(int zero_based,CacheEntry& entry){
#if defined(_WIN32)
 const HMODULE m=self_module();
 if(!m)return;
 const HRSRC resource=FindResourceW(m,
     MAKEINTRESOURCEW(kResourceBase+zero_based),MAKEINTRESOURCEW(10));
 if(!resource)return;
 const DWORD length=SizeofResource(m,resource);
 if(!length)return;
 const HGLOBAL data=LoadResource(m,resource);
 if(!data)return;
 const void* content=LockResource(data);
 if(!content)return;
 const char* text=static_cast<const char*>(content);
#else
 const auto embedded=embedded::get(zero_based);
 if(!embedded.data||!embedded.size)return;
 const char* text=embedded.data;
 const auto length=embedded.size;
#endif
 LensSystem parsed{};
 if(!parsed.load_from_memory(text,length)||
    !valid_lens(parsed))return;
 entry.lens=std::move(parsed);
 entry.valid=true;
}
std::uint64_t hash_value(std::uint64_t hash,std::uint32_t value){
 hash^=std::uint64_t(value);
 return hash*1099511628211ULL;
}
}

const LensSystem* lens_by_index(int id) noexcept{
 if(id<1||id>kCount)return nullptr;
 auto& c=cache[size_t(id-1)];
 try{
  std::call_once(c.once,[&](){load_entry(id-1,c);});
 }catch(...){return nullptr;}
 return c.valid?&c.lens:nullptr;
}
const char* filename(int id) noexcept{
 return id>=1&&id<=kCount?kNames[size_t(id-1)]:nullptr;
}
int index_for_filename(const char* name) noexcept{
 if(!name||!name[0])return 0;
 // The upstream filenames (including parentheses) are exact catalog keys.
 for(int i=0;i<kCount;i++)
  if(std::strcmp(name,kNames[size_t(i)])==0)return i+1;
 return 0;
}
int quick_preset_index(int preset) noexcept{
 if(preset<1||preset>int(kPresets.size()))return 0;
 return index_for_filename(kPresets[size_t(preset-1)]);
}
std::uint64_t geometry_hash(const LensSystem& l) noexcept{
 auto hash=14695981039346656037ULL;
 hash=hash_value(hash,std::bit_cast<std::uint32_t>(l.focal_length));
 hash=hash_value(hash,std::bit_cast<std::uint32_t>(l.sensor_z));
 for(const auto& s:l.surfaces){
  hash=hash_value(hash,std::bit_cast<std::uint32_t>(s.radius));
  hash=hash_value(hash,std::bit_cast<std::uint32_t>(s.thickness));
  hash=hash_value(hash,std::bit_cast<std::uint32_t>(s.ior));
  hash=hash_value(hash,std::bit_cast<std::uint32_t>(s.abbe_v));
  hash=hash_value(hash,std::bit_cast<std::uint32_t>(s.semi_aperture));
  hash=hash_value(hash,std::uint32_t(s.coating));
  hash=hash_value(hash,std::uint32_t(s.is_stop));
 }
 return hash;
}
}

extern "C" FLARELENS_EXPORT
int FlareLensQuickPresetCatalogID(int quick_preset){
 return fl::catalog::quick_preset_index(quick_preset);
}

extern "C" FLARELENS_EXPORT
int FlareLensInspectCatalog(int id,fl::catalog::Report* out){
 if(!out||out->size!=sizeof(fl::catalog::Report))return 0;
 fl::catalog::Report report{};
 report.count=fl::catalog::kCount;
 report.index=id;
 if(const auto* name=fl::catalog::filename(id))
  std::snprintf(report.filename,sizeof(report.filename),"%s",name);
 if(const auto* lens=fl::catalog::lens_by_index(id)){
  report.valid=1;
  report.renderable=lens->num_surfaces()>=2&&
     std::isfinite(lens->focal_length)&&lens->focal_length>0;
  report.surface_count=lens->num_surfaces();
  report.focal_length=lens->focal_length;
  report.sensor_z=lens->sensor_z;
  report.geometry_hash=fl::catalog::geometry_hash(*lens);
 }
 *out=report;
 return report.valid;
}
