# Install FlareSimAE

Download the **[latest GitHub Release](https://github.com/ken-eizo/flaresim_ae/releases/latest)** and choose the ZIP for your operating system. Each official release includes matched Windows CUDA and macOS Metal builds.

**Close After Effects before installing or replacing the plug-in.**

**Important for Mac users:** The published v1.0.3 macOS ZIP is known not to register correctly in After Effects. To test the corrected PiPL and bundle configuration, use the [v1.0.4 macOS CI artifact](https://github.com/ken-eizo/flaresim_ae/actions/runs/38993870423/artifacts/12086655999), then extract the inner `FlareSimAE_macOS_CI.zip`. This is a test build, not a formally validated macOS After Effects release.

## Windows

1. Download and extract the **Windows** ZIP.
2. Copy **`FlareLensAE.aex`** to your After Effects Plug-ins folder, for example:

   ```text
   C:\Program Files\Adobe\Adobe After Effects <version>\Support Files\Plug-ins\FlareSimAE\
   ```

3. Open After Effects, find **FlareSimAE** in Effects & Presets, and apply it to a layer.

## macOS

1. Download and extract the **macOS** ZIP.
2. Copy the **entire `FlareSimAE.plugin` bundle** (without opening or modifying its contents) into your After Effects Plug-ins folder, for example:

   ```text
   /Applications/Adobe After Effects <version>/Plug-ins/FlareSimAE/
   ```

3. Open After Effects, find **FlareSimAE** in Effects & Presets, and apply it to a layer.

The Mac build is **ad-hoc signed and not Apple-notarized**. If macOS blocks it, review the security warning and only approve the plug-in if you trust the source; do not disable Gatekeeper system-wide. macOS After Effects GUI compatibility is not yet independently verified.

## Notes

- **Only the .aex file or .plugin bundle needs to be installed.** The 1,370 lens prescriptions are embedded; no separate `.lens` files are needed.
- Make sure you do not have another active copy of FlareSimAE in any After Effects plug-in search path.
- To detect highlights automatically, apply FlareSimAE to the layer containing those bright pixels; a blank solid does not automatically sample the composition beneath it.
- For GPU operation on Windows, use an NVIDIA GPU with a compatible CUDA driver. Some modes still use CPU processing on both systems.
- Copy the plug-in to a version-specific After Effects Plug-ins folder, **not inside the application executable/bundle**. Adobe also documents MediaCore as a shared plug-in folder, but it is not the recommended default for this AE-specific effect.

For detailed controls, see [User Guide](docs/USER_GUIDE.md). For developers, see [Build from source](docs/BUILD_FROM_SOURCE.md) and [macOS build notes](docs/MACOS_BUILD.md).

License and acknowledgements are in `LICENSE.txt`, `LICENSE_UPSTREAM.txt` and `NOTICE.md` in the downloaded ZIP (and in the source repository).
