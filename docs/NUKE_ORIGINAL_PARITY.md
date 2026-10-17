# FlareSimAE ↔ original FlareSim for Nuke: optical parity

Status: **development only (after v1.0.5)**. This audit does **not**
update the published v1.0.5 release or claim all Nuke/AE outputs are
identical.

## Source of truth

- Original public repository: https://github.com/LocalStarlight/flaresim_nuke
- Original source revision: `0e28ead9a3ec4898c7568f27b1ce8c0de6df7a08`
- The original Git tree, **not** the preexisting `upstream/` working
  copy, is the reference. That working copy had locally changed
  `fresnel.h` and `ghost_cuda.cu` to use an alternative s/p
  polarization calculation.
- Original `lens.h`/`lens.cpp` are changed only to support parsing
  the same `.lens` bytes from AE's built-in lens resources. This
  changes data ingestion, not the underlying optical model.
- Original `trace.cpp`, `ghost.cpp` and `starburst.cpp` were
  unchanged in the AE copy.

## Proven mismatch and fix

The released v1.0.5 `upstream/src/fresnel.h`,
`upstream/src/ghost_cuda.cu` and `src/FlareLensGpu.cu` use
a modified thin-film coating reflectance model with separate
s/p polarizations. The unmodified Nuke code instead uses a
**single scalar amplitude** and the original Airy interference
reflectance. Neither is a universal physical ground truth: here,
**fidelity to the original implementation** is the acceptance
criterion.

We restored the Nuke original equations in the AE CPU optical
reference and CUDA path. macOS Metal optical physics is
*automatically generated* from `src/FlareLensGpu.cu`; it must use the
same original coating formula. We changed a formerly s/p-focused unit
test to independently verify the exact scalar Nuke transfer function.

An **independent Windows CUDA reference executable** was built from
the unmodified 0e28ead Git tree, not the project's patched files.
The executable renders 36 HDR RGB32f scenes from four original Nuke
lenses, using all three pupil modes and 3/5/11 spectral samples.

| Measure | Released v1.0.5 (case 0) | After restoring original optics |
| --- | ---: | ---: |
| Nuke vs AE CUDA relative L1 | 0.0610568 | approximately 8e-10 |
| Nuke vs AE CPU relative L1 | 0.0610581 | approximately 0.000191 |
| Reference cases | 36 | 36 passed |
| Windows per-case acceptance threshold | previously GPU 1.5%, CPU 3.5% | GPU 0.001%, CPU 0.1% |

The old tolerance of 1.5%/3.5% could conceal meaningful differences;
it is now much stricter. Float32 GPU atomic sums can be reordered,
so **bit-for-bit equality is not guaranteed**, even with otherwise
identical optical equations.

The new independently generated golden image
`tests/reference/nuke-original-0e28ead-case0-256x144-rgb32f.bin`
provides a portable reference for macOS Metal where CUDA/Nuke are not
installed. Its SHA-256 and source provenance are verified by
`tests/reference/verify_pristine_sources.py`; Metal now computes its
RGB result and directly compares **each pixel** to original Nuke.
This test does not use the AE CPU image as its reference.

### Native Metal validation (development CI run 39075064841)

On the checked-in **unmodified Nuke** RGB32f reference (256×144),
the GPU renderer produced the following independent pixel-space errors:

| macOS GPU CI environment | Metal relative L1 error |
| --- | ---: |
| macOS 15, Apple Silicon virtual device | 0.000202779 (0.0203%) |
| macOS 15, Intel / paravirtual graphics | 0.000182784 (0.0183%) |
| macOS 26, Apple Silicon virtual device | 0.000202779 (0.0203%) |
| macOS 14 | GPU device unavailable; **not tested on GPU** |

The suite also reopens the **packaged** `.metallib` and runs the original
Nuke golden test after extracting the ZIP. The test threshold is
**0.1% relative L1** (`0.001`), rather than the former 1.5%.
These are virtual CI devices: **physical Apple Silicon, Intel/AMD discrete
graphics and AE inside-host tests remain outstanding**.

## Remaining fidelity gaps — do not claim full parity yet

| Stage | What "same settings" requires | Current limitation / evidence |
| --- | --- | --- |
| Lens | Same .lens file, surfaces and selection | 1,370 original catalog recipes embedded; complete catalog tests |
| Field of view | Same Nuke format, FOV H and derived V, normalized source position | Optical fixed-format test present; differing AE source-layer vs Nuke image format/bounding-box needs real AE verification |
| Ghost pair eligibility | Same original `filter_ghost_pairs`, enabled surface pairs, ray grid | Original source shared and 36 standalone tests; user-specified individual pair masks need additional end-to-end coverage |
| Spectral & coating | Same R/G/B spectral samples, IOR, thin-film formula, wavelengths | Restored and numerically verified in 36 CUDA/CPU tests; Metal golden case added |
| Emission | Same scene-linear image RGB, source grouping, exposure/alpha | Nuke detects from input RGB; AE has an extra manual point/area source mode. Matching manual radiance is **not** equivalent to an arbitrary HDR source image |
| Auto source detection | Same threshold, cap, binning, centroid and clustering, max sources | Logic adapted to AE buffer layouts. Full arbitrary HDR input images have not been compared pixel-by-pixel with actual Nuke host. |
| Preview | Same source stride, compensation, jitter and per-frame seed | AE's adaptive preview/downsample policy is host-specific and may diverge |
| Raster energy | Original Nuke conserves **total ray energy** in pixel splats | `Match FHD Flare Brightness` is AE-specific. **OFF** is required for Nuke-exact raster intensity; ON deliberately changes the result |
| Haze / Ghost blur | Same source footprint, blur radius/passes and boundary convention | Upstream post routines reused or approximated; direct per-pixel Nuke/AE integrated comparison pending |
| Starburst | Same original aperture PSF/FFT, wavelength, scale and gain | Original starburst core shared; host-side placement and summation parity not fully verified |
| Output channels / alpha | Nuke `flare.rgb`, `source.rgb`, `haze.rgb`, `starburst.rgb` + configurable `rgba` | AE exposes single RGBA effect / modes. **Not** a direct one-to-one host-channel representation |
| HDR precision | Scene-linear float Nuke output | AE 32bpc required for meaningful float32 comparisons; 8/16bpc quantize/clamp |
| Mac host | Native Apple Silicon/Intel loader; AE command queues, resource ownership and MFR | CI Metal simulator/virtual GPU validated; **real AE 2023–2026 rendering and physical discrete AMD GPUs remain unverified** |

## Release discipline

- Keep published v1.0.5 tag and ZIPs immutable.
- Require independent **unmodified original Nuke** golden numerical tests
  for CPU, CUDA and Metal; use fixture hash checks to prevent accidental
  replacement by an AE-generated image.
- A parity fix should not silently add empirical brightness multipliers,
  change the lens's physical FOV, resample or blur the original ghost
  rays, or change FFT/star/haze without a comparative fixture.
- Only then benchmark memory allocations, output cost, GPU occupancy,
  tiling/ROI, zero-input and MFR correctness; optimize with numerical
  tolerances still enforced.
- Do **not** declare "completely identical" until the remaining
  scene-linear input, source-detection, postprocessing, compositing,
  named-channel and real-host differences are separately tested.
