# macOS: native Metal GPU renderer and Universal After Effects plugin

FlareSimAE builds from **the same repository and optical equations** on Windows
and macOS. The shared C++ After Effects SmartFX effect, source model, lens
catalog, ROI, settings and physical ray tracer are not separately forked.

| Platform | Native AE GPU API | GPU optical modes | CPU fallback |
| --- | --- | --- | --- |
| Windows x64 | CUDA (AE-provided CUDA stream) | Composite, Flare Only, Sources Preview, Ghost Only, Haze Only | Yes |
| macOS arm64 / x86_64 | Metal (AE-provided MTLCommandQueue) | Same five modes | Yes |

**Diffraction/starburst and optical diagnostic output modes remain CPU-only
on both platforms.** The native Metal implementation does not claim to
accelerate every effect parameter or output mode.

## Shared source / numerical parity

- `src/FlareLensEffect.cpp` handles AE's SmartFX PreRender, ROI, GPU device
  selection, GPU World checkouts, MFR and output buffer management on both OSes.
- `src/FlareLensGpu.cu` implements CUDA. The exact device ray physics section
  is extracted into a generated MSL include by
  `scripts/generate_metal_physics.py`, avoiding a second drifting Fresnel
  implementation.
- `src/FlareLensMetalKernels.metal` implements Metal compute dispatch,
  2x2 atomic photon scattering, HDR detection, ghost blur, veiling haze,
  source markers and alpha composition.
- `src/FlareLensMetal.mm` owns the Metal pipeline cache and encoders. GPU
  Worlds and scratch are allocated/obtained through Adobe GPUDeviceSuite.
  GPU commands are synchronized before the host's buffers are returned.
- Both backends share `FlareLensGpuBridge.hpp` and the same lens/pupil/spectral
  preparation code.
- Mac lens bytes are embedded byte-for-byte from the same 1,370 prescriptions.

The native Metal hardware test `FlareLensMetalGpuTest` compares Composite,
Flare Only and Ghost Only against `fl::render_cpu`; it also exercises source
markers, Haze and Metal HDR source detection. This is a **real GPU test on a
macOS Actions runner**, not an After Effects GUI test. Actual AE host
performance and compatibility still require a user test on a Mac.

## Private Adobe SDK in GitHub Actions

The Mac Adobe SDK was supplied privately by the project owner. It is **never
committed**, published as a release asset, or uploaded as a workflow artifact.

`.github/workflows/macos.yml` restores the minimal SDK headers/resources/util
from 23 encrypted repository secrets `AE_SDK_MAC_00` through
`AE_SDK_MAC_22` and the integrity manifest `AE_SDK_MAC_MANIFEST`.
`scripts/actions_sdk_private.py` verifies the complete SHA-256 manifest,
archive members and path safety before extraction into the runner's temporary
directory. No public URL is required.

The pipeline compiles the CPU core, a universal arm64+x86_64 AE plugin,
the Metal shader library, and a hardware-backed GPU parity test. It verifies
the binary architectures, Rez PiPL resource, Metal shader library and ad-hoc
code signature, then packages the plugin. GitHub Actions secrets are needed
only to rebuild the native plugin, not to use the released binary.

## Build from the same source tree

On a Mac with Xcode Command Line Tools and a properly acquired Adobe AE SDK:

```sh
cmake -S . -B build-macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES='arm64;x86_64' \
  -DAE_SDK_ROOT='/path/to/AfterEffectsSDK/Examples' \
  -DFLARELENS_BUILD_AE=ON
cmake --build build-macos --target FlareLensAE FlareLensMetalGpuTest
ctest --test-dir build-macos -R FlareLensMetalGpuTest --output-on-failure
```

The result is `FlareSimAE.plugin`, containing `Contents/MacOS/FlareSimAE`,
`Contents/Resources/FlareSimAE.rsrc` and
`Contents/Resources/FlareLensMetal.metallib`.

## Installing on macOS

Extract the release ZIP, then place `FlareSimAE.plugin` in an After Effects
plugin folder, such as
`/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/`.
Quit AE before installing; avoid duplicate copies. For Metal acceleration,
enable the host's Metal GPU renderer and use a supported 32-bpc SmartFX
workflow. AE can choose CPU fallback for unsupported formats and modes.

The GitHub Actions binary is **ad-hoc signed, not Apple Developer ID signed
or notarized**. macOS Gatekeeper may require explicit approval. Do not
disable system-wide security protections to install a plugin.

## Qualification limits

- Verified: arm64+x86_64 Mach-O compilation, Metal shader compilation,
  macOS Apple GPU hardware parity test, resource presence, code-signature
  validation and bundle packaging.
- Not yet verified: loading and rendering inside a real macOS After Effects
  application, Intel Mac runtime, all lens/parameter combinations, and
  notarized distribution.
- On both OSes: starburst and optical diagnostic modes still render on CPU.
  Unsupported GPU formats/settings are expected to fall back to CPU.

Do not equate GPU kernel time, a standalone Metal test, or successful
GitHub Actions compilation with AE's Composition Profiler frame time.
