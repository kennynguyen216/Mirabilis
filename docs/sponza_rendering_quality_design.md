# Sponza Rendering Quality Design

## Purpose

This document defines the work required to turn the current Sponza render into
a high-quality still-image target for Mirabilis. The goal is not merely to
increase sample counts. The current quality ceiling comes from missing material
response, incomplete indirect-light integration, limited shadow coverage, and
an asset path that consumes too much GPU memory.

The reference target is `assets/scenes/sponza_showcase.json` using the optimized
Sponza asset on an RTX 3060 Laptop GPU. Interactive frame rate is useful for
camera placement, but the final capture preset may trade frame time for quality.

## Current state

The following pieces already exist and should be preserved:

- HDR forward target (`R16G16B16A16_SFLOAT`).
- ACES/Reinhard tonemapping and linear-to-sRGB encoding.
- A 4096 x 4096 directional shadow map with PCF and texel snapping.
- A depth/normal prepass.
- SSAO with bilateral filtering.
- SSGI with temporal accumulation and spatial filtering.
- FXAA.
- Alpha-mask coverage in the forward, portal, prepass, and shadow passes.
- An optimized Sponza copy with textures capped at 2048 pixels.
- Generated mip chains and glTF sampler mip settings for imported images.
- SSGI fallback and diagnostic images, including hit/miss classification.
- PFM capture paths for SSGI and path-trace diagnostics with metadata sidecars.

The current Sponza image is geometrically correct and reasonably sharp with
SSGI disabled. It still looks flat because the raster material path is Lambert
diffuse, does not consume most of Sponza's material textures, and has no
image-based specular or diffuse environment lighting. Enabling SSGI makes the
scene too dark because the forward shader removes its flat ambient term
unconditionally and replaces it with an indirect estimator whose magnitude is
much lower in an enclosed scene. Ray misses already receive an analytic sky
fallback unless the trace-environment option explicitly disables it.
Half-resolution temporal SSGI can also leave history trails while the camera
moves.

## Goals

1. Preserve visible fine stone, brick, wood, cloth, and metal detail.
2. Give each material a physically plausible diffuse and specular response.
3. Keep interiors readable without flattening contact shadows.
4. Prevent SSGI misses or rejected history from turning surfaces black.
5. Keep directional shadows stable and detailed across the Sponza atrium.
6. Fit the complete scene in the RTX 3060's VRAM budget.
7. Produce a clean screenshot without editor or gameplay overlays.
8. Keep portal rendering consistent with the main material implementation.

## Non-goals

- Real-time hardware ray tracing is not required for this milestone.
- Full path tracing is a reference mode, not the primary Sponza renderer.
- Animation, skinning, and general-purpose texture streaming are outside this
  milestone unless they become necessary to load the scene reliably.
- Noclip and time-trial behavior are unchanged.

## Rendering architecture

The target raster frame is:

```text
shadow cascades
    -> depth/normal/material prepass
    -> SSAO
    -> forward PBR lighting + G-buffer auxiliaries
    -> portal composition
    -> SSGI trace
    -> temporal validation and spatial filtering
    -> indirect-light composite with ambient fallback
    -> tonemapping and sRGB encoding
    -> optional FXAA
    -> clean screenshot capture or swapchain presentation
```

The renderer should be split before the larger material and shadow changes, but
the isolated base-color format correction in section 1 should land first because
it affects every subsequent lighting comparison.
Suggested translation units are:

- `vk_engine_shadows.cpp`
- `vk_engine_prepass.cpp`
- `vk_engine_ssao.cpp`
- `vk_engine_ssgi.cpp`
- `vk_engine_postprocess.cpp`
- `vk_engine_render_debug.cpp`

`vk_engine_renderer.cpp` should retain frame orchestration, common draw-list
submission, and shared scene-data construction. Member ownership can remain in
`vk_engine.h` initially so the refactor is mechanical and reviewable.

## 1. Asset and texture pipeline

### Diagnosis

The full Sponza package contains roughly 2.8 GB of compressed files. PNG file
size is not GPU size: decoded RGBA textures and mip chains can exceed the 3060's
available VRAM. The optimized 2048-pixel copy loads, but PNGs are still uploaded
as uncompressed images. All imported images currently use
`VK_FORMAT_R8G8B8A8_UNORM`, so base color is incorrectly treated as linear data.
The loader also ignores several glTF texture roles; in particular, the existing
metallic/roughness descriptor remains bound to the white fallback even when the
material supplies a metallic/roughness texture.

### Design

Classify every glTF texture by color space and purpose:

| Texture | Vulkan interpretation | Filtering |
|---|---|---|
| Base color | sRGB | trilinear + anisotropic |
| Emissive | sRGB | trilinear + anisotropic |
| Normal | linear | trilinear + anisotropic |
| Metallic/roughness | linear | trilinear + anisotropic |
| Occlusion | linear | trilinear + anisotropic |

Preserve the complete mip chains already generated by `create_image(..., true)`
and the existing glTF mip-filter sampler settings. Select the image format from
texture role: use `VK_FORMAT_R8G8B8A8_SRGB` for base color and emissive, and
`VK_FORMAT_R8G8B8A8_UNORM` for normal, metallic/roughness, and occlusion. Because
one glTF image can theoretically be referenced by roles with different color
spaces, cache uploaded images by `(image index, color-space class)` rather than
assuming one GPU image per source image.

Before enabling anisotropic samplers, query and request the Vulkan
`samplerAnisotropy` device feature. Clamp the requested level to
`maxSamplerAnisotropy` and to the selected preset's 8x or 16x cap. Fall back to
trilinear filtering when the feature is unavailable.

Add an offline asset-processing step. The preferred eventual formats are BC7
for base color/emissive, BC5 for normal maps, and BC4 or packed BC7 for scalar
material channels. KTX2 is preferable as a container if the loader gains KTX2
support. Until then, the 2048 PNG copy remains the compatibility path.

The loader must report estimated texture allocation before uploading and fail
gracefully when the requested set exceeds a configurable budget. A failed
texture should use the existing checkerboard/default resource instead of
terminating through `VK_ERROR_OUT_OF_DEVICE_MEMORY`.

### Acceptance criteria

- Sponza loads twice in one process without an allocation failure or leak.
- All referenced textures resolve; missing paths are printed by material name.
- Base-color textures match the glTF viewer's overall brightness and hue.
- A metallic/roughness debug view proves the authored packed texture is bound.
- Distant floor and arch textures remain stable at oblique camera angles.
- Peak raster mode stays within a measured VRAM budget on the RTX 3060.

## 2. Material data and normal mapping

### Diagnosis

Sponza ships with normal, roughness, metallic, emissive, and opacity data, but
the forward shader mostly uses base color and a Lambert term. Geometric detail
therefore appears baked into color rather than responding to the light.

### Data model

Keep the existing 256-byte material-constant footprint. It is used as the
dynamic-UBO stride and already contains fourteen spare `vec4` values. Name and
document fields within that footprint rather than replacing it with a smaller
structure:

```cpp
struct MaterialConstants {
    vec4 baseColorFactor;
    vec4 metallicRoughnessFactors;
    vec4 emissiveFactorAndStrength;
    vec4 occlusionNormalAlphaCutoff;
    vec4 uvTransform;
    vec4 materialFlags;
    vec4 reserved[10];
};
```

Material flags identify available textures, alpha mode, double-sided state,
and editor-only visualization. The checkerboard flag currently occupies
`metal_rough_factors.z`; migrate it to the named flags field while retaining a
compatibility transition for built-in materials. `traceParameters` in the
software path tracer is copied from `metal_rough_factors`, so any field migration
must update and validate that consumer in the same change.

Add sampled images for normal, metallic-roughness, emissive, and occlusion.
Use shared default textures so missing maps do not require shader branches:

- flat normal `(0.5, 0.5, 1.0)`;
- metallic/roughness defaults from the glTF factors;
- black emissive;
- white occlusion.

### Tangent basis

Add tangent plus handedness to `Vertex` and pass it through every relevant
vertex shader. Use glTF tangents when supplied. When a primitive has UVs and a
normal map but no tangents, generate MikkTSpace-compatible tangents during
import. Transform normal and tangent with the inverse-transpose normal matrix,
orthogonalize the tangent against the normal, and reconstruct the bitangent
from tangent handedness.

The normal-mapped world normal must feed all consumers that describe visible
surface response. Preserve the current depth/normal prepass policy: store
geometric view-space normals for stable SSAO/SSGI rejection and use
normal-mapped normals for BRDF lighting. The prepass currently stores unencoded
normals; octahedral encoding is a possible bandwidth optimization later, and
must remain consistent with the octahedral metadata already used by SSGI.

### Acceptance criteria

- A normal-map toggle produces clear relief on capitals, bricks, and floor
  tiles while silhouettes remain unchanged.
- Non-uniformly scaled objects have correct normal and tangent orientation.
- Mirrored UV islands preserve handedness.
- Portal views and the main view show identical forward material response. If a
  later indirect-light design needs albedo or direct-light MRTs, explicitly add
  those attachments to the portal pipeline or document that portals retain a
  forward-only ambient approximation.
- Masked materials still discard consistently in all four geometry passes.

## 3. Physically based BRDF

### Diagnosis

The current direct-light equation is `max(dot(N, L), 0)`. It cannot express
rough stone highlights, polished metal, cloth response, or grazing reflections.

### Design

Implement the metallic-roughness glTF model using:

- Lambert or Burley diffuse;
- GGX/Trowbridge-Reitz normal distribution;
- Smith correlated visibility;
- Schlick Fresnel;
- energy conservation between diffuse and specular lobes.

Use `F0 = mix(vec3(0.04), baseColor, metallic)`. Clamp perceptual roughness to
a small nonzero value before squaring it for the GGX alpha term. Metallic
surfaces contribute no diffuse lobe. Apply direct-light shadow visibility to
the complete sun BRDF contribution, not only the diffuse portion.

Factor the shared material evaluation into GLSL includes used by main and
portal shaders. Alpha-mask wrappers remain thin variants around the same body.

### Image-based lighting

The Sponza interior needs environment lighting even when the sun is not visible.
Add split-sum image-based lighting:

- diffuse irradiance cube map;
- prefiltered specular environment mip chain;
- 2D BRDF integration LUT.

Generate these from the existing sky environment at startup or ship cached
derived assets. Environment intensity and rotation become render settings.
SSAO may modulate diffuse environment lighting and, conservatively, the lowest
frequency specular contribution. It must not black out direct sunlight.

### Acceptance criteria

- Stone, wood, fabric, and metal are visually distinguishable under one light.
- Roughness changes highlight width without changing base geometry.
- Metals use tinted specular and have no chalky diffuse component.
- Highlights roll off through the tonemapper rather than clipping flat white.
- Portal material appearance matches the main-camera appearance.

## 4. SSGI integration and stability

### Diagnosis

The trace already writes an analytic sky fallback to `fallbackIndirectImage`
and writes classification plus `1.0 - hitFraction` to `diagnosticImage`. These
signals are currently exposed only for debugging. The forward shader removes
`ambientColor * ao` whenever SSGI is enabled, while the composite adds the
filtered trace over the HDR target using additive blending. In Sponza, the
cosine-weighted hemisphere estimator mostly sees dark interior surfaces, so its
magnitude is not calibrated to replace the removed ambient term.

The trace currently stores `incident * albedo`. A later composite must not
multiply that value by base color again. Prefer changing the trace and filter to
store incident radiance without albedo, since bilateral filtering radiance is
more stable across differently colored surfaces.

### Stage A: stabilize the existing forward/additive path

Keep `ambientColor * ao` in forward shading while SSGI is enabled. Change the
SSGI trace to store incident radiance, then apply receiver albedo once before
the additive composite by adding the existing `gbufferAlbedo` image to the
composite descriptor set. Add only the confidence-weighted, screen-hit bounce
contribution; do not add the analytic miss fallback on top of the retained
ambient term. Route the existing hit fraction and temporal validity into the
composite alpha. This stage works with the current additive pipeline, cannot
darken a missed region, and avoids inventing a second fallback or diagnostic
buffer.

### Stage B: physically based indirect replacement

If SSGI should replace environment diffuse rather than supplement it, move
indirect composition to a non-additive full-screen lighting pass. Supply that
pass with receiver albedo, AO, direct lighting, traced incident radiance, and
confidence, then evaluate:

```glsl
vec3 indirectIncident = mix(environmentDiffuse, ssgiIncident, confidence);
vec3 indirectDiffuse = baseColor * ao * indirectIncident;
vec3 finalColor = directLighting + indirectDiffuse * indirectIntensity;
```

The current `SRC_ALPHA, ONE` blend cannot express this replacement because it
cannot subtract the ambient already present in the destination. Portal views
currently have one color attachment and no material MRTs, so this stage must
either extend their render targets or preserve their forward ambient path.

### Temporal rules

Invalidate SSGI history on:

- scene load;
- camera teleport or portal traversal;
- renderer or quality-preset change;
- render-scale or extent change;
- large camera translation/rotation;
- projection change.

Per pixel, reject history using reprojected UV bounds, depth, normal, velocity,
and neighborhood luminance. Clamp accepted history into the current
neighborhood before blending. A screenshot preset may use a high history weight
after the camera is stationary, but moving-camera presets need lower weights.

At present, history is reset for preset changes, extent changes, and disabling
SSGI. Scene load, teleport/portal traversal, and projection changes must be
added. Reuse the existing reset mechanism rather than creating a second history
generation path.

### Presets

| Preset | Resolution | Rays | Steps | History target |
|---|---:|---:|---:|---:|
| Performance | half | 1 | 16 | 0.85-0.90 |
| Balanced | half | 2 | 32 | 0.88-0.92 |
| High | half | 4 | 48 | 0.90-0.94 |
| Capture | full | 8 | 96 | 0.96 after camera settles |

These rows refine the existing `apply_*_settings` presets. They are not new
presets; preserve current values unless a visual test justifies changing them.

### Acceptance criteria

- Enabling SSGI never makes a fully missed region darker than its fallback.
- A static Sponza camera converges without persistent blotches.
- Moving laterally past columns leaves no visible duplicate arches.
- Loading a new scene shows no history from the previous scene.
- SSGI off/on comparisons change indirect color and contact lighting, not base
  exposure or direct sunlight.

## 5. Directional shadows

### Diagnosis

One 4096-square shadow map covers an orthographic box defined by
`_shadowRadius`. Increasing the radius covers more of Sponza but reduces texel
density everywhere. Anything outside the box is treated as lit.

### Stage A: fitted single map

Compute the camera-visible receiver bounds and relevant caster bounds in light
space. Fit the orthographic box to those bounds with a configurable safety
margin, then snap its origin to shadow texels. Clamp the fitted range so one far
outlier cannot destroy near-camera resolution.

Expose the fitted bounds in the existing shadow-bounds debug view. Preserve the
current manual radius as a fallback and comparison mode.

### Stage B: cascaded shadow maps

Use three or four cascades stored in a depth texture array. Compute practical
split distances using a blend of logarithmic and linear partitioning. Fit and
texel-snap each cascade separately. Store every light view-projection and split
distance in scene data.

In `sunlight_visibility`, select the cascade from view-space depth and blend
across a narrow split region to hide transitions. Tune constant, slope, and
normal bias per cascade because a single bias does not scale across different
world-units-per-texel values.

### Acceptance criteria

- No visible hard boundary where Sponza shadows stop.
- Near columns and ornament edges retain more detail than the current radius-80
  single map.
- Slow camera movement does not make shadows shimmer.
- Cascade transitions are invisible in final lighting and visible in a debug
  coloration mode.

## 6. SSAO

SSAO remains a small-scale contact effect. It should complement environment
lighting and SSGI rather than replace either one. For Sponza, keep radius near
`0.75-1.25`, bias near `0.075`, and high sampling for capture. Avoid increasing
radius to several world units; that produces broad gray halos and duplicates
the job of indirect lighting.

Add a Sponza validation camera aimed at column/floor contacts and another at a
grazing wall. Acceptance requires no vertical bars, no repeating noise columns,
and no halo across depth discontinuities.

## 7. Tonemapping, exposure, and color

The tonemapping pass remains after lighting/SSGI and before FXAA. Preserve the
piecewise linear-to-sRGB encode when the swapchain format is UNORM. If the
swapchain changes to an sRGB format, remove the manual encode and let attachment
conversion perform it; never apply both.

Replace the ambiguous exposure multiplier with an optional EV control:

```text
linear multiplier = exp2(exposureEV)
```

Keep manual exposure for deterministic screenshots. Add a false-color or
luminance debug view to identify clipped highlights and crushed interiors.
Auto-exposure can follow later, with percentile luminance, adaptation speed,
and a scene-load reset.

Sponza capture starts with ACES and an exposure chosen so the sunlit upper wall
retains detail while the lower arcade remains readable. The exact value is an
artistic setting and should be saved with the scene or capture preset.

## 8. Anti-aliasing and still capture

FXAA should remain optional and run on tonemapped display-range color. Tune it
against thin railings and roof edges; if it softens carvings, leave it disabled
for the capture.

For higher still quality, add a capture-only supersampling path rather than
increasing FXAA strength. Render at 1.5x or 2x dimensions into temporary images,
allow SSGI to converge, then downsample once with a high-quality reconstruction
filter. This requires resources sized independently from the swapchain and a
capture-specific projection aspect derived from the requested output.

Extend the existing `MIRABILIS_CAPTURE` and `MIRABILIS_SSGI_CAPTURE` paths. They
already write PFM diagnostic captures and metadata containing the scene, camera,
preset, ray settings, history weight, and intensity. Add a direct command that
captures the final post-tonemap image as PNG. Optionally retain pre-tonemap HDR
output as PFM or add EXR. The final-image capture must omit ImGui, the crosshair,
timer, movement hints, and play-control overlays without changing normal
interactive UI state.

### Acceptance criteria

- A screenshot can be captured at a requested resolution without OS scaling.
- The saved PNG is byte-for-byte the final rendered image without UI.
- Repeated captures from a stationary camera are stable after convergence.
- Thin geometry is clean without visible whole-image blur.

## Quality presets

Presets should configure coherent groups rather than only SSGI:

### Interactive

- Render scale 0.75.
- 2048 shadow map or two modest cascades.
- Half-resolution Balanced SSGI.
- Medium SSAO.
- 8x anisotropy.
- Tonemapping on; FXAA optional.

### High

- Render scale 1.0.
- 4096 shadow map or three cascades.
- Half-resolution High SSGI.
- High SSAO.
- 16x anisotropy.
- Tonemapping on.

### Capture

- Full-resolution or supersampled output.
- Four 4096 cascades if memory permits.
- Full-resolution 8-ray SSGI with corrected fallback and history.
- High SSAO.
- 16x anisotropy.
- ACES with manual exposure.
- UI-free PNG plus optional HDR capture.

The Capture preset must check the estimated memory requirement before allocating
resources. If it exceeds the device budget, it should reduce cascade resolution
or supersampling and report the downgrade instead of crashing.

## Debugging and instrumentation

Retain the current depth, normal, AO, motion-vector, SSGI, and shadow debug
views. Add:

- material base color after correct sRGB decode;
- normal-mapped shading normal;
- metallic and roughness channels;
- direct diffuse and direct specular;
- environment diffuse and environment specular;
- SSGI confidence and fallback contribution;
- shadow cascade index;
- final linear luminance before tonemapping;
- estimated and actual texture memory.

GPU timings should separately report shadow, prepass, SSAO, forward shading,
SSGI trace, temporal filter, spatial filter, tonemap, and FXAA. Screenshot mode
should print the final settings and GPU name alongside the capture path.

## Implementation order

1. Correct base-color uploads to sRGB and validate Sponza against a glTF viewer.
2. Bind existing metallic/roughness textures and classify the remaining texture
   roles while preserving the existing mip path.
3. Split `vk_engine_renderer.cpp` without changing rendered output.
4. Request anisotropy support and enable it with a device-limited fallback.
5. Add tangents and normal mapping.
6. Add the metallic-roughness GGX direct-light BRDF.
7. Add diffuse and specular image-based lighting.
8. Stabilize the current additive SSGI path and add missing history invalidation.
9. Decide whether Stage B's full-screen indirect replacement and portal MRTs
   are justified by the Stage A result.
10. Fit the existing single shadow map to visible bounds.
11. Add cascaded shadows if the fitted map is insufficient.
12. Extend capture to UI-free post-tonemap PNG and optional supersampling.
13. Tune and save the final Sponza capture preset.

Each step should build and run independently. Material work must be checked in
both the main and portal views. Shadow and alpha-mask work must also be checked
against the prepass, SSAO/SSGI inputs, and shadow silhouettes.

## Final Sponza acceptance shot

The milestone is complete when one fixed camera produces a UI-free image with:

- readable sunlit and shaded architecture;
- visible normal-map relief on stone and ornamentation;
- distinct stone, wood, cloth, and metal response;
- intact masked geometry and matching masked shadows;
- stable contact AO without columns or halos;
- indirect color that enriches the interior without darkening misses;
- detailed shadows with no visible coverage cutoff or shimmer;
- no temporal trails, whole-image blur, clipping, or unintended color cast;
- a recorded GPU name, preset, exposure, resolution, and convergence time.
