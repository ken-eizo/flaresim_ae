# Building FlareSimAE from source

These instructions are for developers. For binary installation, use [INSTALL.md](../INSTALL.md).

## Requirements

**Platform:** Windows x64.

**Build tools:**
- Visual Studio 2022 with the Desktop development with C++ workload
- Windows SDK and CMake 3.24 or later
- Adobe After Effects C++ SDK (downloaded separately from Adobe)
- NVIDIA CUDA Toolkit for a CUDA-enabled build

**Runtime:**
- A compatible Adobe After Effects installation
- An NVIDIA GPU and compatible driver for the CUDA path, or the CPU fallback
- Sufficient memory for the selected frame resolution and rendering options

The Adobe SDK, After Effects, and the CUDA Toolkit are not distributed in
this repository. Please obtain them from their respective vendors.
The project does not need a local copy of the `GizmoSearchAE` project.

---

## Build the plug-in

**1.** Clone the repository:

```powershell
git clone https://github.com/ken-eizo/flaresim_ae.git
cd flaresim_ae
```

**2.** Download and extract the After Effects SDK. Locate its
`Examples` folder, which must contain
`Headers/AE_Effect.h` and `Util/AEFX_SuiteHelper.c`.

For example:

```text
C:/SDK/AfterEffectsSDK/Examples/
    Headers/
        AE_Effect.h
        ...
    Util/
        AEFX_SuiteHelper.c
        ...
```

**3.** Configure a Windows x64 Release build using the Visual Studio
generator. Replace the example SDK path with your own.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DAE_SDK_ROOT="C:/SDK/AfterEffectsSDK/Examples" `
  -DFLARELENS_ENABLE_CUDA=ON
```

**4.** Build the plug-in:

```powershell
cmake --build build --config Release --parallel 4
```

The resulting effect is:

```text
build/Release/FlareLensAE.aex
```

Although the file is named `FlareLensAE.aex`, the effect is shown as
**FlareSimAE** inside After Effects. The original match name is retained
for compatibility with existing AE projects.

**5.** Run the automated tests:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

These tests check the optical renderer, the CUDA implementation where
available, catalog entries, and host-mock callbacks. They do not replace
testing the compiled effect inside After Effects.

---



For Universal macOS / Metal builds and SDK provisioning, see [MACOS_BUILD.md](MACOS_BUILD.md). Adobe SDK components are not redistributable.
