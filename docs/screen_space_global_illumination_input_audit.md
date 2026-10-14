# Screen-Space Global Illumination: Milestone 0 Input Audit

Date: 2026-09-12  
Status: complete, source audit only; no rendering behavior changed

## Gate status

The software reference tracer has passed the design's prerequisite. Its desktop
implementation report marks progressive accumulation (Milestone 5), separate
linear-HDR direct/indirect captures (Milestone 6), and portal transport
(Milestone 8) complete. The final automated run passed 17/17 cases. Laptop and
human visual acceptance remain outstanding, but they do not prevent an input
audit. Before quantitative SSGI work begins, retain a long-converged,
fixed-camera `indirectRadiance` PFM for the chosen portal-free comparison scene.

Evidence: [reference milestone status](gi_desktop_implementation_report.md#final-milestone-status),
[capture format](gi_desktop_implementation_report.md#known-limitations-and-explicit-non-goals),
and [final run](gi_desktop_implementation_report.md#final-observed-result-2026-09-12).

## Current raster inputs

| Input | Current state | Format and space | SSGI readiness |
| --- | --- | --- | --- |
| Depth | Dedicated sampled prepass depth exists. The runtime selects the first supported format from `D32_SFLOAT`, then `D16_UNORM`. It is cleared to 0 and uses reversed depth (near 1, far 0). Only opaque main-camera surfaces are drawn. | Vulkan NDC depth `[0,1]`; full-window allocation, live `_drawExtent` region | Ready for ray marching after the reconstruction milestone verifies it independently. Background is `depth <= 0`. Transparent surfaces are intentionally absent. |
| Normals | Dedicated sampled normal target exists and is cleared to zero. | `R16G16B16A16_SFLOAT`; unencoded normalized **view-space** XYZ, alpha 1 on covered pixels | Ready. SSGI should initially operate in view space to avoid an extra per-sample transform and match SSAO. |
| Albedo | No albedo G-buffer target exists. Base color exists only transiently in the forward fragment shader as vertex/material tint times a sampled texture, including the procedural debug checker. | Scene textures are uploaded as `R8G8B8A8_SRGB`, so texture sampling decodes them to linear before tinting. No image stores the result. | Missing; must be added in Milestone 1. |
| Lit color | `_drawImage` is a sampleable `R16G16B16A16_SFLOAT` image. Before portal composition and FXAA it contains linear background plus forward-lit world color. | Linear HDR before the final copy to the `B8G8R8A8_UNORM` swapchain. Raster mode has no explicit exposure/tone-map/gamma pass. | A current-frame source exists, but no previous-frame raster history exists. The image is discarded/overwritten each frame and cannot be read while it is also the color target. A separate history/source image is required. |
| Motion vectors | No render target, shader output, previous camera matrix, or previous object transform exists. The `velocity` fields found in camera/player code are gameplay state, not screen-space motion vectors. | None | Missing; required for moving-camera temporal accumulation. |
| Portal mask | Portal silhouettes are encoded transiently as nonzero IDs in the stencil aspect of the main depth/stencil attachment. That attachment is attachment-only, not sampled. There is no sampled mask image. | Main depth/stencil is selected from `D32_SFLOAT_S8_UINT` or `D24_UNORM_S8_UINT`; stencil values identify visible portal regions and recursive levels. | Missing as an SSGI input. Add a dedicated sampled binary mask rather than making SSGI interpret portal IDs. |

Primary evidence: [`init_depth_normal_resources()`](../src/vk_engine_renderer.cpp),
[`draw_depth_normal_prepass()`](../src/vk_engine_renderer.cpp),
[`depth_normal.vert`](../shaders/depth_normal.vert),
[`depth_normal.frag`](../shaders/depth_normal.frag),
[`mesh.frag`](../shaders/mesh.frag),
[`load_scene_texture()`](../src/vk_engine_scene_materials.cpp),
and [swapchain/draw-image creation](../src/vk_engine.cpp).

## Projection and reconstruction contract

The main projection is a 70-degree vertical perspective projection built with
far and near arguments reversed (`10000`, `0.1`), and its Y row is negated for
Vulkan. `GPUSceneData` already carries `inverseProjection` and
`inverseViewProjection`.

The established reconstruction is:

```glsl
vec4 clip = vec4(screenUV * 2.0 - 1.0, sampledDepth, 1.0);
vec3 viewPosition = (inverseProjection * clip).xyz /
                    (inverseProjection * clip).w;
vec3 worldPosition = (inverseViewProjection * clip).xyz /
                     (inverseViewProjection * clip).w;
```

In implementation, evaluate each matrix multiplication once before dividing.
No manual Y flip, Z remap, or reversed-depth inversion is applied: those are
already represented by the stored projection. `ssao_common.glsl` and the
existing position debug views are the authoritative convention. Prepass images
are allocated at window size but only the `_drawExtent` live region is written;
SSGI UVs and dispatch bounds must preserve the existing render-scale mapping.

Evidence: [`build_scene_data()`](../src/vk_engine_renderer.cpp),
[`ssao_common.glsl`](../shaders/ssao_common.glsl), and
[`render_debug.frag`](../shaders/render_debug.frag).

## Portal composition and mask insertion point

Raster order is currently shadow map, main-camera depth/normal prepass, SSAO,
background, forward main geometry, portal stencil/depth masks, portal views,
debug overlay, FXAA, and presentation. Portal views render into `_drawImage`
through stencil, or optionally render to offscreen camera images and are copied
by an unlit portal composite shader.

The sampled prepass is deliberately independent of the portal-mutated main
depth/stencil image. Consequently it continues to describe the opaque host wall
behind a visible portal, not the destination scene. This is safe only if every
SSGI ray entering the portal footprint terminates before consulting that depth.

Milestone 1 should add a full-resolution `R8_UNORM` sampled portal-mask image,
clear it to zero each frame, and stamp 1 for visible portal silhouettes adjacent
to `draw_portal_masks()`. Reuse the existing portal quads, main depth/stencil
visibility, render extent, and stencil decisions. Do not derive the mask from
portal color, copy raw stencil IDs into the SSGI contract, or put destination
depth/normals into the main-camera prepass. Recursive portal interiors remain
masked as one binary region under Option A.

Evidence: [`draw_portal_masks()`](../src/vk_engine_portals.cpp),
[portal pipeline state](../src/vk_engine_materials.cpp), and
[`portal_mask.vert`](../shaders/portal_mask.vert).

## Milestone 1 resource decisions

### Albedo and velocity

Add albedo and velocity as additional outputs of the ordinary forward geometry
pass, not the material-free depth/normal prepass. The forward shader already has
the fully evaluated linear base color; producing it in the prepass would require
adding material descriptors and repeating texture/checker evaluation.

- Albedo: full-resolution `R8G8B8A8_UNORM`, sampled plus color-attachment use.
  Store linear base color before lighting. Alpha may initially be 1 for the
  opaque-only SSGI contract.
- Velocity: full-resolution `R16G16_SFLOAT`, sampled plus color-attachment use.
  Store current-to-previous screen UV displacement with one documented sign
  convention.
- Generalize `PipelineBuilder` and the dynamic-rendering helper from their
  current single-color-attachment API to MRT. Attach albedo and velocity only
  to the main opaque forward pipeline/pass; portal views are masked out and do
  not need valid entries in these buffers.
- Add previous view-projection state and previous per-object world transforms.
  `GPUSceneData`, `GPUDrawPushConstants`, and `RenderObject` currently contain
  only current transforms. Stable object history is needed for editor motion;
  scene loads, resize/render-scale changes, renderer-mode switches, camera or
  object teleports, and portal traversal must invalidate temporal history.
- Clear uncovered albedo and velocity to zero. Keep transparent surfaces out of
  the initial SSGI contract, matching the depth/normal prepass.

This is a medium-sized geometry-pipeline change rather than a shader-only
addition. The engine's pipeline builder and rendering-info helper each assume a
single color attachment today.

Evidence: [`GPUDrawPushConstants`, `GPUSceneData`, and `RenderObject`](../src/vk_types.h),
[`mesh.vert`](../shaders/mesh.vert), [`mesh.frag`](../shaders/mesh.frag), and
[single-attachment pipeline setup](../src/vk_pipelines.cpp).

### Linear color history

Keep `_drawImage` as the displayed/composited raster destination. Add explicit
ping-pong linear-HDR source/history images for SSGI instead of treating
`_drawImage` or `_postProcessImage` as history. `_postProcessImage` is an FXAA
destination and may not be produced at all; `_drawImage` begins each raster
frame with an undefined-layout discard and is later modified by portals and
debug overlays.

For the first reference-comparison implementation, use a **direct-only source**
with no previous indirect feedback. This makes the bounce count and brightness
diagnosable against a controlled reference capture. Store/swap previous-frame
direct lighting explicitly, then evaluate temporal feedback as a separately
named quality option after the single-bounce baseline passes. Do not sample a
post-FXAA, post-portal, debug, or swapchain image as incident radiance.

## SSAO, ambient, and composition decision

The current fragment shader bakes three quantities into one color:

```text
baseColor * (0.28 * SSAO + shadowed Lambertian sunlight)
```

That makes the current `_drawImage` unsuitable as a clean direct-only source:
the hardcoded ambient is already an indirect-light stand-in, and SSAO has
already darkened it.

Recommended resolution for the later compositing milestone (not a Milestone 1
change): SSGI validation mode should:

1. provide a direct-only raster-lighting source that excludes the hardcoded
   `0.28` ambient term, while preserving the existing ambient behavior when
   SSGI is disabled;
2. composite `direct + SSGI indirect` at intensity 1.0;
3. disable SSAO initially, so the comparison contains no unquantified
   double-darkening.

This records the proposed answer to Part 9's open question; it does not authorize
removing ambient lighting from the current raster renderer. The actual removal
or mode-gating belongs to the compositing milestone and should be evaluated
against the reference first. After validation, SSAO may be reintroduced only as
modulation/compensation for the SSGI fallback term, behind an explicit setting
and a debug view. It must not multiply screen-space hit lighting or the final
direct-plus-indirect result.

Evidence: [`build_scene_data()`](../src/vk_engine_renderer.cpp),
[`mesh.frag`](../shaders/mesh.frag), and
[`input_structures.glsl`](../shaders/input_structures.glsl).

## Required Milestone 1 debug views

Before SSGI consumes any new image, extend `RenderDebugView` with:

- linear albedo;
- motion vector direction/magnitude, plus a zero-motion check at a fixed camera;
- portal mask overlay and mask-only view;
- direct-only linear HDR source (display-mapped for inspection, never used for
  numerical comparison after display mapping).

Existing depth, view-normal, view-position, and world-position views remain the
verification path for current inputs. Resource creation must query required
format features, follow the existing render-scale live-region convention, and
recreate or invalidate dependent history on extent changes.

## Milestone 0 conclusion

Available now: opaque main-camera sampled depth, view normals, reconstruction
matrices, and a current-frame linear-HDR render target.

Must be added before ray marching: linear albedo, screen-space motion vectors,
a sampled binary portal mask, a clean direct-light source/history pair, and the
MRT/previous-transform plumbing that produces them. The existing raster ambient
and SSAO are flagged for resolution before they can be used in an SSGI
validation source; neither is changed by this audit. Milestone 1 must verify each
new buffer in isolation; no ray-march implementation should begin until those
views pass.
