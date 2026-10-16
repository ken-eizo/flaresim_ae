#include "FlareLensEmbeddedMac.hpp"
#include "FlareLensCatalogGenerated.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <cstring>

int main(){
 namespace fs=std::filesystem;
 int count=0;
 for(int i=0;i<fl::catalog::kCount;i++){
  const auto entry=fl::catalog::embedded::get(i);
  if(!entry.data||!entry.size)return 1;
  // Windows needs UTF-8 conversion for native filename lookup. POSIX paths
  // already use UTF-8 bytes, without deprecated std::filesystem::u8path.
#if defined(_WIN32)
  const auto path=fs::path(FLARELENS_TEST_SOURCE_DIR) /
      fs::u8path(fl::catalog::kNames[size_t(i)]);
#else
  const auto path=fs::path(FLARELENS_TEST_SOURCE_DIR) /
      fs::path(fl::catalog::kNames[size_t(i)]);
#endif
  std::ifstream stream(path,std::ios::binary);
  if(!stream){std::cerr<<"Cannot read "<<i<<"\n";return 2;}
  const std::string expected((std::istreambuf_iterator<char>(stream)),
                              std::istreambuf_iterator<char>());
  if(entry.size!=expected.size() ||
    std::memcmp(entry.data,expected.data(),entry.size)!=0){
   std::cerr<<"Embedded catalog mismatch: "<<i<<"\n";return 3;
  }
  count++;
 }
 if(fl::catalog::embedded::get(-1).data ||
    fl::catalog::embedded::get(fl::catalog::kCount).data)return 4;
 std::cout<<"MAC_EMBEDDED_CATALOG_BYTES_MATCH_ALL "<<count<<"\n";
 return 0;
}
