# FlareSimAE 1.0.1 — Windows CUDA dispatch regression

## Why 1.0.0 can feel slow immediately after adding the effect

The Windows v1.0.0 plugin includes CUDA, but its SmartFX GPU eligibility
logic disabled CUDA for **partial output frame requests with a manually
positioned light**, even when the scene was otherwise GPU-compatible.

After Effects may request a tile during initial viewing, composition
changes, gizmo interaction, cached previews, or multi-frame rendering.
The CPU fallback then executes the full lens ghost calculation, which can
be much slower.

A second check inside the GPU callback unconditionally rejected manual
light source tiles, so fixing only the eligibility flag was insufficient.

## Fix

- For a CUDA-eligible output and a valid in-frame tile, SmartPreRender
  requests the **full input sensor** instead of just the tile.
- SmartPreRender advertises GPU rendering for manual-light, in-frame
  partial output requests.
- The GPU callback accepts those tiles, calculates the same
  **full optical sensor**, and copies the requested tile into the host's
  GPU output. No optical-space tiling approximations are introduced.
- The result rectangle still respects AE's render request. Invalid or
  entirely outside-sensor requests never advertise CUDA rendering.
- When the host does not provide CUDA, CPU rendering retains its
  ROI-specific input checkout and remains the fallback.

## Sources Preview on CUDA

The lightweight output mode **Sources Preview** now runs on CUDA:

- Full-frame and tiled modes.
- Manually positioned lights or detected HDR sources.
- A low-cost per-pixel RGB attenuation pass and a bounded
  per-source marker kernel; no optical ray tracing.
- Opaque marker alpha, matching the CPU representation.

## Validation

The native Windows AEX was tested by calling its **actual**
PF_Cmd_SMART_PRE_RENDER and PF_Cmd_SMART_RENDER_GPU handlers, using a
host-mock GPU context with a real NVIDIA CUDA stream.

- Full-frame and tiled modes: Composite, Flare Only, Sources Preview,
  Ghost Only, Haze Only.
- GPU vs CPU optical pixel comparison for manual light tiles:
  relative L1 error under 0.1% for tested frames.
- Sources Preview pixel comparison: exact agreement for the tested
  manual and automatically detected source configurations.
- No leaked host GPU device allocations under test.
- The full CTest suite is required before releasing the ZIP.

This confirms callback-level parity and functional CUDA dispatch in a
mock host. It is **not** an actual After Effects GUI/time-line performance
measurement, nor does it guarantee that every host request selects CUDA.

## Still unsupported on CUDA

- Starburst/diffraction compositing: enabling Diffraction Gain still
  selects the CPU optical rendering path.
- Starburst Only, Ray Hits Preview, Optical Diagnostics are CPU outputs.
- Native Metal macOS rendering is not implemented.
- A complete per-frame launch/ROI reuse/cache framework requires
  additional host measurements.

The Windows release should NOT claim every feature is GPU accelerated.

## Existing Windows plugin installation

The original AEX located under Adobe's installed plugin directories is
**not replaced by the repository build command**. After Effects must be
closed before changing the active AEX. Remove duplicate copies and
install the newly tested plugin to observe any performance difference.

Use **Development Host Trace** to distinguish GPU_ELIGIBILITY,
GPU_ENTER, GPU_DONE and CPU_BEGIN in real host sessions.
