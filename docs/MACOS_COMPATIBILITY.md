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
   the Metal kernels successfully (CI `39038302118`). **These runners are
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

**Unresolved:** This development CI cannot operate actual Adobe After Effects
on a Mac or simulate all Apple hardware/drivers/macOS releases. Intel Metal GPU
execution may be unavailable on a hosted virtual Intel runner. macOS 11–14
in-host runtime remains unverified until exercised on those OS releases.
CPU fallback after host GPU memory-allocation failure must be verified inside
AE; the current hard memory gate does not establish low-memory safety.
