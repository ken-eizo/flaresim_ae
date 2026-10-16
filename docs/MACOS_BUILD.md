# macOS builds (GitHub Actions)

The repository has a **macOS CPU optical build path** and a GitHub Actions
workflow at [`.github/workflows/macos.yml`](../.github/workflows/macos.yml).

## Current limitations

The macOS port is a native **CPU-only** After Effects effect. It is not a
CUDA, Metal, or Vulkan renderer. macOS users must not expect the same GPU
performance as the Windows CUDA binary. Native AE host compatibility
remains unverified until a complete macOS build and an After Effects host
test have succeeded.

CI has two jobs:

1. **macOS CPU optical core**: always compiles and runs portable tests on
   GitHub's macOS runner. **No Adobe SDK is involved.** A pass proves only
   that the portable optics/tests build, not that the plugin is loadable.
2. **macOS native AE plugin (CPU)**: runs only when the owner enables
   `FLARESIM_MAC_SDK_READY=true` as a GitHub Actions repository variable.
   Downloads the owner's separately acquired Mac AE SDK to the runner's
   temporary folder and compiles an unsigned/adhoc-signed universal
   `FlareSimAE.plugin` for `arm64+x86_64`. If successful, uploads a
   short-lived **GitHub Actions artifact** (not a public Release).

Do not mark the macOS plugin supported or attach it to a stable Release
until native AE tests establish its actual behavior.

## Adobe SDK inputs

The two paths supplied by the developer are:

- Windows SDK: `E:\ae25.6_61.64bit.AfterEffectsSDK`
- macOS SDK: `E:\AfterEffectsSDK_25.6_61_mac`

**GitHub-hosted Mac runners cannot access local Windows E: drives.**

The macOS directory contains a file named

```text
AfterEffectsSDK_25.6_61_mac/ae25.6_61.64bit.AfterEffectsSDK.tar.zstd.zip
```

Despite the filename extension `.zip`, its magic header is a
**Zstandard frame** (28 B5 2F FD), not a ZIP container.

SHA-256 observed on the provided Mac SDK payload:
```text
e02fa2b488c3cceb238866b648eb9a2526d308a260744367915a2f173663c36c
```

The pipeline checks this exact digest before extraction, then uses a trusted
runner-installed `zstd` decoder. The included third-party `zstd` binary
from the SDK folder is **never executed** in CI. The pipeline also does
not commit or upload SDK headers, binaries, resources or archives to the
public repository.

Adobe's developer SDK distribution terms still apply. Only an SDK
supplied through an authorized private source should be used.

## Enabling the native plugin job

1. Host the original 1,540,471-byte Mac SDK Zstandard payload in a **private
   HTTPS storage location** for which you have permission to provide access
   to your own GitHub Actions runner.
2. In GitHub **Settings → Secrets and variables → Actions → Secrets** add
   `AE_SDK_MAC_URL` containing a working, private/signed HTTPS download URL.
   The workflow never prints this URL.
3. Under **Variables**, set `FLARESIM_MAC_SDK_READY` to `true`.
4. Open **Actions → macOS build → Run workflow**. Check the two jobs and the
   `FlareSimAE-macOS-CPU-unverified` artifact.

Until those credentials exist, the plugin job is intentionally skipped
rather than falsely claiming that the AEX compiled for Mac.

If the signed URL expires, update the secret and rerun. GitHub's artifacts
are ephemeral; a validated macOS Release must be prepared separately.

## Local Mac build

Once the owner has legitimately extracted the Mac AE SDK on a Mac:

```sh
cmake -S . -B build-macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES='arm64;x86_64' \
  -DAE_SDK_ROOT='/path/to/ae25.6_61.64bit.AfterEffectsSDK/Examples' \
  -DFLARELENS_BUILD_AE=ON -DFLARELENS_ENABLE_CUDA=OFF
cmake --build build-macos --target FlareLensAE
```

The build is intended to produce `FlareSimAE.plugin`. GitHub Actions
additionally runs a binary-architecture and PiPL resource check.

## Platform specifics

- Windows uses compiled Win32 `RCDATA` for its 1,370 catalog lenses.
- macOS uses a generated, read-only C++ byte table holding the **same
  exact 1,370 source texts** and stable catalog indices.
- The Windows modal `DialogBox` lens browser is replaced by a
  user-triggered Cocoa combo-box picker.
- The existing effect Match Name and parameter indices are preserved.
- macOS native `Rez` compiles `FlareLensPiPL.r` for the effect bundle.
- CUDA GPU selectors and GPU flags are omitted on macOS; optical rendering
  falls back to the CPU.
- Unsigned or ad-hoc signed GitHub Actions artifacts are **not notarized**.
  Do not disable OS security controls simply to install an untested artifact.

## Next improvements

After the first successful Mac plugin CI, verify packaging, signing,
After Effects host compatibility, 8/16/32-bpc CPU renders, MFR, UI
interactions, and project save/reopen on actual Apple Silicon and Intel
machines. Metal acceleration requires a separate native renderer and
measurement against CPU/CUDA, not an automatic translation.
