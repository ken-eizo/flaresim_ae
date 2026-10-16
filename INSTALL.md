# FlareSimAE — Installation Guide

This guide walks you through installing FlareSimAE for After Effects on
Windows, either from a pre-built ZIP or by compiling the native effect.

The first public release is **v1.0.0**. Compilation and automated tests have
passed on the development system, but not all interactive workflows have been
validated inside After Effects. Test on noncritical projects first.

---

## What's in the release zip

Download the Windows CUDA ZIP matching your release version from
[GitHub Releases](https://github.com/ken-eizo/flaresim_ae/releases).

```text
FlareLensAE.aex         (Windows After Effects effect)
README.md
INSTALL.md
LICENSE.txt
LICENSE_UPSTREAM.txt
NOTICE.md
FlareLensCatalog.tsv   (embedded catalog reference index)
lenses/
    lens_files/        (original prescriptions; optional reference data)
    convert_ob.py
    convert_p2p.py
```

**Only `FlareLensAE.aex` needs to be installed in After Effects.** The 1,370
original prescriptions are also compiled into the AEX; the included lens
files and conversion scripts are reference material, not sidecars required
by the plug-in.

No macOS, Linux, or CPU-only ZIP is available for this release.

---

## What's in the source repository

The main parts of the repository are:

```text
flaresim_ae/
    CMakeLists.txt
    README.md
    INSTALL.md
    src/
        FlareLensEffect.cpp
        FlareLensCustomUI.cpp
        FlareLensGpu.cu
        ...
    upstream/
        LICENSE
        src/
        lenses/
            lens_files/
                ... (1,370 original prescriptions)
    tests/
    docs/
```

All 1,370 original lens prescriptions are **embedded in the compiled plug-in**.
You do not need to copy the `upstream/lenses/` directory into After Effects
after building.

---

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

## Install the plug-in

**1.** Download the ZIP from
[Releases](https://github.com/ken-eizo/flaresim_ae/releases) and extract it,
or compile from source using the instructions above. Close After Effects
before replacing an installed plug-in.

**2.** Copy the compiled `FlareLensAE.aex` manually into the `Plug-ins`
directory of your After Effects installation, for example:

```text
C:/Program Files/Adobe/Adobe After Effects <version>/Support Files/Plug-ins/FlareSimAE/
    FlareLensAE.aex
```

You may need administrator privileges to write to this directory.
The actual installation folder depends on your After Effects version
and installation location.

**3.** Make sure there is only **one active copy** of this effect's AEX
under the plug-in search paths used by After Effects. Older copies may
have the same AE match name.

**4.** Launch After Effects and look for **FlareSimAE** among the
available effects. Apply it to a compatible layer and inspect its
effect controls.

The plug-in is experimental. Before using important projects, test on a
copy of a project and verify both CPU/GPU output and saving/loading.

---

## Multiple After Effects versions

You can maintain separate copies of the AEX for different After Effects
installations. Build against a compatible SDK for the version you intend
to use, and install only the appropriate build for that version.

Do not put multiple copies of the same match name in the plug-in folders
visible to a single After Effects installation.

---

## Troubleshooting

### "FlareSimAE doesn't appear in the effects list"

- Check that `FlareLensAE.aex` is in a folder scanned by the correct
  After Effects installation.
- Remove or move duplicate/older plug-in copies out of active search paths.
- Restart After Effects after making changes.
- Confirm that you built the x64 Release effect rather than only a
  benchmark or standalone test executable.
- Examine After Effects' plug-in loading messages or logs for SDK/binary
  compatibility errors.

### "The effect loads, but GPU rendering is unavailable"

- Confirm that the CUDA-enabled variant was compiled successfully.
- Confirm that your NVIDIA driver and GPU support the build.
- Note that some output/diagnostic modes deliberately use CPU rendering.
- Check After Effects' project GPU settings and the effect's render
  diagnostics when investigating a fallback.

### "I enabled Detect Bright Sources but see no flare"

- Use HDR footage or reduce **Source Threshold**. The default threshold
  is `2.0`, while ordinary scene-linear SDR white is approximately `1.0`.
- Apply the effect to the layer containing the bright pixels. An empty
  solid does not automatically read the visible composition underneath it.
- Select **Sources Preview** in **Output Mode**. If it shows the
  expected source positions, investigate ray tracing and output brightness
  rather than the source detector.
- Start with **Maximum Sources** set low while checking expensive lenses.

### "The render is much slower than expected"

- Reduce **Ray Grid**, **Maximum Sources**, and **Spectral Samples**.
- Increase **Source Threshold** so fewer light sources are detected.
- Use **Only Selected Pair** to isolate a single optical ghost.
- Enable **Use Preview Quality** while interacting with parameters.
- Remember that raw CUDA benchmark times are not the same as the
  After Effects frame render/cache time.

### "Where do I put the lens files?"

You do **not** need to install them separately. FlareSimAE embeds the
original 1,370 catalog prescriptions inside the AEX.

Use **Browse All Nuke Lenses** for a catalog lens, or select
**Custom Lens** and **Initialize Custom Lens** to copy a prescription
into the editable After Effects parameter.

---

## Build limitations

The v1.0.0 Release distributes Windows binaries only. An experimental
macOS **CPU-only** source build and GitHub Actions workflow are documented
in [docs/MACOS_BUILD.md](docs/MACOS_BUILD.md); native Metal and Vulkan GPU
renderers remain unimplemented. Mac plugin compatibility is not yet validated.

The project is based on
[FlareSim for Nuke](https://github.com/LocalStarlight/flaresim_nuke).
The upstream optical code and its original MIT attribution are retained
in [upstream/LICENSE](upstream/LICENSE). See the repository
[README.md](README.md) and [NOTICE.md](NOTICE.md) for further credits.
