# macOS compatibility — development audit (not a release)

**Status:** Research and automated testing. The published v1.0.4 release is unchanged.
This page records what is *supported by external requirements*, what the source
implements, and what has actually been tested. A universal binary is not a
guarantee that every Intel Mac/Apple Silicon combination is verified.

## Target compatibility

| After Effects host | Adobe's macOS minimum | Processor support | FlareSimAE status |
| --- | --- | --- | --- |
| 26.x (2026) | macOS 14 Sonoma | Apple Silicon; supported AVX2 Intel Macs | Needs AE-in-host qualification |
| 25.x (2025) | macOS 13 Ventura for recent 25.x | Apple Silicon; supported AVX2 Intel Macs | Needs AE-in-host qualification |
| 24.x (2024) | macOS 12 Monterey | Apple Silicon native; supported Intel Macs | Needs AE-in-host qualification |
| 23.x (2023) | macOS 11 Big Sur | Apple Silicon / supported Intel, version-specific restrictions | Legacy target only, unverified |

Sources:
- Adobe 2026: https://helpx.adobe.com/jp/after-effects/desktop/get-started/technical-requirements/system-requirements.html
- Adobe 2025: https://helpx.adobe.com/jp/after-effects/desktop/get-started/technical-requirements/2025.html
- Adobe 2024: https://helpx.adobe.com/jp/after-effects/desktop/get-started/technical-requirements/2024.html
- Adobe Apple Silicon/Rosetta: https://helpx.adobe.com/uk/after-effects/desktop/get-started/technical-requirements/after-effects-apple-silicon.html
- Apple Metal devices: https://support.apple.com/ja-jp/102894

**Build target:** the CPU universal binary and precompiled MSL library explicitly
target **macOS 11.0 / Metal 2.3**, preserving the possibility of older AE 23.x
installations. Do not lower this or enable newer Metal features without testing
the oldest OS. The actual host's current Adobe requirements take precedence.

The v1.0.4 bundle compiled its `.metal` file without an explicit deployment
target. Apple's standalone Metal compiler can use the *build SDK minimum*
instead. Therefore the published binary's compatibility with older macOS cannot
be inferred from the C++ `CMAKE_OSX_DEPLOYMENT_TARGET` alone. The development
branch explicitly supplies `-mmacosx-version-min` and `-std=macos-metal2.3`.
Apple's compiler behavior: https://developer.apple.com/forums/thread/105719

## Host and GPU safety audit

The host owns `id<MTLDevice>`, `id<MTLCommandQueue>` and the input/output
GPU worlds. Never allocate a plugin-owned replacement device to reach another
GPU: instead use the AE-provided queue, check that borrowed GPU buffers belong
to that device, and reject inconsistent or undersized resources without
dispatch. Mac GPU resources may come from Apple Silicon, discrete AMD, or
Intel integrated graphics; multi-GPU systems are possible.

Changes under test:
- Compare AE buffer device identity with the host queue's device.
- Check full input allocation length before detection, as well as stride alignment.
- Validate signed tile and ROI intersections *before* unsigned byte offsets.
- Reject insufficient or invalid render buffer pitches and missing inputs.
- Wrap Objective-C/Metal work in per-callback autorelease pools for MFR
  worker threads; keep the current synchronous command completion until safe
  resource retirement has been demonstrated.
- Prevent advertising Metal GPU rendering when the backend rejects a frame
  for exceeding its hard per-image scratch bound.
- Render Flare Only, Ghost Only, and Haze Only with a separate zero-plate
  Metal kernel that does not bind an input world. A previously unconditional
  source read could access an unbound `buffer(0)` on transparent/blank AE
  layers; Composite and Sources Preview continue to require valid input.
- Stress device-keyed pipeline caches with four parallel render threads,
  each having its own GPU queue, scratch and host-style image buffers.
- On memory-constrained Macs, allocate only one full RGBA32F scratch image
  for unblurred ghosts (previously two); allocate only a 16-byte sentinel
  for preview and analytic Haze Only. Keep two images when blur or dense
  haze requires ping-pong storage. Verify output against the CPU reference
  with exactly the smaller allocation, not only with oversized scratch.

Relevant local **AEIG** research chapters:
- `docs/gpu-system/cross-platform-effect-architecture.md`: Adobe GPU
  ownership, MFR, fallback and memory residency.
- `docs/gpu-system/release-qualification-matrix.md`: differentiated
  build, standalone GPU, actual host and cross-platform evidence.
- `research/findings/F-GPU-020-shared-cpp-effectmain-native-macos-metal-source-stage.md`:
  Metal/PiPL source-stage findings (historical, not proof of this product).
- `docs/host-integration/cpp-sdk/platform-header-parity.md`.
The AEIG study has separate experimental evidence; it does not automatically
certify FlareSimAE.

## Qualification checklist

1. **Source:** consistent `EffectMain`, canonical PiPL identifiers and
   flags, Cocoa bundle executable, bundle contents and read-only resources.
2. **Build:** x86_64 and arm64 slices; `vtool -show-build` minimum OS;
   compiled `.metallib` minimum OS; archive unpacks and preserves PiPL.
3. **GPU:** Require an actual Metal device and execution of composite,
   blur, preview, haze, detection, blank-input, invalid-resource and 4-thread
   concurrent rendering tests. CTest **Skipped** is never counted as GPU
   runtime proof. Both macOS 15 arm64 and x86_64 hosted runners executed
   the Metal kernels successfully (final CI `39038673198`). **These runners are
   virtualized:** arm64 reports `Apple M1 (Virtual)`; x86_64 reports
   `Apple Paravirtualized Graphics Device`. This confirms MSL execution
   under both CPU ABIs, **not** performance or reliability on all physical
   Intel/AMD/Apple Silicon GPUs. macOS 14's hosted runner (CI `39037857931`)
   reported **no Metal GPU**: its CI leg only verifies compilation, ABI,
   PiPL and bundle/signing. A physical Sonoma Mac and dedicated Intel/AMD
   devices are still required for full native GPU runtime qualification.
4. **AE host:** independently install and run the plugin in AE 24.x/25.x/26.x,
   with at least one Apple Silicon and one Intel Mac. Test all five output
   modes at 32bpc, CPU fallback at 8/16/32bpc, MFR simultaneous frames,
   ROI/negative origins, empty input, and imported legacy projects.
5. **Reliability:** cancel while detecting/tracing; close project during
   rendering; large frames, low-memory device, multiple GPUs and device
   loss/disconnect; compare zero/negative HDR, alpha, and 1,370 lens IDs.
6. **Performance:** real AE composition profiler p50/p95/p99 and peak memory,
   using the *same* fixture across Intel and Apple Silicon, not a kernel-only
   speed comparison.

## Evidence snapshot — 2026-10-17

The exact tested development source commit is `fb0f20bd10006d185b10b1ba24b27fc9a64805d3`.
[GitHub Actions run 39038673198](https://github.com/ken-eizo/flaresim_ae/actions/runs/39038673198)
finished with **five successful jobs**, not a published Release.

| CI environment | Compilation + PiPL/sign/ZIP | Actual Metal kernel execution |
| --- | --- | --- |
| macOS 14 arm64 hosted | PASS | NOT TESTED — no MTLDevice, CTest skipped |
| macOS 15 arm64 virtual M1 | PASS | PASS — GPU math, empty input, bound checks, 4×3 parallel renders |
| macOS 15 x86_64 virtual Intel | PASS | PASS — Apple Paravirtualized Metal GPU |
| macOS 26 arm64 hosted | PASS | PASS — GPU math, empty input, bound checks, 4×3 parallel renders |
| Cross-platform CPU optical core | PASS | Not a Metal test |

Universal `x86_64` and `arm64` slices each report `LC_BUILD_VERSION minos 11.0`.
No job runs actual Adobe After Effects. The separate Windows development
regression build from commit `384ec23` passed all **25/25** Windows CTests;
subsequent edits are macOS-only Metal, test, CI, and documentation paths.

The original **v1.0.4 release and main branch have NOT been changed**.
Do not promote this development branch until physical Intel/AMD/Apple GPUs,
older supported macOS/AE versions, GPU resource teardown under MFR and
low-memory fallback are validated in the relevant real hosts.

**Unresolved:** This development CI cannot operate actual Adobe After Effects
on a Mac or simulate all Apple hardware/drivers/macOS releases. Intel Metal GPU
execution may be unavailable on a hosted virtual Intel runner. macOS 11–14
in-host runtime remains unverified until exercised on those OS releases.
CPU fallback after host GPU memory-allocation failure must be verified inside
AE; the current hard memory gate does not establish low-memory safety.


## 2026-10-17: Reported all-black rendering — evidence and reproduction

A user has reportedly seen a fully black frame with **published v1.0.4**.
No original machine specs, AE build, project, pixel-depth, screenshots, or
trace have been supplied. This is a genuine **unconfirmed report**, not a
closed defect. The public v1.0.4 Release remains untouched.

**Confirmed defect in the published Mac source:** the Metal optical plate
kernel unconditionally read `input [[buffer(0)]]`, even for Flare Only,
Ghost Only and Haze Only. The development branch provides a separate
`fl_plate_empty` kernel with NO required input binding. This removes a
plausible invalid-buffer/blank-render cause; we cannot claim that it
explains every black frame reported by users.

**Verified negative case:** the shipping AEX's original, real parameter
defaults (Light Position fixed coordinates `50,50`, radiance `64`,
diameter `96`, gain `1000`) rendered non-black pixels from a
transparent layer at **640×360 8bpc** in the actual compiled Windows
EffectMain mock-host exercise (not live AE). This disproves a *universal*
CPU-initial-defaults-is-black failure, not all GPU/host scenarios.

**Added regression:** Metal 32-bit float Composite now renders a fully
zero BGRA/alpha input and rejects all-blank RGB or RGB without alpha;
it complements the existing Flare Only/empty input test. The packaged
download is now extracted and the actual packaged `.metallib` is
loaded and run by a standalone GPU test, rather than assuming the
CI-generated `.metallib` matches a valid downloadable asset.

Remaining separate diagnoses: real AE CUDA/Metal GPU command selection,
effect video cache / 2025 host regressions, dynamic data fields,
nonmatching source versus composition sizes, absent source-detection
threshold hits, 8bpc quantization of faint physical ghost rays, and
different lens/FOV/pair choices. Use `Output Mode → Sources Preview`
(to check emission/detection), `Ray Hits Preview` (to check physical
sensor hits), `Optical Diagnostics` (to inspect the render stages),
and `Development Host Trace` if reproducing a black frame. This is
observability, not proof that the underlying fault is fixed.

Adobe separately documents fixes to **black preview/render flashes** and
**effects extending layer bounds** in After Effects **25.5**. A host
25.4-specific report must be distinguished from a FlareSimAE defect:
https://helpx.adobe.com/after-effects/desktop/troubleshooting/fixed-and-known-issues/fixed-issues.html

## AE 2024–2026: API architecture and qualification

| AE host | Adobe macOS minimum | Relevant behavior | Current product evidence |
| --- | --- | --- | --- |
| 24.x (2024) | macOS 12 Monterey | ARM64 native; no Rosetta AE24 | SDK source/architecture eligible, **host untested** |
| 25.0–25.3.2 (2025) | Monterey supported for older 25.x; check each build | SmartFX + GPU path already existed | **host untested** |
| 25.4–25.6 (2025) | macOS 13 Ventura | AE 25.5 includes black flash/extended-bounds host fixes | **host untested** |
| 26.0–26.5 (2026) | macOS 14 Sonoma | SDK guide updated in 26.5; existing Effect API path should be reviewed for each host build | User confirmed Mac plugin load on an **unspecified AE version**; 26.x host qualification pending |

Adobe sources:
- 24.x: https://helpx.adobe.com/jp/after-effects/desktop/get-started/technical-requirements/2024.html
- 25.x: https://helpx.adobe.com/jp/after-effects/desktop/get-started/technical-requirements/2025.html
- 26.x: https://helpx.adobe.com/jp/after-effects/desktop/get-started/technical-requirements/system-requirements.html
- Apple Silicon/Rosetta: https://helpx.adobe.com/uk/after-effects/desktop/get-started/technical-requirements/after-effects-apple-silicon.html
- Adobe SDK multi-version practice: https://ae-plugins.docsforadobe.dev/intro/compatibility-across-multiple-versions/

**AEIG archaeological boundaries reviewed:**
- `docs/reference/api-completeness-status.md`: SDK 25.6 vs guide
  26.5 identifier delta; a difference in documented identifiers is
  **not** evidence that this effect needs different binaries.
- `docs/host-integration/cpp-sdk/platform-header-parity.md`:
  normalized 25.6 Mac/Win header semantics are identical, but does not
  prove binary or host behavior.
- `docs/host-integration/cpp-sdk/verified-render-contracts.md`:
  SmartFX pre-render checkout, clipped `result_rect`, intrinsic
  `max_result_rect`, zero alpha and dependency/teardown correctness.
- `docs/host-integration/cpp-sdk/independent-abi-reimplementations.md`:
  suite layouts may not be prefix compatible across *historical* host
  generations. Never blindly substitute a newer suite table for an older.
- `docs/gpu-system/cross-platform-effect-architecture.md` and
  `docs/gpu-system/release-qualification-matrix.md`: device ownership,
  host-issued GPU Worlds, MFR, conservative GPU completion, evidence grades.
- `docs/ui-system/effect-controls-custom-ui/cross-platform-compatibility.md`:
  separate UI/Retina/macOS lifetime and host version requirements.

The official GPU SmartFX API dates to **AE 16.0**, and the modern MFR
contract to **AE 22.x**, so our dependencies are not inherently new in
24–26. Adobe generally advises using the newest SDK and testing each AE
version, with runtime feature/host checks where needed. This project
builds with distributed **SDK 25.6** and currently packages one Universal
Mac plugin and one Windows x64 CUDA/CPU AEX. **Universal** means native
`x86_64` + `arm64` Mach-O slices; it does NOT establish loader/
renderer qualification against AE 24, 25, and 26 on distinct machines.
Older AE versions before 24, Adobe-unsupported OS/CPUs, and native
Windows-on-Arm are NOT covered by the current single release.

## Nuke source-of-truth: resolution and optical cropping

The original [LocalStarlight/flaresim_nuke](https://github.com/LocalStarlight/flaresim_nuke)
`src/ghost_cuda.cu` uses the **format width and height** to map a
ray's physical sensor landing point to image coordinates:
`px=(position.x/(2*sensor_half_w)+0.5)*fmt_w+fmt_x0_in_buf`,
and an analogous `py`. The `fmt_x0_in_buf` term keeps the optical
axis tied to the Nuke **format**, not a variable-expanded bounding box.
`src/FlareSim.cpp` computes the sensor halves from lens focal length
and horizontal/vertical camera field of view. Auto vertical FOV follows
the **format aspect ratio**, not an arbitrary bounding-box crop.

FlareSimAE's CPU and Metal ray tracers use the same normalized
millimetre→pixel mapping for their full optical sensor. A pure
1920×1080→960×540 resolution change at unchanged FOV/source positions
should preserve the **relative** ghost footprint; the 2D position/size
in physical pixels scales down. Photons outside the sensor aperture/FOV
are clipped **in both implementations**. Changing aspect ratio, FOV or
source layer geometry can alter the portion that falls inside the sensor.

A real AE difference still needs host validation: AE SmartFX currently
anchors the intrinsic optical extent to `PF_InData.width/height`
(the source layer), whereas the Nuke kernel anchors to the complete
Nuke **format** and has a separate output-bounding-box offset. Thus
a small source layer inside a large AE comp may clip a flare by design
of the current AE adapter, even if simply lowering preview resolution
at constant frame aspect should not. Do **not** silently change the
physical FOV/size or expand `max_result_rect` without host/ROI contract
testing: Adobe warns invalid rectangles can corrupt the render graph.

A CPU optical fixture now compares normalized energy centroids for
320×180 vs 160×90 at the same FOV, both centred and with a shifted
emitter; AE legacy ROI/downsampling pixel parity also remains covered.
