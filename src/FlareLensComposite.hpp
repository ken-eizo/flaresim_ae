#pragma once
#include <algorithm>
#include <cmath>

namespace fl {
// AE receives premultiplied RGBA worlds. A rendered optical emitter can add
// light even where its input is completely transparent (e.g. an empty shape
// layer). The ray tracer returns additive RGB radiance and does not define
// coverage. Derive the *minimum representable* coverage from emitted radiance
// for ordinary AE "Over" compositing, without modifying that radiance.
//
// Fully opaque inputs remain fully opaque, and no-emission inputs keep their
// exact original alpha. This is an AE presentation adapter, NOT a change to
// FlareSim/Nuke optics. For mathematically exact additive mixing with underlying
// footage, use Flare Only as an additive layer.
inline float alpha_for_emission(float input_alpha,
                                float emitted_r,float emitted_g,float emitted_b) noexcept {
 const float a=std::isfinite(input_alpha)?
    std::clamp(input_alpha,0.f,1.f):0.f;
 const float peak=std::max({emitted_r,emitted_g,emitted_b,0.f});
 const float coverage=std::isfinite(peak)?std::clamp(peak,0.f,1.f):0.f;
 return a+(1.f-a)*coverage;
}
}
