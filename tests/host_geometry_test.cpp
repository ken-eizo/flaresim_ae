#include "FlareLensHostGeometry.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
int main(){
 // Ordinary full-frame, full-quality AE smart and legacy worlds.
 auto a=fl::host_geometry(1920,1080,1,1,1,1,0,0,1920,1080);
 if(!a.valid()||a.full_w!=1920||a.full_h!=1080||
    std::fabs(a.normalized_x(960)-.5f)>1e-6f)return 1;
 // ROI 400x200 at layer-origin (1300,700) must not redefine the sensor.
 auto r=fl::host_geometry(1920,1080,1,1,1,1,1300,700,400,200);
 if(r.full_w!=1920||r.full_h!=1080||
    r.global_x(0)!=1300||r.global_y(0)!=700||
    std::fabs(r.normalized_x(960)-.5f)>1e-6f)return 2;
 // Half-res SmartFX output: AE hands the Point Param over in the Half
 // world's pixels (960,540 -> 480,270), as After Effects itself does.
 auto half=fl::host_geometry(1920,1080,1,2,1,2,0,0,960,540);
 if(half.full_w!=960||half.full_h!=540||
    std::fabs(half.normalized_x(480)-.5f)>1e-6f||
    std::fabs(half.normalized_y(270)-.5f)>1e-6f)return 3;
 // Same half-res comp, subrect checkout, stage coordinates still global.
 auto tile=fl::host_geometry(1920,1080,1,2,1,2,440,120,320,200);
 if(tile.full_w!=960||tile.full_h!=540||tile.global_x(0)!=440||
    std::fabs(tile.normalized_x(480)-.5f)>1e-6f)return 4;
 // Third of a layer that does not divide by three: the world rounds to 333
 // pixels, the point keeps its fraction of the layer.
 auto third=fl::host_geometry(1000,1000,1,3,1,3,0,0,333,333);
 if(third.full_w!=333||
    std::fabs(third.normalized_x(370.f/3.f)-.37f)>1e-6f)return 7;
 // A minimal test host may not initialize downsample rational.
 auto fallback=fl::host_geometry(1920,1080,0,0,0,0,0,0,1920,1080);
 if(fallback.full_w!=1920||fallback.full_h!=1080)return 5;
 // Low memory ROI includes rendering to sensor but prevents size inference
 // from the checkout buffer alone.
 auto smaller=fl::host_geometry(0,0,0,0,0,0,100,200,300,240);
 if(smaller.full_w!=400||smaller.full_h!=440)return 6;
 std::cout<<"AE ROI + downsample + point-coordinate geometry passed\n";
 return 0;
}
