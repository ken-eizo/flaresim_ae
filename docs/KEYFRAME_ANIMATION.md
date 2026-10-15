# Custom Lens Keyframe Animation

## Host representation

The Custom Lens widget is backed by an After Effects `PF_Param_ARBITRARY_DATA` parameter.
AE owns the keyframe list and requests transitions through `PF_Arbitrary_INTERP_FUNC`.
The widget does **not** create separate native timeline tracks for every surface property.

The lens prescription is stored in a versioned, fixed-size `fl::custom::Document`.
The `FLATTEN` and `UNFLATTEN` callbacks save/restore this document.
The document layout and serialized version were preserved by the October 2026 animation update.

## Interpolation

- Focal length, radius, thickness, refractive index, Abbe number and semi-aperture are linearly interpolated between two keyframes with the same surface count.
- Coating/stop fields are categorical and use a discrete switch (t < 0.5 selects the left value).
- If the surface counts are different, the left prescription is held until the right keyframe. This prevents invalid surface correspondence.
- The UI-selected surface index follows the left document until the keyframe endpoint.
- Both keyframe sources are immutable; a new `PF_ArbitraryH` holds the interpolated result.

The editor's selection state is currently part of the serialized `Document`. Separating transient selection from the optical prescription is a potential future improvement.

## Verification status

The `FlareLensCustomKeyframeTest` mock invokes the real AEX arbitrary callback and checks interpolation, discrete topology, flatten/unflatten and byte-for-byte round trips.

**AE GUI tests remain outstanding:** manually verify keyframe creation, interpolation, Undo/Redo, project save/load, copying keyframes, motion blur/time remapping and concurrent MFR evaluation before calling this feature production-ready.
