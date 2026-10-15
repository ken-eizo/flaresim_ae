#define NOMINMAX
#include "FlareLensCatalogPicker.hpp"
#include "FlareLensCatalog.hpp"
#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

namespace fl::catalog {
namespace {
constexpr int kDialog=16001,kSearch=16002,kList=16003;
struct Picker {
 int selected=1;
 std::vector<int> mapping;
};
std::wstring to_wide(const char* ascii){
 std::wstring out;
 if(ascii)for(const auto* p=ascii;*p;p++)
  out.push_back(wchar_t(static_cast<unsigned char>(*p)));
 return out;
}
std::string normalized(std::wstring value){
 std::string r;
 r.reserve(value.size());
 for(const wchar_t c:value)
  r.push_back(char(std::tolower(static_cast<unsigned char>(c))));
 return r;
}
void populate(HWND dlg,Picker& picker){
 wchar_t query[255]{};
 GetDlgItemTextW(dlg,kSearch,query,255);
 const std::string filter=normalized(query);
 HWND list=GetDlgItem(dlg,kList);
 SendMessageW(list,WM_SETREDRAW,FALSE,0);
 SendMessageW(list,LB_RESETCONTENT,0,0);
 picker.mapping.clear();
 int active=-1;
 for(int index=1;index<=kCount;index++){
  const char* name=filename(index);
  if(!name)continue;
  std::string folded(name);
  for(char& c:folded)
   c=char(std::tolower(static_cast<unsigned char>(c)));
  if(!filter.empty()&&folded.find(filter)==std::string::npos)continue;
  picker.mapping.push_back(index);
  const auto wide=to_wide(name);
  const LRESULT row=SendMessageW(list,LB_ADDSTRING,0,
                                   reinterpret_cast<LPARAM>(wide.c_str()));
  if(row>=0)SendMessageW(list,LB_SETITEMDATA,WPARAM(row),index);
  if(index==picker.selected)active=int(row);
 }
 if(active<0&& !picker.mapping.empty())active=0;
 if(active>=0)SendMessageW(list,LB_SETCURSEL,active,0);
 SendMessageW(list,WM_SETREDRAW,TRUE,0);
 InvalidateRect(list,nullptr,TRUE);
}
INT_PTR CALLBACK proc(HWND dlg,UINT message,WPARAM wp,LPARAM lp){
 auto* state=reinterpret_cast<Picker*>(GetWindowLongPtrW(dlg,DWLP_USER));
 switch(message){
  case WM_INITDIALOG:{
   state=reinterpret_cast<Picker*>(lp);
   SetWindowLongPtrW(dlg,DWLP_USER,reinterpret_cast<LONG_PTR>(state));
   populate(dlg,*state);
   SetFocus(GetDlgItem(dlg,kSearch));
   return FALSE;
  }
  case WM_COMMAND:{
   const int id=LOWORD(wp),note=HIWORD(wp);
   if(id==kSearch&&note==EN_CHANGE&&state){
    populate(dlg,*state);return TRUE;
   }
   if(id==IDOK||(id==kList&&note==LBN_DBLCLK)){
    HWND list=GetDlgItem(dlg,kList);
    const LRESULT row=SendMessageW(list,LB_GETCURSEL,0,0);
    if(row!=LB_ERR&&state){
     const LRESULT value=SendMessageW(list,LB_GETITEMDATA,WPARAM(row),0);
     if(value>=1&&value<=kCount){
      state->selected=int(value);
      EndDialog(dlg,IDOK);return TRUE;
     }
    }
    return TRUE;
   }
   if(id==IDCANCEL){EndDialog(dlg,IDCANCEL);return TRUE;}
   break;
  }
 }
 return FALSE;
}
HMODULE self_module(){
 HMODULE h=nullptr;
 GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
   reinterpret_cast<LPCWSTR>(&self_module),&h);
 return h;
}
}
bool choose_embedded_lens(HWND owner,int current,int& chosen){
 Picker picker{};
 picker.selected=std::clamp(current,1,kCount);
 const auto m=self_module();
 if(!m)return false;
 const INT_PTR r=DialogBoxParamW(m,MAKEINTRESOURCEW(kDialog),owner,
                                   &proc,reinterpret_cast<LPARAM>(&picker));
 if(r!=IDOK)return false;
 chosen=picker.selected;
 return true;
}
}
