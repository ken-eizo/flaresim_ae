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

## CPU sample-memory improvement (2026-10-16)

- The CPU optical ray tracer now borrows an immutable, shared pupil-sampling
  vector directly from a bounded, thread-safe cache instead of copying that
  vector for each frame. The original `make_pupil` value-returning API is
  preserved for callers that require an owned vector.
- A full 512×512 2-float pupil contains up to 262,144 samples, or
  **2,097,152 bytes**. The optimization avoids copying this memory on a
  cache hit in the CPU render path; it does not change sampling positions,
  radiometric weights or output pixel values.
- This change does not replace the GPU ray-tracing kernels. Its frame-time
  benefit has not been benchmarked in After Effects. The embedded macOS
  CPU-port testing shares the same optical core.

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

## Renderer optimizations (2026-10)

Measured with `FlareLensFrameBenchmark --sweep` (1920x1080, catalog lens 1, 109° FOV, one manual light) on an RTX 5050 Laptop GPU and an i7-13650HX. `--catalog 1 1370 1` times every catalog lens with the same defaults.

| Setting | CUDA before → after | CPU before → after |
| --- | ---: | ---: |
| Defaults (Ray Grid 64) | 3.3 → 2.4 ms | 488 → ~19 ms |
| Ray Grid 512 | — → 9.4 ms | — → 85 ms |
| Ray Grid 1024 | 240 → 14 ms | 3.9 s → 0.11 s |
| Ray Grid 1942 | 873 → 25 ms | 14.5 s → 0.20 s |
| Diffraction with animated rotation | CPU only → 2.6 ms | 66 → 27 ms |
| Ghost Blur 0.1 × 6 passes | 29 → 6.2 ms | — |

- **Ray Grid ≤ 256** keeps per-photon tracing and its Nuke parity. Both renderers visit the pupil in Morton order (`coherent_pupil`), so SIMD lanes and CUDA warps vignette together.
- **Ray Grid > 256** uses the interpolated pupil mesh in `src/FlareLensMesh.hpp`. A 256×256 mesh is traced exactly. Interior cells emit interpolated photons at the requested density, capped at about one photon per covered pixel. Edge and fold cells are traced exactly with about two (edges) or four (folds) photons per pixel of their size on screen, never more than the requested density; CPU and CUDA derive these counts from the same neighbouring vertices. Energy is conserved, and outlines and caustic rims stay sharp. Compared with exact tracing at the same grid, the difference is about 1% L1 at Ray Grid 1942 (1.5% at 1024), mostly the exact render's sampling noise.
- **CPU:** a persistent worker pool (`FlareLensThreadPool`) runs the renderer. Photons are written by row band, so no atomics are needed and the result is deterministic. Rays are traced 8 at a time with AVX2 when the CPU supports it, two such batches stepped through the lens together: each batch's surface events form one long dependency chain, so a core overlaps the two (per-lane arithmetic unchanged). Blur, haze, starburst, composite and source detection are threaded, and full-frame buffers are reused across frames.
- **CPU splat:** the per-photon loops use exact inline `floor`/`isfinite` (MSVC otherwise calls the CRT for each), skip mesh photon rows that cannot reach the band, and accumulate mesh frames in RGBA with one SSE add per pixel. Per-row statistics are accumulated locally (per-pixel writes to shared row arrays bounced cache lines between threads). Mesh vertices outside the aperture are not traced.
- **CUDA:** the build includes sm_120 machine code. The wavelength-parallel kernel is the default. Index tables live in shared memory, and photons are written with 128-bit atomics on sm_90+. Diffraction runs on the GPU, and the wide blurs use Kahan-compensated FP32 instead of FP64.
- **Shared-path tracing:** ghost pairs with the same second bounce b share the path to b and back. Both renderers trace it once per group and branch off each pair at its first bounce a (`cpu::trace_group_*`, `flarelens_group_kernel`, the mesh vertex kernels); every ray is bit-identical to tracing the pair alone (`tests/cpu_trace_group_test.cpp`). Most rays of many-element lenses die inside the lens, so the large zooms and telephotos gain the most:

  | Catalog lenses at the defaults | CPU before → after | CUDA before → after |
  | --- | ---: | ---: |
  | 40–64 surfaces (39 lenses), median | 97 → 48 ms | 8.3 → 5.0 ms |
  | 30–39 surfaces (207 lenses), median | 54 → 37 ms | 4.5 → 4.1 ms |
  | 99th percentile of the catalog | 115 → 56 ms | 9.2 → 5.8 ms |
  | Nikon 180-400 TC (64 surfaces, 1936 pairs) | 350 → 54 ms | 25 → 4.7 ms |
  | Same lens, Ray Grid 1024 | — → 0.77 s | 357 → 107 ms |

  Lenses under 20 surfaces gain about 10–15% in interleaved A/B runs; whole-catalog medians drift by more than that with the laptop's temperature.
- **Ghost pair selection** (upstream `filter_ghost_pairs()`) runs in two cached stages: its traces do not depend on the sensor size, so they run once per lens on the worker pool, and a FOV change only redoes the area-boost arithmetic. Same pairs and boosts (`tests/core_test.cpp`); the 64-surface lens takes 17 ms once instead of 111 ms on every new FOV.
- **Light on the optical axis:** with the light near the frame centre few rays are vignetted, so most of them travel the whole ghost path (all ghosts then also stack on one spot). Canon RF 800mm (43 surfaces, 861 pairs) at the defaults: CPU ~70 ms and CUDA ~7.5 ms centred, against ~30 ms and ~2 ms off-axis. This is real optical work: faster reciprocal/rsqrt arithmetic gave nothing on the CPU (the tracer is latency bound) and only ~15% on CUDA while breaking the 1e-5 Nuke parity, so neither is used.
- **Metal** still uses the original per-ray kernel (one thread per pair, sample and wavelength; no pair groups, shared-memory index tables or pupil mesh). Its generated optical equations are untouched by the CUDA changes (`scripts/generate_metal_physics.py` output is identical).
- **Still expensive by nature:** cost scales with sources × ghost pairs × Ray Grid² × wavelengths. With Detect Bright Sources at 512 sources, CUDA takes ~76 ms and the CPU ~1.0 s. A Cluster Radius merges neighbouring HDR blocks and cuts this in proportion.

## Reduced-resolution previews (Half, Third, Quarter)

A preview renders into a world downsampled by `in_data->downsample_*` and must look like the Full frame scaled down. `preview_scale()` in `src/FlareLensEffect.cpp` sets this up for the CPU and CUDA paths:

- **Light Position:** After Effects hands point parameters over already in the preview world's pixels (960,540 on a 1920×1080 layer arrives as 480,270 at Half; see `PF_InData::downsample_x` in `AE_Effect.h`). `fl::HostGeometry::normalized_x` divides by the layer size in preview pixels and never scales the point again. Until this fix the point was scaled twice, so the light moved toward the top-left corner: to half its position at Half, a third at Third and a quarter at Quarter (a centred light rendered at 240,135 of 1920x1080).
- **Ghosts:** the Ray Grid and wavelength count stay those of Full; the old adaptive reduction to Preview Ray Grid 12 is gone, and its checkbox is hidden. For integer factors, the ghost layer is traced and blurred at full resolution, then box-averaged onto the preview (`box_downscale_rgb`, `flarelens_add_light_box`), so it equals Full scaled down. Metal and non-integer ratios trace on the preview raster with energy scaled by the pixel area.
- **Detected sources:** the scan step covers the Full step's area in full-resolution pixels (scan step × downsample), and Cluster Radius is a full-resolution distance (radius × downsample on the preview, also on the GPU detector). Energy factors correct for the actual source count when the step does not divide evenly.
- **Haze:** stays on the preview raster, with energy per full-resolution pixel and a blur box of the Full blur's variance (`frame_blur_radius`, a fractional outer tap).
- **Diffraction:** averages the full-resolution pixels each preview pixel covers.

`tests/preview_fidelity_test.cpp` renders the real AEX at 1/1, 1/2, 1/3 and 1/4, handing the Light Position over in preview pixels as After Effects does, and compares with the Full frame box-filtered. Manual light, haze, diffraction and Ray Grid 512 all match to rel. L1 ≤ 0.001 with energy ratio 1.000. Detected sources match exactly at Half and Quarter; at Third, 4 px scan blocks become 3 px, so energy matches but detail differs. Before this change, previews were 4×, 9× and 16× too bright, and auto-detected previews up to 63×.

Diagnostic environment variables:
- `FLARESIM_THREADS=n` caps the CPU worker threads.
- `FLARESIM_SIMD=0` forces the scalar CPU tracer.
- `FLARESIM_MESH=0` forces exact tracing at every Ray Grid.
