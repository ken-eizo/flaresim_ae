#import <AppKit/AppKit.h>
#include "FlareLensCatalogPicker.hpp"
#include "FlareLensCatalog.hpp"
#include <algorithm>
#include <cstdlib>
#include <string>
namespace fl::catalog {
bool choose_embedded_lens(FlareLensNativeWindow, int current_index, int& chosen_index){
 // Only called from AE PF_Cmd_USER_CHANGED_PARAM on the UI thread.
 // Cocoa builds use an editable completion-enabled combo rather than
 // Windows DialogBox. All 1370 entries are supplied by the same catalog.
 if (![NSThread isMainThread])return false;
 @autoreleasepool {
  NSAlert* alert=[[NSAlert alloc] init];
  [alert setMessageText:@"FlareSimAE — Select Lens"];
  [alert setInformativeText:@"Type a lens name or catalog ID, then click Select."];
  [alert addButtonWithTitle:@"Select"];
  [alert addButtonWithTitle:@"Cancel"];
  NSComboBox* combo=[[NSComboBox alloc] initWithFrame:NSMakeRect(0,0,470,28)];
  [combo setUsesDataSource:NO];
  [combo setCompletes:YES];
  [combo setNumberOfVisibleItems:20];
  for(int i=1;i<=kCount;i++){
   const char* name=filename(i);
   if(name)[combo addItemWithObjectValue:
     [NSString stringWithUTF8String:name]];
  }
  if(const char* name=filename(std::clamp(current_index,1,kCount))){
   [combo setStringValue:[NSString stringWithUTF8String:name]];
  }
  [alert setAccessoryView:combo];
  if([alert runModal]!=NSAlertFirstButtonReturn)return false;
  const char* chosen=[combo.stringValue UTF8String];
  if(!chosen)return false;
  int id=index_for_filename(chosen);
  if(!id){
   char* end=nullptr;
   const long value=std::strtol(chosen,&end,10);
   if(end&&*end==0&&value>=1&&value<=kCount)id=int(value);
  }
  if(!id)return false;
  chosen_index=id;
  return true;
 }
}
}
