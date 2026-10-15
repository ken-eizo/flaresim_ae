# Architecture

FlareSimAE is an AE-native host adapter around optical components derived from `flaresim_nuke`.

## Repository layout

| Path | Purpose |
| --- | --- |
| `src/FlareLensEffect.cpp` | AE effect registration, parameter setup, SmartFX/CPU render dispatch, automatic source detection |
| `src/FlareLensCustomUI.cpp` | Custom Effect Controls editor, arbitrary data serialization, keyframe interpolation |
| `src/FlareLensCore.cpp` | CPU ray tracing and deterministic image accumulation |
| `src/FlareLensGpu.cu` | CUDA optical ghost kernels and postprocessing on host-managed GPU streams |
| `src/FlareLensGpuDetection.cu` | CUDA bright source extraction |
| `src/FlareLensOptics.cpp` | Pupil/spectral sampling, diffraction/starburst and haze utilities |
| `src/FlareLensCatalog.cpp` | Embedded lens-prescription resources |
| `upstream/` | Original FlareSim Nuke source and lens files, with original notices |
| `tests/` | CTest suite with CPU/GPU, mock host, parity and benchmark programs |

## Optical tracing

A lens consists of ordered `Surface` records. Optical paths with two internal reflections are enumerated. Given a source direction, a pupil sample and a wavelength, each path propagates through refractive or reflective interactions at the surfaces and is projected onto a virtual sensor. Path throughput comes from Fresnel reflectance/transmittance and wavelength-dependent refraction. Output samples are accumulated into an HDR image.

The current ray renderer implements geometric optics. A separate FFT-derived aperture diffraction pattern does not represent a full electromagnetic-wave propagation model of all ghost paths.

## AE host integration

- CPU and CUDA pathways are separated behind the effect's render selectors.
- GPU compositing works on the AE host's CUDA world and command stream; a native Vulkan renderer is **not** implemented.
- The lens editor is an AE Custom Effect Controls widget. It is **not** an independent native timeline layer.
- Lens documents use an AE Arbitrary Data parameter. The current persisted v1 layout remains unchanged for backwards compatibility.
- Numeric fields interpolate between two documents; surface count changes use hold interpolation.
- No AEX installation or AE GUI launch is performed by the automated tests.

## Performance and reproducibility

Benchmark units must specify the image size, lens, pair count, number of sources, sample-grid size, wavelength count, output mode and effect options. Raw CUDA test time is **not** AE timeline/cache time.

The current renderer is proportional to the number of ghost pairs, sources, wavelengths and pupil samples in its expensive ray-tracing stage.

See [CUDA and Vulkan performance](GPU_PERFORMANCE.md), [keyframe animation](KEYFRAME_ANIMATION.md) and root [INSTALL](../INSTALL.md).
