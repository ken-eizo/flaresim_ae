# FlareSimAE

**FlareSimAE** is an experimental, native **Adobe After Effects** lens-flare plugin based on the optical modeling from [flaresim_nuke](https://github.com/LocalStarlight/flaresim_nuke). It simulates multiple reflections in a photographic lens to generate ghost flares from a lens prescription.

This is an **unofficial, work-in-progress After Effects adaptation**, not an Adobe product or an official release by the upstream maintainers.

## Current development status

- Windows x64 with an NVIDIA CUDA path and CPU fallback.
- Native AE effect with SmartFX integration and a compact custom Effect Controls lens editor.
- Camera-lens presets derived from the original Nuke repository, including an embedded catalog of 1,370 `.lens` prescriptions.
- Geometric two-bounce ghost tracing with wavelength-dependent refraction, Fresnel weighting, and selectable spectral samples.
- Manual light source placement or automatic bright-source detection from the effect's input layer.
- Diffraction/starburst and veiling-haze components, composite/flare-only/ghost-only and source-preview modes.
- The custom lens prescription is stored using AE Arbitrary Data. Numeric prescription properties interpolate between keyframes; changes in surface count are held until the next keyframe.
- Source-direction CUDA precomputation, bounded GPU scratch allocation, and host-independent tests.

**Important:** the most recent build was verified by automated native host-mock, CUDA and optics regression tests (**24/24 passed**). Custom editor keyframe interaction, rendering, cancellation, and UI behavior in an actual After Effects session **have not yet been revalidated** for this build. This repository is a **developer preview**, not a stable public binary release.

## Building from source

The project currently builds on **Windows** using CMake, Visual Studio 2022, the **Adobe After Effects C++ SDK** and optionally the NVIDIA CUDA Toolkit.

The Adobe SDK is **not redistributed**. Download it separately from Adobe and point `AE_SDK_ROOT` to its `Examples` directory.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DAE_SDK_ROOT="C:/path/to/AfterEffectsSDK/Examples" `
  -DFLARELENS_ENABLE_CUDA=ON

cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The built plugin is `build/Release/FlareLensAE.aex`.

See [INSTALL.md](INSTALL.md) for details and known limitations. Neither a local GizmoSearchAE checkout nor the original Nuke application is required to compile the AE source.

## How it works

1. **Lens prescription:** spherical/planar optical surfaces with curvature, spacing, refractive index, dispersion and coating.
2. **Sources:** a manual source or bright regions identified from the effect's own input layer (not the entire underlying AE composition).
3. **Ghost paths:** two reflective surfaces define a double-bounce optical path. Refracted and reflected rays are propagated to the sensor plane.
4. **Rendering:** pupil samples and wavelength samples contribute HDR light to the image; optional diffraction, haze and compositing are applied separately.

The current FFT-generated diffraction component does **not** amount to a complete wave-optical model of internal lens reflections. The geometric ghost renderer also is **not** identical to a generic full-frame FFT bloom convolution.

## GPU performance

Performance strongly depends on `ray grid × ray grid × wavelength count × source count × active ghost-pair count`. Benchmarks below use an RTX 2070 SUPER, 1920×1080, one Nikkor 50mm f/1.2 preset, single-source ghost-only mode, and **exclude After Effects timeline scheduling and cache costs**.

| Ray grid | Wavelengths | Ghost pairs | Example CUDA median |
| ---: | ---: | ---: | ---: |
| 512 | 3 | 1 | 2.55 ms |
| 512 | 7 | 1 | 4.19 ms |
| 512 | 3 | 78 | 87–112 ms (varied between runs) |

These are **examples, not a 10ms guarantee**. The full 78-pair scene takes far longer than a one-pair render.

For reproducible tests and optimization notes, see [CUDA and Vulkan performance](docs/GPU_PERFORMANCE.md).

## Supported and unsupported environments

- **Implemented:** Windows x64; NVIDIA CUDA; CPU rendering path.
- **Not verified for this build:** interactive real-AE behavior, many GPU/CPU fallback combinations, the full spectrum of host configurations.
- **Not available:** native macOS/Metal binary or native Vulkan compute renderer.
- **Not guaranteed:** realtime full-quality output for arbitrary light counts, lenses or Ray Grid settings.

## Development

The source includes `src/` (AE integration), `upstream/src/` (original optical foundation), `upstream/lenses/` (prescriptions), `tests/` (unit, host-mock, CUDA and parity tests) and `docs/` (selected engineering notes). The CMake build embeds the 1,370 optical prescriptions and builds without Nuke.

See [architecture](docs/ARCHITECTURE.md), [keyframe animation](docs/KEYFRAME_ANIMATION.md), and [CUDA/Vulkan performance](docs/GPU_PERFORMANCE.md). Contributions, measurements and compatibility reports are welcome. Please distinguish results measured in a standalone test from performance displayed by After Effects.

## Credits and license

FlareSimAE is based on [LocalStarlight/flaresim_nuke](https://github.com/LocalStarlight/flaresim_nuke). Original ray-tracing work: **Eamonn Nugent**. Nuke plugin adaptation and further work: **Steve Watts Kennedy**. AE adaptation: **ken-eizo / contributors**.

The upstream code is licensed under the **MIT License**. Preserve the original notices in [upstream/LICENSE](upstream/LICENSE). See [NOTICE.md](NOTICE.md) for attribution and third-party boundaries.

The repository uses the MIT License; see [LICENSE](LICENSE). Adobe After Effects SDK files and NVIDIA proprietary libraries are **not** part of this repository and retain their respective licenses.
