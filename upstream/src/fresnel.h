// ============================================================================
// fresnel.h — Fresnel equations, thin-film AR coatings, glass dispersion
// ============================================================================
#pragma once

#include <cmath>
#include <algorithm>

// std::clamp was added to <algorithm> in C++17 but GCC 6 shipped an incomplete
// C++17 stdlib that lacks it.  Provide a fallback for old toolchains.
#if defined(__GNUC__) && __GNUC__ < 7 && !defined(__clang__)
namespace std {
    template<class T>
    constexpr const T& clamp(const T& v, const T& lo, const T& hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }
}
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---- Dispersion (Cauchy via Abbe number) --------------------------------

// Compute wavelength-dependent IOR from the d-line IOR and Abbe number.
// Reference wavelengths: F = 486.13 nm, d = 587.56 nm, C = 656.27 nm
inline float dispersion_ior(float n_d, float V_d, float lambda_nm)
{
    if (V_d < 0.1f || n_d <= 1.0001f)
        return n_d; // air or non-dispersive

    constexpr float lF = 486.13f;
    constexpr float lC = 656.27f;
    constexpr float ld = 587.56f;

    // n_F - n_C = (n_d - 1) / V_d
    float dn = (n_d - 1.0f) / V_d;

    // Cauchy: n(λ) = A + B/λ²
    float inv_lF2 = 1.0f / (lF * lF);
    float inv_lC2 = 1.0f / (lC * lC);
    float inv_ld2 = 1.0f / (ld * ld);

    float B = dn / (inv_lF2 - inv_lC2);
    float A = n_d - B * inv_ld2;

    return A + B / (lambda_nm * lambda_nm);
}

// ---- Fresnel equations --------------------------------------------------

// Unpolarized Fresnel reflectance at a dielectric interface.
// cos_i: cosine of incidence angle (positive).
inline float fresnel_reflectance(float cos_i, float n1, float n2)
{
    cos_i = std::abs(cos_i);
    float eta = n1 / n2;
    float sin2_t = eta * eta * (1.0f - cos_i * cos_i);

    if (sin2_t >= 1.0f)
        return 1.0f; // total internal reflection

    float cos_t = std::sqrt(1.0f - sin2_t);

    float rs = (n1 * cos_i - n2 * cos_t) / (n1 * cos_i + n2 * cos_t);
    float rp = (n2 * cos_i - n1 * cos_t) / (n2 * cos_i + n1 * cos_t);

    return 0.5f * (rs * rs + rp * rp);
}

// ---- Thin-film AR coating -----------------------------------------------

// Single-layer coating reflectance (thin-film interference).
// coating_n : coating refractive index (e.g. MgF2 = 1.38)
// d_nm      : physical coating thickness in nm
// lambda_nm : light wavelength in nm
// Unpolarized thin-film interference: calculate independent s and p
// amplitude coefficients, then average reflected intensities. The earlier
// single-polarization approximation overstated off-axis reflections.
inline float coating_reflectance(float cos_i, float n1, float n2,
                                 float coating_n, float d_nm, float lambda_nm)
{
    cos_i = std::clamp(std::abs(cos_i), 0.f, 1.f);
    if (!(n1>0&&n2>0&&coating_n>0&&lambda_nm>0))
        return 0.f;
    const float sin2_i = std::max(0.f, 1.f-cos_i*cos_i);
    const float sin2_c = (n1/coating_n)*(n1/coating_n)*sin2_i;
    const float sin2_s = (n1/n2)*(n1/n2)*sin2_i;
    if (sin2_c >= 1.f || sin2_s >= 1.f)
        return fresnel_reflectance(cos_i,n1,n2);
    const float cc = std::sqrt(std::max(0.f,1.f-sin2_c));
    const float cs = std::sqrt(std::max(0.f,1.f-sin2_s));
    const float phase = 4.f*float(M_PI)*coating_n*d_nm*cc/lambda_nm;
    const float cphase = std::cos(phase);
    // Real non-absorbing dielectric admittances, s and p independently.
    const auto intensity=[&](float a,float b){
        const float aa=a*a,bb=b*b,cross=2.f*a*b*cphase;
        const float denom=1.f+aa*bb+cross;
        return denom>1e-20f?std::clamp((aa+bb+cross)/denom,0.f,1.f):1.f;
    };
    const float r01s=(n1*cos_i-coating_n*cc)/(n1*cos_i+coating_n*cc);
    const float r12s=(coating_n*cc-n2*cs)/(coating_n*cc+n2*cs);
    const float r01p=(coating_n*cos_i-n1*cc)/(coating_n*cos_i+n1*cc);
    const float r12p=(n2*cc-coating_n*cs)/(n2*cc+coating_n*cs);
    return .5f*(intensity(r01s,r12s)+intensity(r01p,r12p));
}

// ---- Combined surface reflectance ---------------------------------------

// Returns Fresnel reflectance at a lens surface, accounting for AR coating.
// coating_layers: 0 = uncoated, 1 = single-layer MgF2, 2+ ≈ multi-coat
inline float surface_reflectance(float cos_i, float n1, float n2,
                                 int coating_layers, float lambda_nm)
{
    if (coating_layers <= 0)
        return fresnel_reflectance(cos_i, n1, n2);

    // MgF2 single-layer: n=1.38, quarter-wave thickness at 550 nm
    constexpr float mgf2_n = 1.38f;
    constexpr float design_lambda = 550.0f;               // nm
    float qw_thickness = design_lambda / (4.0f * mgf2_n); // ≈ 99.6 nm

    float R = coating_reflectance(cos_i, n1, n2, mgf2_n, qw_thickness, lambda_nm);

    // Preserve the legacy multilayer approximation until real glass/coating
    // prescriptions exist. A power of 2 avoids an O(layers) loop per ray.
    if (coating_layers>1)
        R=std::ldexp(R,-2*(std::min(coating_layers,32)-1));

    return std::clamp(R, 0.0f, 1.0f);
}
