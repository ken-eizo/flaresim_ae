# After Effects 2023–2026 / FHD-to-720p resolution fidelity (development)

**Scope:** `dev/macos-compatibility-audit`. This is **not** a released build.
The published **v1.0.4 remains untouched**.

## What the request means

For a scene with identical lens, horizontal/vertical FOV, normalized source
position, color and optical settings, lowering the raster from 1920×1080 to
1280×720 should preserve the **relative shape and size of the flare on
screen** and **scene-linear brightness per image pixel** as closely as
possible. The lower resolution should provide *fewer samples / poorer detail*,
not a physically larger or brighter flare.

Important distinction: same literal AE Point Control pixel position (e.g.
100,100) is **not** the same normalized position across different comp sizes.
For equivalent location, scale Light Position by the new raster dimensions,
e.g. (960,540) in FHD versus (640,360) in 720p.

## Verified original defect: pixels become brighter at smaller resolution

Source optical splat: `src/FlareLensCore.cpp` maps sensor-space X,Y to
`((sensor_x / (2*sensor_half_width))+.5)*raster_width` (and Y similarly).
The original Nuke code `src/ghost_cuda.cu` uses the same formula, anchored to
the *Nuke format* rather than the bounding box. Consequently **size already
scales correctly** when the output aspect/FOV and optical source are the same.

Each ray has a fixed integrated energy and splats to 2×2 destination pixels;
with a smaller raster, the **same integrated light is concentrated into fewer
pixels**. Full-frame intensity per pixel therefore rises as image area falls.

New regression through the **actual compiled Windows AEX EffectMain**
(`tests/host_render_test.cpp`, transparent input, 32bpc, isolated Flare Only,
same normalized light position, lens, FOV and optical settings):

| Scenario | 1920×1080 mean RGB | 1280×720 mean RGB | 720p / FHD |
| --- | ---: | ---: | ---: |
| Published Nuke-exact scattering | 0.00330388 | 0.00743346 | **2.24992** |
| Experimental FHD matching enabled | unchanged | resolution normalized | **0.999966** |

The FHD output is checked **bit-for-bit identical** between modes.
Additional shape regression measures centroid and intensity-weighted
horizontal/vertical extent in normalized image coordinates and requires
differences ≤0.01 between resolutions.

## Conservative implementation

Appends exactly **one** AE-native checkbox:
**Match FHD Flare Brightness / Enable (Manual Light)**.

**Default OFF.** Existing projects and their established
v1.0.4/Nuke numerical appearance remain unchanged. This is intentional
until visual review and actual multi-AE-version in-host testing.
No existing parameter ID/position is reused. New checkbox ID=64
(`P_FHD_STABLE_BRIGHTNESS`).

When ON and **Manual Light** (not automatic bright-source detection), the
source radiance multiplied before CPU/CUDA/Metal optical rendering is:

`resolution_scale = (optical_raster_width * optical_raster_height) / (1920 * 1080)`

It is *1* for full-size FHD, *4/9* for 720p, *1/4* for half-size FHD preview,
and *4* for UHD 3840×2160. It is the **actual computed optical sensor
resolution**, preserving consistency under AE interactive downsampling.
This changes radiance/energy only, **not** FOV, ray intersections, spectral
weights, geometry, blur scales or output position.

Do not advertise this experimental policy as a replacement for upstream
Nuke's exact energy-conserving raster splat: it deliberately implements a
different *display-radiance* policy requested for AE comp scaling.

The manual source's physically modelled disk diameter remains a reference
full-resolution layer-pixel size. It is an area-integration *emitter model*,
not a rasterized photographic sprite; no extra disk-size scaling is done.

Unchanged: **Detect Bright Sources**, whose number/density of independent
HDR sources changes with image resolution, `max_sources` and `scan_step`;
a naive area multiplier may overcorrect it. Full end-to-end
screen brightness matching for detected sources, Haze/Starburst and
arbitrary aspect-ratio changes requires separate tests and algorithms.

## Adobe SDK / AEIG compatibility gates

Adobe's guide recommends building with a current SDK and thoroughly testing
every AE host version claimed compatible. Never infer host/product version
from PF API version: AEIG's `docs/archaeology/pf-api-version-lineage.md`
records that the 25.6 Effect ABI still uses the **AE 23.5-era
13.29 PF compatibility pair** even though internal implementations change.
Refer to AEIG's `docs/reference/version-matrix.md`,
`docs/host-integration/cpp-sdk/verified-render-contracts.md`,
`docs/host-integration/cpp-sdk/platform-header-parity.md` and
`docs/gpu-system/release-qualification-matrix.md`.

| Requested AE | Status for current source | macOS minimum for relevant host |
| --- | --- | --- |
| 2023 / 23.x | Public API/ABI design eligible; **host NOT TESTED** | Big Sur 11 |
| 2024 / 24.x | Public API/ABI design eligible; **host NOT TESTED** | Monterey 12 |
| 2025 / 25.x | Public API/ABI design eligible; **host NOT TESTED** | depends on minor version (Ventura 13 for later 25.x) |
| 2026 / 26.x | Public API/ABI design eligible; **full host matrix NOT TESTED** | Sonoma 14 |

One native universal `x86_64 + arm64` bundle is compiled with minimum
macOS 11.0/Metal 2.3 so binaries can be *considered* for AE 2023.
This is a deployment-target requirement, **not** real host-loader evidence.
Windows x64 binaries require validation separately for all four AE versions.
Actual legacy AE acceptance requires loading the effect, testing 8/16/32bpc,
MFR, CPU fallback, GPU and PiPL/parameter serialization/old projects
on those precise AE builds. No older AE application is currently available
in the automated runner, and no 2023/2024/2025 live host test is claimed.

Adobe cross-version SDK guide:
https://ae-plugins.docsforadobe.dev/intro/compatibility-across-multiple-versions/
Adobe old host requirement:
https://community.adobe.com/questions-534/announcement-changes-to-minimum-os-requirements-for-after-effects-23-0-314157

## Release gate

Keep OFF-by-default until (a) user visual review, (b) CPU/GPU parity for
manual emitter across FHD/720p, (c) no regression with existing projects,
and (d) AE 2023–2026 loader/runtime qualification. Do **not** publish
a Release from this development branch.

## Verified implementation snapshot (2026-10-17)

Development **source and tests:** `27f78089cacb3167b80df171eece5266df0c270a`.
Documentation is committed separately from the source. The following
checks explicitly succeeded; none launches After Effects 2023–2026:

- [macOS multi-version development CI](https://github.com/ken-eizo/flaresim_ae/actions/runs/39041708711):
  5/5 jobs successful. macOS 15 arm64, macOS 15 Intel x86_64, and macOS
  26 arm64 virtual GPU runs execute `METAL_FHD_REFERENCE_EMISSION_SCALE_PASSED`,
  with expected `factor=0.444444`, observed optical energy
  `ratio=0.444444`. The test also executes directly with the Metal
  library **extracted from the packaged development ZIP**.
  macOS 14 compiled/bundled/signed successfully, but no Metal device
  was available in the hosted runner: GPU runtime is **unverified**.
- Windows full project CTest suite: **25/25 passed**, with the new
  compiled AEX render-size and brightness test.
- Actual Windows AEX against mocked AE host: raw 720p/FHD mean HDR
  intensity ratio `2.24992`, opt-in ratio `0.999966`, FHD bytes
  unchanged. Normalized intensity-weighted X/Y centroid/spread
  `(0.471231, 0.477174, 0.269591, 0.281201)` at FHD and
  `(0.471253, 0.477197, 0.269573, 0.281204)` at 720p.
- Default **OFF** preserves published output and all existing parameter
  IDs; the new param is appended at ID=64. This has **not** been
  loaded in real AE 2023/2024/2025/2026 as part of this audit.

Windows NVIDIA CUDA-specific render output at FHD/720p is covered by
the shared host-side source energy adjustment and successful Windows
regression suite, **not** by an additional end-to-end paired CUDA
device pixel-brightness test. That GPU-specific validation remains open.
