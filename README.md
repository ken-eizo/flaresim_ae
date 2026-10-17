# FlareSimAE

**Physically based lens flares for Adobe After Effects.**

FlareSimAE is an open-source After Effects port of [FlareSim for Nuke](https://github.com/LocalStarlight/flaresim_nuke). It traces light through modeled lens surfaces to produce optical ghost reflections, with 1,370 lens prescriptions built in.

- **Windows:** NVIDIA CUDA GPU acceleration.
- **macOS:** native Metal GPU acceleration (Universal arm64 / x86_64).
- Both platforms support CPU fallback for features that do not yet run on the GPU.

## Download and install

**[Download the latest release](https://github.com/ken-eizo/flaresim_ae/releases/latest)** · **[Installation instructions](INSTALL.md)**

Download the ZIP for your operating system. Install only `FlareLensAE.aex` (Windows) or `FlareSimAE.plugin` (macOS) into your After Effects Plug-ins folder.

The macOS build is ad-hoc signed, not Apple-notarized. Its Metal GPU implementation has passed Apple GPU tests, but in-host After Effects operation on macOS has not yet been independently verified.

## Quick start

1. Apply **FlareSimAE** to a layer.
2. Select a lens using **Browse All Nuke Lenses**.
3. For a manual light, adjust **Light Position** and **Manual Source Radiance**. Alternatively, use **Detect Bright Sources** to find highlights **in the effect's input layer**.
4. Adjust **Flare Gain** and **Ray Grid** to control the result.

CUDA and Metal accelerate **Composite**, **Flare Only**, **Sources Preview**, **Ghost Only**, and **Haze Only**. Diffraction/starburst and optical diagnostics still use CPU fallback.

For settings, diagnostics and performance advice, see the **[User Guide](docs/USER_GUIDE.md)**. Developer information is in **[docs/](docs/)**.

## Credits and license

Based on **[FlareSim for Nuke](https://github.com/LocalStarlight/flaresim_nuke)** by **Steve Watts Kennedy (LocalStarlight)**, built upon the optical rendering work of **Eamonn Nugent (space55)**: [blackhole-rt](https://github.com/space55/blackhole-rt).

After Effects adaptation: [ken-eizo](https://github.com/ken-eizo).

Released under the MIT License. See [LICENSE](LICENSE), [upstream/LICENSE](upstream/LICENSE), and [NOTICE.md](NOTICE.md). This is an unofficial adaptation, not endorsed by the upstream authors or Adobe.
