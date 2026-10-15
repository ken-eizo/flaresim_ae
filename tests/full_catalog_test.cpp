// Validate EVERY shipped original Nuke .lens resource inside the built AEX.
// Reject any silent fallback: both filename and full surface-geometry hash
// must exactly match the independent parser loading the upstream disk file.
#define NOMINMAX
#include <windows.h>
#include "FlareLensCatalog.hpp"
#include "FlareLensCore.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

using Inspect=int(*)(int,fl::catalog::Report*);
using InspectQuick=int(*)(int);
static std::uint64_t hash_file_geometry(const LensSystem& l){
 // Independent hash: does not call the AEX catalog diagnostic hash.
 // FNV-1a mixed over stored geometric surface values.
 std::uint64_t h=14695981039346656037ULL;
 const auto mix=[&](float v){
  std::uint32_t bits{};
  std::memcpy(&bits,&v,sizeof(bits));
  h=(h^bits)*1099511628211ULL;
 };
 mix(l.focal_length);mix(l.sensor_z);
 for(const auto& s:l.surfaces){
  mix(s.radius);mix(s.thickness);mix(s.ior);mix(s.abbe_v);
  mix(s.semi_aperture);
  h=(h^std::uint32_t(s.coating))*1099511628211ULL;
  h=(h^std::uint32_t(s.is_stop))*1099511628211ULL;
 }
 return h;
}
// The geometry test alone could miss a parser bug shared by disk and RCDATA.
// Compare the actual embedded payload BYTE-FOR-BYTE against each Nuke file.
static bool exact_embedded_bytes(HMODULE module,int id,
                                 const std::filesystem::path& file_path){
 const HRSRC resource=FindResourceW(module,
   MAKEINTRESOURCEW(fl::catalog::kResourceBase+id-1),MAKEINTRESOURCEW(10));
 if(!resource)return false;
 const DWORD length=SizeofResource(module,resource);
 const HGLOBAL data=LoadResource(module,resource);
 if(!length||!data)return false;
 const void* bytes=LockResource(data);
 if(!bytes)return false;
 std::ifstream source(file_path,std::ios::binary|std::ios::ate);
 if(!source)return false;
 const std::streamoff size=source.tellg();
 if(size<0||static_cast<std::uint64_t>(size)!=length)return false;
 source.seekg(0,std::ios::beg);
 std::vector<char> original(length);
 if(!source.read(original.data(),std::streamsize(length)))return false;
 return std::memcmp(bytes,original.data(),length)==0;
}
int main(){
 wchar_t executable[MAX_PATH]{};
 if(!GetModuleFileNameW(nullptr,executable,MAX_PATH))return 1;
 const auto plugin=std::filesystem::path(executable).parent_path()/
                   L"FlareLensAE.aex";
 const HMODULE module=LoadLibraryW(plugin.c_str());
 if(!module){std::wcerr<<L"FAILED_LOAD_AEX "<<plugin<<L"\n";return 2;}
 const auto inspect=reinterpret_cast<Inspect>(
   GetProcAddress(module,"FlareLensInspectCatalog"));
 if(!inspect)return 3;
 const auto quick=reinterpret_cast<InspectQuick>(
    GetProcAddress(module,"FlareLensQuickPresetCatalogID"));
 if(!quick)return 5;
 constexpr int expected_count=1370;
 if(fl::catalog::kCount!=expected_count)return 4;
 const auto root=std::filesystem::path(__FILE__).parent_path().parent_path()/
     "upstream/lenses/lens_files";
 // Exercise competing first-time loads of the SAME and DIFFERENT lens IDs.
 // This catches call_once / MFR cache races before the serial full audit.
 constexpr std::array<int,16> stress_ids={{
   1,2,12,99,201,388,501,630,701,832,944,1051,1199,1327,1369,1370}};
 std::atomic<int> concurrent_failures{0};
 std::vector<std::thread> workers;
 for(int thread_id=0;thread_id<12;thread_id++)
  workers.emplace_back([&,thread_id]{
   for(int j=0;j<48;j++){
    const int id=stress_ids[size_t((j+thread_id*3)%stress_ids.size())];
    fl::catalog::Report record{};
    if(!inspect(id,&record)||!record.valid||record.index!=id||
       record.surface_count<1||!record.geometry_hash||
       std::strcmp(record.filename,fl::catalog::kNames[size_t(id-1)]))
      concurrent_failures.fetch_add(1,std::memory_order_relaxed);
   }
  });
 for(auto& worker:workers)worker.join();
 if(concurrent_failures.load()!=0){
  std::cerr<<"CONCURRENT_CATALOG_FIRST_LOAD_FAILURES="
           <<concurrent_failures.load()<<"\n";
  return 21;
 }
 size_t successful=0,unrenderable=0,resource_failures=0,
        geometry_failures=0,raw_data_failures=0;
 std::set<std::uint64_t> fingerprints;
 std::uint64_t nikkor_hash=0;
 for(int id=1;id<=expected_count;id++){
  fl::catalog::Report report{};
  if(!inspect(id,&report)||report.count!=expected_count||
     report.index!=id||!report.valid||
     std::strcmp(report.filename,fl::catalog::kNames[size_t(id-1)])!=0){
   ++resource_failures;
   if(resource_failures<=25)
    std::cerr<<"EMBEDDED_RESOURCE_LOAD_FAILED id="<<id
             <<" name="<<fl::catalog::kNames[size_t(id-1)]<<"\n";
   continue;
  }
  LensSystem original;
  const auto file_path=root/fl::catalog::kNames[size_t(id-1)];
  if(!exact_embedded_bytes(module,id,file_path)){
   ++raw_data_failures;
   if(raw_data_failures<=25)
    std::cerr<<"EMBEDDED_RESOURCE_BYTE_MISMATCH id="<<id
             <<" name="<<report.filename<<"\n";
   continue;
  }
  const auto path=file_path.string();
  if(!original.load(path.c_str())||
     report.surface_count!=original.num_surfaces()||
     report.renderable!=int(original.num_surfaces()>=2&&
       std::isfinite(original.focal_length)&&original.focal_length>0)||
     report.focal_length!=original.focal_length||
     report.sensor_z!=original.sensor_z||
     report.geometry_hash!=hash_file_geometry(original)){
   ++geometry_failures;
   if(geometry_failures<=25)
    std::cerr<<"EMBEDDED_RESOURCE_NOT_IDENTICAL id="<<id
             <<" name="<<report.filename<<"\n";
   continue;
  }
  unrenderable+=!report.renderable;
  fingerprints.insert(report.geometry_hash);
  if(std::strcmp(report.filename,"AI_Nikkor_50mm_F1.2S.lens")==0)
   nikkor_hash=report.geometry_hash;
  ++successful;
 }
 std::cout<<"ORIGINAL_NUKE_CATALOG_AUDIT valid="<<successful
  <<" unrenderable_originals="<<unrenderable
  <<" resource_failures="<<resource_failures
  <<" raw_byte_failures="<<raw_data_failures
  <<" geometry_failures="<<geometry_failures
  <<" concurrent_first_load_failures="<<concurrent_failures.load()<<"\n";
 if(resource_failures||raw_data_failures||geometry_failures)return 10;
 fl::catalog::Report missing{};
 if(inspect(0,&missing)||inspect(expected_count+1,&missing))return 12;
 if(!nikkor_hash)return 13;
 for(const char* name:{
  "Canon_51mm_F1.25_(Canon_RF_50mm_F1.2_L_USM).lens",
  "Canon_85mm_F1.24.lens",
  "Fujifilm_36mm_F1.45_(Fujifilm_Fujinon_XF35mm_F1.4_R).lens",
  "Hasselblad_120mm_F3.5_macro_(Hasselblad_XCD_3.5_120_Macro).lens"}){
  bool found=false;
  for(int id=1;id<=expected_count;id++){
   if(std::strcmp(fl::catalog::kNames[size_t(id-1)],name))continue;
   fl::catalog::Report report{};
   if(!inspect(id,&report)||report.geometry_hash==nikkor_hash){
    std::cerr<<"SILENT_NIKKOR_FALLBACK="<<name<<"\n";return 14;
   }
   found=true;break;
  }
  if(!found)return 15;
 }
 std::cout<<"ORIGINAL_NUKE_LENSES_EMBEDDED="<<successful
          <<" UNIQUE_SURFACE_GEOMETRIES="<<fingerprints.size()
          <<" NIKKOR_FALLBACKS=0\n";
 if(fingerprints.size()<100)return 16;
 // Verify the complete retained quick-preset mapping in the actual AEX,
 // including all eight models that previously fell back to Nikkor.
 constexpr const char* preset_names[12]={
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
 };
 if(quick(0)!=0||quick(13)!=0)return 17;
 for(int preset=1;preset<=12;preset++){
  const int catalog_id=quick(preset);
  if(catalog_id<1||catalog_id>expected_count)return 18;
  fl::catalog::Report p{};
  if(!inspect(catalog_id,&p)||std::strcmp(p.filename,preset_names[preset-1]))
   return 19;
  if(preset>1&&p.geometry_hash==nikkor_hash){
   std::cerr<<"QUICK_PRESET_SILENT_NIKKOR_FALLBACK "<<preset<<"\n";
   return 20;
  }
 }
 std::cout<<"12_QUICK_PRESETS_MAP_TO_DISTINCT_EXPECTED_NUKE_FILES\n";
 FreeLibrary(module);
 return 0;
}
