#pragma once
#include "FlareLensCore.hpp"
#include <vector>
namespace fl {
struct PupilSample {float u=0,v=0;};
struct SpectralSample {float lambda=550,r=0,g=0,b=0;};
std::vector<PupilSample> make_pupil(const Settings& s);
std::vector<SpectralSample> make_spectrum(const Settings& s);
void box_blur_rgb(std::vector<float>& rgb,int w,int h,int radius,int passes);
void render_haze(std::vector<float>& rgb,const std::vector<Source>& sources,int w,int h,
                 float gain,float radius,int passes,int source_footprint=1);
void render_diffraction(std::vector<float>& rgb,const std::vector<Source>& sources,
                        int w,int h,int blades,float rotation,float gain,float scale);
}
