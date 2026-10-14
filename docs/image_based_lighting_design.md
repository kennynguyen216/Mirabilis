# Image-Based Lighting for the Raster Material Model

## Document status

**Status:** Implemented on branch `material-model` (results below)
**Date:** 2026-09-14
**Refines:** Milestone 6 of [Raster Material Model](raster_material_model_design.md),
which deferred image-based lighting to its own design because it touches
skybox loading, startup time, and the SSGI environment-miss convention.

## Summary

After Milestone 5 the raster material model is physically based everywhere
except its indirect term. Diffuse and specular ambient light both come from
one flat colour, `ambientColor = 0.28`, split by Karis's analytic
environment-BRDF fit. Metals therefore reflect a grey nothing, and the lit
colour of a surface does not depend on the sky above it.

This design replaces the flat colour with the selected skybox:

1. **Diffuse irradiance** as nine spherical-harmonic (SH) coefficients,
   projected on the CPU while the panorama is loaded.
2. **Prefiltered specular radiance** as a GGX-prefiltered equirectangular mip
   chain, built by a compute shader whenever the skybox changes.
3. **A BRDF lookup table** of the split-sum scale and bias, built on the CPU at
   start-up from the path tracer's own Fresnel and separable Smith terms.

The flat ambient model stays reachable through one toggle for comparison.

## Current state

- `set_skybox` (`src/vk_engine_resources.cpp`) loads the selected panorama with
  a full mip chain as RGBA16F (HDR) or sRGB (8-bit). For HDR it also computes
  `_skyboxIndirectClamp`, the 99.9th luminance percentile, above which a pixel
  belongs to the sun disk that the shadow-mapped direct term already delivers.
- `shaders/environment.glsl` is the single environment policy: intensity
  (`ssgiFallbackSettings.x`), a black override (`ssgiFallbackSettings.y`), and
  the sun ceiling (`indirectSettings.w`). `indirectSettings.y` chooses the
  panorama or the analytic gradient that the path tracer also uses.
- SSGI fills ray misses with `environment_radiance(direction)` at a coarse mip.
- Portal cameras, which cannot run SSGI, add
  `substitute * environment_irradiance(N)`, a single very coarse mip read.
- `material_ambient` in `shaders/material_brdf.glsl` splits
  `ambientColor * occlusion * ambientScale` into diffuse and specular.
  `ambientScale` is the SSGI ambient-retention policy (default 0.5 while SSGI
  runs).
- **Units.** The raster ambient term outputs `albedo * ambientLight`, so
  `ambientLight` is a radiance: a uniform surround of radiance `L` lights a
  Lambert surface to `albedo * L`. Irradiance `E` from SH must therefore enter
  as `E / pi`.

## Goals

1. Diffuse ambient light follows the sky's colour and direction.
2. Metals reflect the sky, sharper for smoother surfaces.
3. One environment policy: intensity, black override, and sun clamp apply to
   the new terms exactly as they apply to SSGI misses.
4. Main and portal cameras receive identical image-based lighting.
5. The SSGI retention policy applies to IBL diffuse exactly as it did to flat
   ambient; SSGI never scales specular, which it does not supply.
6. Rebuilding for a new skybox costs well under a second.
7. Sponza raster frame time rises by no more than 0.5 ms.

## Non-goals

- Local reflection probes, parallax correction, or screen-space reflections.
- Image-based lighting in the path tracer.
- Multiple-scattering compensation (the reference renderer has none).
- Occluding IBL specular beyond the existing ambient-occlusion term.

## Design

### 1. Diffuse irradiance: order-2 spherical harmonics

In `set_skybox`, after the pixels are decoded and before they are freed:

- For each sampled texel (a stride keeps a 4K panorama to about 250,000
  samples), convert to linear radiance, apply the sun ceiling by luminance
  exactly as `apply_environment_policy` does, and weight by the texel's solid
  angle `(2 pi / W) (pi / H) sin(theta)`.
- Project onto the nine real SH basis functions, using the direction
  convention of `equirectangular_uv`.
- Convolve with the clamped cosine lobe (band factors pi, 2 pi / 3, pi / 4) to
  obtain irradiance coefficients.

The coefficients are stored as `glm::vec4 environmentSH[9]` in `GPUSceneData`.
For the analytic gradient the same projection runs over the gradient
function, so switching `traceEnvironmentMap` keeps diffuse IBL consistent with
SSGI misses.

### 2. Specular: a GGX-prefiltered mip chain

A new compute shader, `shaders/environment_prefilter.comp`, writes an
RGBA16F storage image of 512 x 256 with six levels. Level `k` holds radiance
prefiltered for roughness `k / 5`.

- Level 0 is a filtered copy of the panorama.
- Each other level uses GGX importance sampling with the usual N = V = R
  assumption (64 samples), reading the source panorama at a mip chosen from
  each sample's PDF so few samples stay smooth (filtered importance sampling).
- Each sample passes through the sun ceiling before it is averaged. Intensity
  and the black override stay in the shader, so changing them needs no
  rebuild.

The build runs inside `set_skybox` through `immediate_submit`, and its time is
logged. For the analytic gradient, the shader reads the gradient function
instead of the texture.

### 3. BRDF lookup table

A 64 x 64 RGBA16F image with `(scale, bias)` against `(N·V, roughness)`, built
on the CPU at start-up by importance-sampling the path tracer's GGX with its
separable Smith term and Schlick Fresnel, 256 samples per texel. It replaces
`material_environment_brdf`'s analytic fit when IBL is on.

### 4. Bindings

The scene descriptor set (set 0) gains:

| Binding | Contents | Sampler |
|---|---|---|
| 4 | prefiltered environment | linear, mips, U repeat, V clamp |
| 5 | BRDF lookup table | linear, clamp |

`update_skybox_descriptors` already rewrites binding 3 in every frame's main
and portal scene sets; it writes bindings 4 and 5 beside it. The global
descriptor pool ratio for combined image samplers rises from 4 to 6, because a
scene set now holds five.

### 5. Shading

`GPUSceneData` gains `iblSettings`: x = 1 when IBL is enabled, y = the
prefiltered chain's highest level. `material_ambient` becomes:

```glsl
vec3 irradiance = ibl_irradiance(N);                 // E(N) from SH, policy applied
vec3 diffuse    = irradiance / PI * base * (1 - metallic) * occlusion * ambientScale;
vec3 R          = reflect(-V, N);
vec3 prefiltered = textureLod(prefilteredEnvironment, equirectangular_uv(R),
                              roughness * maxLevel).rgb;   // policy applied
vec2 ab         = texture(brdfLut, vec2(NoV, roughness)).rg;
vec3 specular   = prefiltered * (F0 * ab.x + ab.y) * occlusion;
```

With specular disabled, the diffuse term uses `base` in place of
`base * (1 - metallic)`, as today. With IBL disabled, the flat-ambient path is
unchanged.

**Portal cameras** apply the same function. Their extra SSGI substitute adds
`substitute * irradiance / PI * base * (1 - metallic) * occlusion`, so the two
cameras still divide indirect light the same way.

### 6. Controls

- "Image-based lighting" checkbox under Material Shading, default on.
- The existing environment intensity and skybox selector drive it.
- `MIRABILIS_IBL_DISABLE=1` for capture runs.

## Milestones

1. **Data:** SH projection, BRDF lookup table, prefilter compute pass, and
   bindings. IBL defaults off, so no image changes.
   *Acceptance:* all 53 baseline captures identical; prefilter time logged; no
   validation errors.
2. **Shading:** IBL diffuse and specular in `material_brdf.glsl` for both
   passes; toggle defaults on.
   *Acceptance:* new raster baselines; metals in `material_reference_lab.json`
   reflect the sky with sharpness following roughness; parity (direct only) is
   unchanged; Sponza frame time within +0.5 ms of Milestone 0 of the material
   model design.

## Risks

| Risk | Effect | Mitigation |
|---|---|---|
| Sky irradiance is unoccluded in enclosed scenes | Interiors such as Sponza brighten | Same occlusion and SSGI retention as flat ambient; toggle; measure mean luminance |
| Sun disk captured in SH or prefiltered chain | Double-counted, unshadowed sun | Sun ceiling applied per sample before projection and prefiltering |
| Irradiance entered without 1/pi | Ambient 3x too bright | Units section; compare a uniform-white panorama with flat ambient |
| Equirectangular seam or pole in prefilter | Visible line in reflections | Wrap U, clamp V, sample by direction |
| Skybox switch leaves a stale chain | Reflections of the previous sky | Build inside `set_skybox` before descriptors are rewritten |

## Results

**Milestone 1 (8cd5e96).** `tmp/material-baseline/m6-data/compare-m7-loader.txt`:
all 53 captures identical, with IBL built but off. Every start-up loads more
than one panorama, so the rebuild path runs on each switch. Prefiltering takes
0.4 ms for the 8192 x 4096 legacy panorama and 1.5 ms for the 4K HDR skies.
The irradiance L0 coefficient is (2.91, 3.26, 3.31) for the legacy sky, about
0.27 radiance after the basis constant and 1/pi, close to the old flat 0.28.

**Milestone 2 (a6c5fa3).** `tmp/material-baseline/m6/compare-m6-data.txt`:
47 of 53 identical. The six differences are the raster captures; no
path-trace or harness capture changed, and all 21 harness cases pass. Parity
(`parity-m6`) is unchanged: median error 0.0006, energy ratio 0.999.

| Capture (mean luminance) | Flat ambient | IBL | Ratio |
|---|---:|---:|---:|
| Sponza, SSGI on | 0.0623 | 0.0562 | 0.90 |
| Sponza, SSGI off | 0.0792 | 0.0518 | 0.65 |
| Material lab, SSGI off | 0.2209 | 0.2266 | 1.03 |
| Shadow showcase, SSGI off | 0.4724 | 0.4906 | 1.04 |

- The interior risk went the other way from the one predicted: Sponza
  darkens, because the noon HDR sky's irradiance is below the old flat
  ambient. With SSGI on, the default, the change is 10%.
- Metals now reflect the sky: the gold GGX cube in `gi_material_lab.json`
  (roughness 0.22) goes from (0.225, 0.163, 0.076) to (0.266, 0.216, 0.098)
  on its front face, taking on the sky's bluer tint.
- IBL shares the environment policy, so a scene with a black environment gets
  black image-based lighting. `material_reference_lab.json` sets one for the
  path tracer, and its smooth metals now render black away from the sun.
- Release benchmark: Sponza 4.031 ms (below the material model's Milestone 0
  4.239 ms), sandbox 3.774 ms, portal_bhop_course 2.353 ms.
- Not tested: equirectangular seams and poles in reflections under camera
  motion, and interactive skybox switching in the editor.
