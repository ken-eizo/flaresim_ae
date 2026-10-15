# CUDA and Vulkan performance measurements

## Scope

Measurement device: NVIDIA GeForce RTX 2070 SUPER 8GB, Windows 10.
Lens: `AI_Nikkor_50mm_F1.2S.lens`, 1920x1080 image, native CUDA ghost path, no bloom/blur/haze. Measurements include the CUDA launcher and synchronization but exclude AE's full frame scheduling, input evaluation and caching.

Measurements are **not guaranteed frame times** and can vary with GPU clocks, driver state and other GPU workloads.

## Measured CUDA workloads

| Ray grid | Wavelengths | Sources | Pairs | Optical candidate rays | Median CUDA time |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 512 | 3 | 1 | 1 | 786,432 | 2.55 ms |
| 512 | 7 | 1 | 1 | 1,835,008 | 4.19 ms |
| 512 | 3 | 1 | 78 | 61,341,696 | 87–112 ms in separate runs |
| 512 | 7 | 1 | 78 | 143,130,624 | 197–437 ms in separate runs |

The number of expensive optical evaluations scales approximately as

```
grid × grid × number_of_sources × active_ghost_pairs × wavelength_samples
```

This is why a 512-ray-grid setting alone cannot determine whether 10 ms is achievable.

## CUDA optimizations tested

- **Kept:** source direction precomputation on the host-provided CUDA stream, avoiding per-pupil repeated tangent and normalization computations. A first benchmark showed an improvement for a full-scene 512-grid case, though follow-up measurements varied.
- **Kept:** omission of unused analytic-haze X/Y scratch buffers. With a 1920x1080 image and 512 sources, ghost-only mode saves 6,144,000 bytes of temporary GPU allocation.
- **Rejected:** caching wavelength-dependent index of refraction in extra global GPU arrays made the tested workload slower. That experiment was reverted.

## Vulkan

A Vulkan compute implementation is technically possible but is **not present**. A direct translation of the same kernels cannot be assumed faster than CUDA. After Effects currently provides this implementation with a CUDA-managed GPU world and stream; introducing Vulkan would require a separately verified memory-sharing and synchronization architecture, or additional frame copies.

The priority for a genuinely large speedup is to reduce redundant *optical work* through shared forward paths, meaningful source consolidation, robust pair-contribution bounds, adaptive pupil sampling or reused position-dependent PSFs. Changes must be measured against a high-quality reference and must not silently drop visible ghosts.

## How to measure

Build the benchmark and select one of the command-line cases:

```powershell
cmake --build build --config Release --target FlareLensBenchmark
.\build\Release\FlareLensBenchmark.exe --selected512
.\build\Release\FlareLensBenchmark.exe --grid512
```

Repeat the test under matched device conditions. Record GPU clock/power and source/pair/spectral counts. Run `ctest` after modifications.

The target of a 10 ms **AE timeline render** additionally requires measurements of host checkout, rendering, GPU synchronization, MFR, compositing and cache behavior.
