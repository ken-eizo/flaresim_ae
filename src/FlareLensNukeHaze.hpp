#pragma once
// Exact original Nuke source-block coverage, including the last floor(w/ds)
// tile, which absorbs right/bottom remainder pixels (not just ds*ds pixels).
#include "FlareLensCore.hpp"
#include <algorithm>
#include <cmath>
namespace fl {
struct NukeHazeRect {int x0=0,y0=0,x1=0,y1=0;};
inline NukeHazeRect nuke_haze_rect(const Source& source,
 int width,int height,int scan_step) noexcept {
 if(width<=0||height<=0)return {};
 const int f=std::clamp(scan_step,1,32);
 const int cx=int(source.x*width),cy=int(source.y*height);
 if(cx<0||cy<0||cx>=width||cy>=height)return {};
 const int dw=std::max(1,width/f),dh=std::max(1,height/f);
 const int bx=std::min(cx/f,dw-1),by=std::min(cy/f,dh-1);
 const int x0=bx*f,y0=by*f;
 const int x1=bx==dw-1?width:std::min(width,x0+f);
 const int y1=by==dh-1?height:std::min(height,y0+f);
 return {x0,y0,x1,y1};
}
}
