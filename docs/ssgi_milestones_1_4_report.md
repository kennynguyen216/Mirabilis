# SSGI Milestones 1-4 Implementation Report

Date: 2026-09-12  
Status: implemented and runtime-validated; raw output intentionally unfiltered

Continuation: [SSGI Milestones 5-9 Implementation Report](ssgi_milestones_5_9_report.md).
The scope statement below describes the Milestone 4 checkpoint, not the current
renderer state.

## Visible result

The raster renderer now exposes a one-sample, full-resolution indirect-lighting
image through **Render Settings > Screen-Space Buffers > Debug View > SSGI (raw
noisy indirect)**. It is deliberately noisy and changes every frame. No temporal
accumulation, spatial filtering, or gameplay-lighting composite is included;
those belong to later milestones.

Related debug views are available beside it:

- Albedo (linear)
- Motion vectors
- Portal mask
- Direct lighting source
- SSGI hit/miss (green hit, blue miss, magenta portal termination)
- SSGI steps (blue through green to red as the budget is consumed)

For an unattended launch directly into the raw view, set
`MIRABILIS_RENDER_DEBUG_VIEW=12`. Values 8-14 select the new views in enum order.

## Milestone 1: G-buffer completion

Complete for the initial opaque SSGI contract.

- The main forward pass now uses four render targets: existing lit HDR color,
  `R8G8B8A8_UNORM` linear albedo, `R16G16B16A16_SFLOAT` current-to-previous UV
  velocity, and `R16G16B16A16_SFLOAT` direct-only lighting.
- The pipeline and dynamic-rendering helpers now support MRT while portal and
  offscreen passes remain single-target.
- Motion includes previous camera and object transforms. Previous affine object
  transforms fit the guaranteed 128-byte push-constant budget as three rows.
- Two HDR direct-light history images ping-pong. The current direct target is
  copied after forward shading and the SSGI pass samples the prior side.
- A sampled `R8_UNORM` portal mask is cleared and stamped from the existing
  visible portal geometry before SSGI. Linked and recursive portal interiors
  are one binary Option-A exclusion region.
- Required image format capabilities are checked at startup.

Transparent surfaces remain outside the contract, matching the existing opaque-
only depth/normal prepass.

## Milestone 2: position reconstruction

Complete and kept on the engine's existing convention. SSGI reconstructs view
position from reversed Vulkan depth using `inverseProjection`, with no manual Y
flip or depth inversion. Existing camera-depth, view-position, and world-position
debug views remain available for cross-checking.

## Milestone 3: single-sample ray march

Complete as an explicit first baseline.

- One cosine-weighted hemisphere direction is generated per covered pixel and
  frame.
- The ray starts at `P + N * startOffset`.
- The first implementation uses uniform **view-space** steps. This is simpler
  to validate than perspective-correct screen-space stepping; step count and
  ray length are exposed in the UI.
- A hit occurs when the ray passes behind reconstructed scene depth but remains
  within the fixed/depth-scaled thickness interval.
- Screen exit, projection exit, background, and budget exhaustion miss to the
  fallback. Entering the portal mask terminates immediately to the fallback.
- Hit/miss and normalized steps-used diagnostics are written alongside raw GI.
- Binary hit refinement is not yet enabled.

Defaults: 32 steps, 12 world/view units of ray length, 0.35 thickness, and 0.08
normal start offset.

## Milestone 4: lighting evaluation

Complete for the single-bounce baseline.

- Screen-space hits sample the previous frame's direct-only HDR history. The
  hit UV is reprojected using the current velocity buffer.
- Misses sample the existing linearized sky panorama by world-space ray
  direction.
- Cosine-weighted Lambertian sampling uses the matching PDF cancellation, so
  the estimator is `incidentRadiance * originAlbedo`.
- The first frame has no valid history and therefore uses fallback; subsequent
  frames use screen-space hits.
- Existing raster ambient and SSAO behavior is unchanged. The clean direct-only
  MRT excludes both, so they do not contaminate this SSGI estimate.

## Validation

- Debug build: passed.
- SPIR-V validation: passed for all modified/new shaders.
- Six bounded debug-view runs (albedo, velocity, portal mask, raw SSGI,
  hit/miss, steps): passed under Vulkan synchronization validation.
- Portal-pair mask run: passed under Vulkan validation with no warnings/errors.
- Full `scripts/validate_software_trace.ps1` regression: **18/18 passed**, including
  raster scenes, resize/mode switching, reference accumulation/invalidation,
  Cornell/material captures, and portal transport. Artifacts:
  `tmp/gi-checkpoints/validation-20260912-083604/`.
- `git diff --check`: passed; only existing line-ending advisories were emitted.

## Scope boundary for the next milestone

This delivery intentionally stops before temporal accumulation. Camera/object
motion vectors exist, but raw SSGI is expected to flicker because its sample
direction changes every frame. Milestone 5 must add history reprojection,
depth/normal/portal rejection, clamping, and EMA blending before this buffer is
suitable for gameplay composition.
