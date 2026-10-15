#include "FlareLensCore.hpp"
#include <array>
#include <filesystem>
#include <iostream>
#include <cmath>
#include <string>

int main(){
 const auto root=std::filesystem::path(__FILE__).parent_path().parent_path()/
     "upstream/lenses/lens_files";
 constexpr std::array<const char*,12> names={{
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
 for(size_t i=0;i<names.size();i++){
  auto pathname=(root/names[i]).string();
  LensSystem lens;
  if(!lens.load(pathname.c_str())||lens.num_surfaces()<2||
     lens.num_surfaces()>64||!(lens.focal_length>0)||
     !std::isfinite(lens.sensor_z)){
   std::cerr<<"INVALID_LENS preset="<<i+1<<" file="<<names[i]<<"\n";
   return int(i)+1;
  }
  fl::Settings s{};
  s.grid=4;s.max_pairs=6;s.gain=1000;
  const auto image=fl::render_cpu(lens,{{.4f,.25f,100,100,100}},s,96,64);
  if(image.rgb.size()!=96u*64*3)return 30+int(i);
  for(float v:image.rgb)if(!std::isfinite(v)||v<0)return 60+int(i);
  std::cout<<"LENS_PRESET_VALID id="<<i+1<<" surfaces="
           <<lens.num_surfaces()<<" mm="<<lens.focal_length
           <<" name="<<lens.name<<"\n";
 }
 std::cout<<"ALL_12_NUKE_OPTICAL_LENS_CATALOG_PASS\n";
}
