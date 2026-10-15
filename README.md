# FlareSimAE

A CUDA-accelerated After Effects plugin for physically-based lens flare simulation.
FlareSimAE traces rays through real lens prescriptions to produce ghost
reflections that respond to source position, wavelength, and aperture shape —
without artist-painted flare textures.

FlareSimAE is an unofficial After Effects adaptation of
[FlareSim for Nuke](https://github.com/LocalStarlight/flaresim_nuke).
This is a **developer preview**: the source builds and passes automated tests,
but the latest build has not been fully validated inside After Effects.

---

## Credits

FlareSimAE is built on [**FlareSim for Nuke**](https://github.com/LocalStarlight/flaresim_nuke),
developed by **Steve Watts Kennedy** ([@LocalStarlight](https://github.com/LocalStarlight)).

FlareSim itself is built on the foundational work of **Eamonn Nugent**
([@space55](https://github.com/space55) · [55.dev](https://55.dev/)),
whose original CPU-based lens flare renderer established the ray-tracing
approach, optical physics, and lens file format used by these projects.
The original codebase is
[github.com/space55/blackhole-rt](https://github.com/space55/blackhole-rt/).

The After Effects adaptation is developed by
[**ken-eizo**](https://github.com/ken-eizo), Codex.
The original optical code, lens prescriptions, and copyright notices are
preserved in [upstream/](upstream/).
See [LICENSE](LICENSE), [upstream/LICENSE](upstream/LICENSE), and
[NOTICE.md](NOTICE.md).

---

## How it works

FlareSimAE takes an image as input. It can treat its bright pixels as light
sources, or use a manually positioned source. For each source it fires a grid
of rays across the entrance pupil of the lens, tracing them through the
optical surfaces. Two surfaces reflect the ray rather than transmitting it:
this is a ghost bounce pair. The ray then continues to the sensor plane,
where its contribution is accumulated in the output image.

Because the tracing is based on the lens geometry, ghosts exhibit:

- **Correct position** — each ghost moves according to the optical surfaces,
  rather than sliding a fixed image across the frame.
- **Chromatic aberration** — different wavelengths refract and reflect
  differently, producing colour-dependent ghost shapes.
- **Aperture shape** — circular or polygonal pupil samples influence the
  footprint of each ghost.
- **Fresnel weighting** — reflections are weighted by surface reflectance,
  including the modeled anti-reflection coatings.

The optical ghost renderer uses geometric ray tracing. Diffraction,
starburst, and veiling haze are separate components. The FFT-based
diffraction calculation is **not** a complete wave-optical simulation of
every internal lens reflection.

The Windows GPU implementation uses the CUDA stream supplied by the After
Effects host. A CPU render path is also provided. Native Vulkan and macOS
Metal implementations are not currently available.

---

## Installation

See **[INSTALL.md](INSTALL.md)** for step-by-step installation and source
build instructions, including troubleshooting.

**Quick version:** download the Windows ZIP from
[GitHub Releases](https://github.com/ken-eizo/flaresim_ae/releases), extract
`FlareLensAE.aex` into the After Effects Plug-ins folder, and restart After
Effects. This is a developer-preview build, not a fully validated stable release.
The 1,370 lens prescriptions are already embedded in the `.aex`, so the
included `lenses/` directory is optional reference data.

To build from source, obtain the Adobe After Effects SDK and NVIDIA CUDA
Toolkit, then compile with CMake as described in the installation guide.

**Requirements:**
- **Platform:** Windows x64 (current source build)
- **Host:** Adobe After Effects with a compatible native effect plug-in API;
  the newest build still needs manual host validation
- **Build tools:** CMake 3.24+, Visual Studio 2022 with the Windows SDK
- **GPU:** NVIDIA CUDA for GPU rendering; CPU rendering is also available
- **SDK:** Adobe After Effects C++ SDK, downloaded separately

---

## Quick start

1. Apply **FlareSimAE** to the footage or precomposition containing your light
   sources. To use automatic detection, the bright pixels must be present in
   the **effect's own input layer**.
2. Open **Browse All Nuke Lenses** to choose from the 1,370 embedded
   prescriptions, or select **Custom Lens** to edit the optical surfaces.
3. Set **Horizontal FOV**, or enable **Use Sensor Dimensions** to enter
   sensor width, height, and camera focal length.
4. For automatic sources, enable **Detect Bright Sources** and lower
   **Source Threshold** until the intended highlights register. Select
   **Sources Preview** in **Output Mode** to see the detected positions
   without running the expensive ghost render.
5. Adjust **Flare Gain**, **Ray Grid**, and **Spectral Samples**. For a quick
   manual test, leave detection off and use **Light Position** and
   **Manual Source Radiance**.
6. Use **Ghost Pair Selection** to render all pairs, a selected pair, or all
   pairs except the selected one.

**Note:** Source Threshold uses scene-linear luminance. Its default value of
`2.0` is above ordinary SDR white (`1.0`). Use sufficiently bright HDR input
or reduce the threshold while testing.

---

## Output channels

Unlike the Nuke version, which can write separate named channel groups such
as `flare.rgb` and `source.rgb`, the After Effects version exposes its
components through **Output Mode**.

| Output Mode | Contents |
|-------------|----------|
| **Composite** | Input image combined with the enabled flare components |
| **Flare Only** | Flare contribution without the original image |
| **Sources Preview** | Markers at detected or manual light positions; skips optical ghost tracing |
| **Ghost Only** | Lens-reflection ghosts |
| **Haze Only** | Veiling haze |
| **Starburst Only** | Aperture diffraction pattern |
| **Ray Hits Preview** | Diagnostic view of optical ray hits |
| **Optical Diagnostics** | Diagnostic rendering for troubleshooting |

These are selectable outputs of the same effect, **not** additional Nuke-style
named image channels. Some diagnostic and starburst modes use the CPU path
rather than the CUDA render path.

---

## Parameters

### Ghost

**Light Position**
Position of a manual light source in the composition. This supports
off-frame source placement.

**Manual Source Radiance**
Scene-linear HDR intensity of the manually positioned source. This is
independent of the UI colour swatch; small values may produce nearly
invisible ghosts after multiple Fresnel reflections.

**Manual Source Diameter (px)**
Reference diameter used to model a finite-sized manual light. It is not
an independent bitmap flare texture.

**Light Tint**
RGB tint applied to the source contribution.

**Flare Gain**
Overall intensity multiplier for ghost reflections. Because a ghost path
involves two reflections, physically weighted contributions may be very
small. Use this separately from source radiance when adjusting appearance.

**Ray Grid**
Number of entrance-pupil samples per dimension. A value of `N` produces
`N²` samples for each source and ghost-pair combination. Higher values
can produce smoother ghosts but increase ray-tracing work quadratically.

**Pupil Sampling**
Sampling patterns: *Regular*, *Stratified*, *Halton*, and *Golden Angle*.

**Jitter Seed / Auto Jitter Seed**
Control the stochastic pupil-sampling pattern where relevant. Automatic
seeding uses the frame time; the resulting variation should be considered
when choosing temporal quality settings.

**Detect Bright Sources**
Enables detection of highlights in the effect's input image rather than
using only the manual light.

**Source Threshold**
Minimum scene-linear luminance for automatic source detection. Raising
this suppresses dim highlights and can greatly reduce rendering time.

**Maximum Sources**
Hard limit on the number of detected sources (1–2048). The brightest
candidates are retained when the limit is reached.

**Source Cap**
Optional limit on source luminance. `0` disables the cap.

**Scan Step**
Tile size used by automatic source scanning. The implementation averages
colour within tiles rather than treating each pixel as a separate light.
Smaller values can find more detail but may generate more sources.

**Cluster Radius**
Optional radius for combining nearby bright-source candidates.
`0` disables clustering. This is not full image-region segmentation.

**Gate Detection with Input Alpha**
Optionally attenuates detection by source alpha.

---

### Preview Mode

**Use Preview Quality**
Overrides the render settings with lighter preview values.

**Preview Ray Grid / Preview Max Sources / Preview Scan Step /
Preview Spectral**
Lower-cost counterparts to the corresponding production settings. The
automatic-source preview includes compensation for the reduced detection
sampling density where applicable.

**Adaptive Quality for Reduced Resolution**
Can lower optical sampling for reduced-resolution After Effects previews.
Full-resolution output retains its configured final-quality settings.

---

### Camera

**Horizontal FOV**
Horizontal field of view in degrees, used to convert source positions
into the ray angles seen by the lens.

**Vertical FOV Override**
Optional vertical field of view. When disabled, the image aspect ratio
determines the vertical coverage.

**Use Sensor Dimensions**
Computes field of view from physical camera specifications instead of
directly entering the FOV.

**Sensor Width mm / Sensor Height mm / Camera Focal Length mm**
Physical camera parameters used with **Use Sensor Dimensions**.

---

### Aperture

**Aperture Blades**
`0` represents a circular aperture. Values from `3` to `16`
represent polygonal apertures. The setting influences ghost footprints
and the generated diffraction pattern.

**Aperture Rotation**
Rotates the polygonal aperture in degrees.

---

### Spectral

**Spectral Samples**
Number of wavelength samples used for ghost tracing:
`3 RGB`, `5`, `7`, `9`, or `11` wavelengths.
More wavelengths can provide smoother colour separation at higher cost.

---

### Post-process

**Ghost Blur Radius / Ghost Blur Passes**
Controls smoothing of the ghost contribution after ray tracing.

**Veiling Haze Gain / Haze Radius / Haze Blur Passes**
Controls the broad scattered-light component. A gain of `0` disables it.

---

### Starburst

**Diffraction Gain**
Controls the starburst/diffraction component. A gain of `0` disables it.

The diffraction pattern is generated from the aperture using an FFT-based
Fraunhofer model. This models aperture diffraction but should not be
confused with a full wave-optical treatment of lens-internal ghosts.

**Diffraction Scale**
Controls the spatial extent of the diffraction pattern.

---

### Diagnostics

**Development Host Trace**
Enables diagnostic host logging for source detection, optical
contributions and GPU/CPU rendering paths.

**Ray Hits Preview / Optical Diagnostics**
Available through **Output Mode**. These help distinguish an optical
intersection problem from a brightness or exposure problem.

---

### Output

**Output Mode**
Selects the image components displayed in the effect output. See
[Output channels](#output-channels) for the full list.

---

## Ghost pair selection

The original Nuke plugin has a dedicated Pairs tab. The After Effects
version instead exposes **Ghost Pair Selection** in its native effect
controls:

- **All Pairs** — render all active pairs from the selected lens.
- **Only Selected Pair** — render a single pair for inspection.
- **Exclude Selected Pair** — render all active pairs except one.

**Front Surface Index / Rear Surface Index**
Identify the selected reflecting surfaces using the AE control's
1-based indices. These are converted internally to optical surface
indices.

The current AE UI does not provide the Nuke plug-in's full per-pair
checkbox list. Selecting fewer pairs can dramatically reduce render time.

---

## Lens files

FlareSimAE retains the original Nuke `.lens` prescription format.
Each file describes an ordered set of optical surfaces from the front
of a lens towards the sensor:

```text
name: Example Lens
focal_length: 50.0

surfaces:
# radius  thickness  ior    abbe   semi_ap  coating
  40.0     5.0      1.670   47.2    24.0      1
 -80.0     3.0      1.000    0.0    24.0      0
  stop     4.0      1.000    0.0    14.0      0
  ...
```

| Column | Meaning |
|--------|---------|
| `radius` | Signed radius of curvature in millimetres; `0` is planar |
| `thickness` | Axial distance to the next surface |
| `ior` | Refractive index of the medium following the surface |
| `abbe` | Abbe number describing dispersion |
| `semi_ap` | Clear semi-diameter of the optical surface |
| `coating` | Surface coating setting |

The original 1,370 prescriptions are preserved in
[upstream/lenses/lens_files/](upstream/lenses/lens_files/).
At build time they are embedded in the `.aex` as read-only resources,
so installation does **not** require copying 1,370 separate files.

**Lens Selection**
Switches between the embedded **Complete Nuke Catalog** and the
editable **Custom Lens**. Use **Browse All Nuke Lenses** to find
catalog entries by name.

**Custom Lens / Initialize Custom Lens**
The AE-native editor can modify optical surfaces. **Initialize Custom
Lens** copies the currently selected catalog prescription into the
editable document. Its data is saved inside the AE project with
Arbitrary Data.

Numeric fields can interpolate between AE keyframes. Changes in the
number of surfaces use hold interpolation to avoid matching unrelated
optical surfaces. These callbacks have passed automated tests; the
complete timeline UI/save-and-reopen workflow still needs manual AE
validation.

---

## Render behaviour

FlareSimAE is a native After Effects SmartFX plug-in, not a Nuke node.
The host may request different regions, resolutions, frames, or multiple
frames concurrently. Its automatic source detector needs access to the
effect's input image, while the optical renderer projects contributions
to the final frame.

The CUDA path renders using a host-managed CUDA stream. Unsupported
render modes can fall back to CPU. Render cancellation, frame
evaluation, cache behaviour, multi-frame rendering and actual
timeline frame times depend on After Effects and require in-host
testing in addition to standalone GPU benchmarks.

The latest Windows source build passed **24/24 automated tests**, but
that result does **not** establish full validation in After Effects.

---

## Performance tips

- **Ray Grid** is a major render-time lever. Increasing it from 64 to
  512 multiplies pupil samples by 64.
- **Source Threshold / Maximum Sources** prevent accidental
  runaway renders on footage with hundreds of bright highlights.
- **Ghost Pair Selection** makes it possible to isolate one pair when
  studying a lens or adjusting optical parameters.
- **Preview Quality** can use a smaller pupil grid and fewer spectral
  samples for interactive work.
- **Lens complexity** matters: the full ray-tracing workload is roughly
  proportional to `grid² × sources × ghost pairs × wavelengths`.

On an RTX 2070 SUPER, a standalone 1920×1080 CUDA test with Ray Grid
512 took about **2.55 ms** for one ghost pair / one source / three
wavelengths, but approximately **87–112 ms** for 78 pairs. These are
test measurements, **not** guaranteed AE timeline render times.

See [GPU performance](docs/GPU_PERFORMANCE.md) for conditions and
limitations.

---

## Development

The current source supports Windows x64 builds with Visual Studio,
CMake, and the Adobe After Effects SDK. CUDA acceleration is optional
at build time; no proprietary Adobe SDK files are included in this
repository.

See [INSTALL.md](INSTALL.md) for compilation,
[Architecture](docs/ARCHITECTURE.md) for the rendering system, and
[Keyframe animation](docs/KEYFRAME_ANIMATION.md) for the saved custom
lens data format.

The project is licensed under the MIT License, retaining the upstream
copyright and attribution notices. See [LICENSE](LICENSE) and
[NOTICE.md](NOTICE.md).
