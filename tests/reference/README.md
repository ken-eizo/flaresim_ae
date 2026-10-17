# Unmodified FlareSim for Nuke reference

Golden source: [LocalStarlight/flaresim_nuke](https://github.com/LocalStarlight/flaresim_nuke)
at **0e28ead9a3ec4898c7568f27b1ce8c0de6df7a08**.

`nuke-original-0e28ead-case0-256x144-rgb32f.bin` contains the original
unmodified CUDA renderer's planar interleaved RGB32f data, not the
FlareSimAE CPU/GPU renderer. It is 256 x 144 x 3 float32 values,
with **442,368 raw bytes**, little-endian, SHA-256:
`902edee8e1955d9666e145d0870d3708b50f76783206fd2bb24a8ee86a3239b8`.

The reference was generated from the upstream Git tree using
`tests/upstream_reference.cpp`, with the **original** `fresnel.h`,
`ghost_cuda.cu`, `ghost.cpp`, `lens.cpp`, and `trace.cpp`. Its source
is the first of 36 `tests/upstream_fixture_cases.hpp` fixtures:

- `AI_Nikkor_50mm_F1.2S.lens`
- 256 x 144, horizontal field of view 40°, 24 × 24 ray grid
- Pupil jitter OFF, 3 spectral samples, aperture circular
- Flare gain 1000; input scene-linear RGB (300, 140, 75) at
  normalized (0.39, 0.48)
- Raw ghost reflection RGB **before** haze, starburst, blur, output
  compositing, or any AE-only manual-light area or resolution compensation

The CUDA reference's full 36-frame file is independently regenerable
from the original commit. The Windows reference test rebuilds and
compares 36 cases; macOS Metal can verify against the small, checked-in
unaltered original fixture **without needing CUDA or a Nuke license**.

Do not claim exact Nuke application output from this test alone: pixel
inputs, source selection, Nuke Iop channel behavior, Nuke host preview
and postprocessing still require independent end-to-end tests.

The upstream and AE ray weighting use floating-point atomic additions:
IEEE float rounding and execution order can differ, so tests use a
numeric tolerance and explicitly report relative L1 error.
