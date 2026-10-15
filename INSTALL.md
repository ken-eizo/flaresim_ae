# FlareSimAE — source build notes (pre-release)

**Status:** developer preview. This source tree builds without private sibling repositories,
but the Adobe SDK and (for GPU builds) NVIDIA CUDA Toolkit must be supplied separately.
Do not claim support for host configurations that have not been verified inside After Effects.

## Prerequisites (Windows)

- Visual Studio 2022 with MSVC C++ and Windows SDK
- CMake >= 3.24
- Adobe After Effects C++ SDK (download separately from Adobe; **not included**)
- NVIDIA CUDA Toolkit compatible with the target GPU architecture
- Adobe SDK `Examples/Util/AEFX_SuiteHelper.c` (provided by the Adobe SDK)

## Configure

You can provide absolute paths through explicit CMake cache entries:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DAE_SDK_ROOT="C:/SDK/AfterEffects/Examples" `
  -DFLARELENS_ENABLE_CUDA=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Alternatively define environment variable `AE_SDK_ROOT` before
initial CMake configuration. CMake cache options
take precedence after that.

**Output:** `build/Release/FlareLensAE.aex`.

The complete upstream Nuke lens catalog is compiled into the AEX. Runtime
installation does not require the original .lens files.

## Compatibility and attribution

This AE project is an **unofficial derivative** of
[LocalStarlight/flaresim_nuke](https://github.com/LocalStarlight/flaresim_nuke),
which credits the original Eamonn Nugent ray-tracing work.
The original optical code and lens files remain under `upstream/` with
`upstream/LICENSE` (MIT). Preserve those copyright notices.

Publishing license details for new AE-specific source code, complete dependency
instructions, binary artifact hosting, and actual After Effects host validation
are still pending. See `docs/AE_KEYFRAMES_PERFORMANCE_PUBLIC_RELEASE_2026-10-16.md`.

Do not copy Adobe SDK source/binaries into the public repository.
