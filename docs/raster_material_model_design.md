# Raster Material Model: Normal Mapping and Metallic-Roughness BRDF

## Document status

**Status:** Draft for review; not implemented
**Date:** 2026-09-14
**Measured at:** `main` 6b7a989
**Refines:** steps 5 and 6 of [Sponza Rendering Quality Design](sponza_rendering_quality_design.md)
(sections 2 and 3), and plans step 7 (image-based lighting) as a later phase
**Related:** [Sponza handoff](sponza_handoff.md), [Portal lighting continuity](portal_gi_lighting_continuity_issue.md)

The Sponza design document already states *what* the material model should
be. This document states *how* to build it in the current code: which data
changes, which shaders and passes consume it, the order of commits, and how
each step is verified. Where this document disagrees with the Sponza design,
the reason is given and this document should win.

## Summary

The raster renderer shades every surface as Lambert diffuse under one sun plus
a flat ambient term. It ignores the metallic/roughness textures it already
binds, has no tangents, no view vector, and no emission. The software path
tracer, meanwhile, already evaluates a GGX metallic-roughness BRDF.

This project gives the raster forward pass and the portal forward pass:

1. a tangent per vertex, from glTF or generated on import;
2. sampled metallic/roughness textures and tangent-space normal maps;
3. a GGX specular lobe for the sun using **the same formulas as the path
   tracer**, so the reference renderer remains a meaningful comparison;
4. a split of the flat ambient term into diffuse and specular parts, so metals
   do not turn black before image-based lighting exists;
5. material emission;
6. G-buffer outputs that keep SSGI physically consistent once surfaces can be
   metallic and view-dependent.

Image-based lighting, texture compression, and path-tracer normal maps are
explicitly later work.

## Current state

Everything in this section was verified in code on 2026-09-14.

### Vertex data

- `Vertex` is 48 bytes: `position, uv_x, normal, uv_y, color`
  (`src/vk_types.h:54`). There is no tangent.
- The struct is **redeclared independently in eight vertex shaders** that index
  the vertex buffer through `buffer_reference`: `mesh.vert`,
  `portal_view.vert`, `portal_mask.vert`, `depth_normal.vert`,
  `depth_normal_mask.vert`, `shadow_depth.vert`, `shadow_depth_mask.vert`, and
  `colored_triangle_mesh.vert`. The last one is not referenced by any source
  file.
- `loadGltf` reads `POSITION`, `NORMAL`, `TEXCOORD_0`, and `COLOR_0` only
  (`src/vk_loader.cpp:254-314`). A second copy of that code lives in
  `loadGltfMeshes` (`src/vk_loader.cpp:782`), which is declared but never
  called.
- Built-in meshes are written by hand in `init_default_meshes`
  (`src/vk_engine_resources.cpp:342-497`): the floor quad, the unit cube used by
  walls, the surf ramp, and the portal quad. The portal quad uses designated
  initializers.
- The path tracer keeps a CPU copy of every uploaded mesh as
  `TraceMeshSource::vertices` (`std::vector<Vertex>`) and flattens it into
  world-space triangles (`src/vk_engine_path_trace.cpp:409-440`).

### Material data

- `MaterialConstants` is 256 bytes and is used as the dynamic-UBO stride:
  `colorFactors`, `metal_rough_factors`, `extra[14]` (`src/vk_engine.h:293`).
- The shader names only four of those vectors (`shaders/input_structures.glsl:44-54`):
  `colorFactors`, `metal_rough_factors` (x metallic, y roughness,
  **z = debug checkerboard flag**), `uvTransform` (`extra[0]`), and
  `alphaMask` (`extra[1]`, x = cutoff). `extra[2..13]` are free.
- Material set 1 has three bindings (`src/vk_engine_materials.cpp:45-51`):
  0 constants, 1 `colorTex`, 2 `metalRoughTex`. Binding 2 is populated for
  24 of 28 Sponza materials but **never sampled** by any raster shader.
- Materials are written in three places: `GLTFMetallic_Roughness::write_material`,
  the glTF loader's `createMaterial` (`src/vk_loader.cpp:660-723`), and the
  editor material path, which rewrites the descriptor set by hand when a
  material changes (`src/vk_engine_scene_materials.cpp:131-153`).
- Editor materials (`SceneMaterial`, `src/scene.h:87`) carry base-colour texture,
  tint, UV scale, metallic, roughness, emission colour and strength,
  transmission, and IOR. They have no normal or metallic/roughness texture.

### Shading

- `shaders/mesh_shading.glsl` computes
  `baseColor * (ambient + visibility * max(N·L, 0) * sunlightColor)`.
  `ambient` is `ambientColor (0.28) * AO * retention`, where retention is the
  SSGI ambient-retention setting when SSGI is on.
- `shaders/portal_view_shading.glsl` duplicates that body, writes a single
  colour attachment, and replaces part of the ambient term with
  `environment_irradiance(N)` while the main camera runs SSGI.
- There is no view vector. `GPUSceneData` (`src/vk_types.h:114`,
  `shaders/scene_data.glsl`) contains view and projection matrices but no
  camera position.
- The raster path has no emission (`src/vk_engine_resources.cpp:586` notes this).
- Forward outputs: `outFragColor`, `outAlbedo`, `outVelocity`, and
  `outDirectLighting = baseColor * direct`. SSGI treats the direct-lighting
  buffer as the radiance its rays find and multiplies its filtered result by
  `outAlbedo` at composite (`shaders/ssgi_composite.frag:78`).
- The depth/normal prepass writes **geometric** view-space normals
  (`shaders/depth_normal.vert:41-45`).

### The path tracer's material model

`shaders/path_trace_material.glsl:22-40`, selected when `materialModel == 1`
(the default):

```text
alpha    = max(roughness^2, 0.002025)
F0       = mix(0.04, albedo, metallic)
F        = F0 + (1 - F0) (1 - V·H)^5                    Schlick
D        = alpha^2 / (π ((N·H)^2 (alpha^2 - 1) + 1)^2)  GGX
G        = G1(N·V) G1(N·L)                              separable Smith
G1(x)    = 2x / (x + sqrt(alpha^2 + (1 - alpha^2) x^2))
diffuse  = (1 - F)(1 - metallic) albedo / π
specular = F D G / (4 (N·V)(N·L))
```

The sun contributes `f · (N·L) · sunRadiance` (`shaders/path_trace_transport.glsl:43`).
Metallic and roughness come from material factors only, and shading uses
geometric normals. Since `0487a85` only base-colour textures keep a CPU copy,
so the tracer cannot sample normal or roughness maps without reversing that
memory saving. The validation log reports a single-scattering white-furnace mean
of 0.944, so no multiple-scattering compensation is applied.

### Light units

The raster sun colour is `vec4(1)` (`src/vk_engine_renderer.cpp:497`) and the
diffuse term is `albedo · N·L · sunlightColor`, with no 1/π. That equals
`π · (albedo/π) · E · N·L`: the raster sun colour behaves as irradiance and the
raster output is **π times a BRDF**. The new specular lobe must follow the same
convention, or rough dielectrics will change brightness when this lands.

### Assets

| Asset | Primitives with `TANGENT` | `normalTexture` | Emissive / occlusion textures |
|---|---:|---:|---|
| `sponza_optimized/NewSponza_Main_glTF_003.gltf` | 405 | 24 | none |
| `structure.glb` | 0 | 1 | none |
| `tung_tung_tung_sahur.glb` | 0 | 0 | none |
| `basicmesh.glb` | 0 | 0 | none |

Sponza supplies its own tangents. `structure.glb` needs generated ones. No
tangent-generation library is vendored under `third_party/`.

### Baseline performance

Release build, RTX 3060 Laptop GPU, 1280x720 window at 0.75 render scale,
400 frames, `MIRABILIS_SSGI_BENCHMARK=1`:

| Scene | Average frame |
|---|---:|
| `sponza_showcase.json` | 4.3 ms |
| `sandbox.json` | 3.6 ms |
| `portal_bhop_course.json` | 2.3 ms |

### Latent bug found while measuring

The built-in floor writes its checkerboard flag into `metal_rough_factors.z`
(`src/vk_engine_resources.cpp:516`) and then copies the whole vector into
`traceParameters` (`:532`). The path tracer reads `parameters.z` as
transmission and `parameters.w` as IOR (`shaders/path_trace_transport.glsl:88-90`).
Any object that uses the built-in floor material, rather than an editor
material, is therefore traced as fully transmissive with IOR clamped to 1. This
is fixed in Milestone 1, before the flag moves.

No `gi_*` validation scene is affected: every floor there enables an editor
material. The scenes that use the built-in floor material are `sandbox.json`,
`portal_bhop_course.json`, `sponza_showcase.json`, and `empty_platform.json`.

## Goals

1. Normal-mapped lighting on every material that supplies a normal texture.
2. Metallic and roughness from glTF textures multiplied by their factors.
3. A GGX sun term in raster that matches the path tracer's BRDF exactly when
   normal maps and material textures are disabled.
4. Metals remain readable in shadow before image-based lighting exists.
5. Material emission in raster, matching the path tracer's `traceEmission`.
6. Identical material response in the main camera and in portal cameras.
7. SSGI keeps receiving view-independent radiance and a diffuse albedo.
8. No rendering change at all from the data-layout milestones.
9. Sponza raster frame time rises by no more than 1.0 ms at the baseline
   settings above.

## Non-goals

- Image-based lighting (diffuse irradiance, prefiltered specular, BRDF LUT).
  Milestone 6 names the work; it needs its own design.
- Normal maps or textured roughness in the path tracer.
- Texture compression (BC5/BC7), cascaded shadows, still capture.
- Occlusion textures. No current asset supplies one.
- Clearcoat, sheen, anisotropy, or transmission in raster.
- Multiple-scattering energy compensation. The tracer does not apply it either.
- Editor authoring of normal or metallic/roughness textures, beyond the
  optional Milestone 7.

## Design

### 1. Vertex format

Append a tangent:

```cpp
struct Vertex {
    glm::vec3 position;
    float uv_x;
    glm::vec3 normal;
    float uv_y;
    glm::vec4 color;
    // xyz = object-space tangent, w = bitangent sign (+1 or -1).
    // w = 0 means the mesh has no usable tangent; shaders then fall back to
    // the geometric normal.
    glm::vec4 tangent;
};
static_assert(sizeof(Vertex) == 64);
static_assert(offsetof(Vertex, tangent) == 48);
```

Appending keeps every existing field offset, so the path tracer's triangle
flattening and the portal quad's designated initializers stay valid. The layout
is std430-aligned with no padding.

Move the GLSL declaration into one include, `shaders/vertex.glsl`, and include
it from all seven live vertex shaders in one commit, **before** the struct
grows. The commit that grows it then edits only `vertex.glsl` on the shader
side. Delete `colored_triangle_mesh.vert` and `loadGltfMeshes` (with its
`friend` declaration in `src/vk_engine.h`) in separate commits beforehand. A
shader that still declares the 48-byte struct would read every vertex after
the first at the wrong offset, and the validation layers cannot detect that.

`CMakeLists.txt` collects shaders and `.glsl` includes with `file(GLOB)` at
configure time, so re-run the CMake configure step after deleting a shader or
adding an include.

**Memory:** the change adds 16 bytes per vertex (+33%) to each GPU vertex
buffer and to the path tracer's CPU copy. Add the vertex count to the existing
per-scene texture log line so the cost is measured, not estimated.

### 2. Tangent sources

In priority order:

1. **glTF `TANGENT`** (`vec4`), read in `loadGltf` beside `NORMAL`.
2. **Generated.** If a primitive has `TEXCOORD_0` but no `TANGENT`, run a CPU
   helper after the vertices and indices are read:

   ```cpp
   // Per-triangle UV gradients, accumulated per vertex, Gram-Schmidt
   // orthogonalized against the normal. w carries handedness.
   void generate_tangents(std::span<Vertex> vertices,
                          std::span<const uint32_t> indices);
   ```

   Triangles with degenerate UVs contribute nothing. A vertex that ends with
   a zero tangent gets an arbitrary tangent perpendicular to its normal and
   `w = 1`.
3. **Built-in meshes.** Call the same helper in `init_default_meshes`. It is
   exact for the floor, cube, ramp, and portal quad, which are flat per face.
4. **No UVs:** leave `w = 0`.

The glTF specification asks for MikkTSpace tangents when none are supplied. The
generator above differs from MikkTSpace only at smoothed UV seams, and the one
affected asset is `structure.glb`. Vendoring `mikktspace.c` is left as an open
question rather than a prerequisite.

### 3. Tangent frame in the vertex shaders

```glsl
mat3 model = mat3(PushConstants.render_matrix);
mat3 normalMatrix = transpose(inverse(model));
vec3 N = normalize(normalMatrix * vertex.normal);
// A tangent is a direction along the surface, so it transforms with the model
// matrix, not with the inverse transpose.
vec3 T = model * vertex.tangent.xyz;
T = normalize(T - N * dot(N, T));
// A mirroring transform flips the frame's handedness.
float sign = vertex.tangent.w * (determinant(model) < 0.0 ? -1.0 : 1.0);
outTangent = vec4(T, sign);   // sign == 0 passes "no tangent" through
```

The bitangent is rebuilt in the fragment shader as `cross(N, T) * sign`. Only
`mesh.vert` and `portal_view.vert` need this. The prepass and shadow shaders
keep using geometric normals.

### 4. Material constants

Keep the 256-byte stride, and name the fields in C++ and GLSL together:

| Slot | Field | Contents |
|---|---|---|
| 0 | `colorFactors` | base colour factor (unchanged) |
| 1 | `metal_rough_factors` | x metallic, y roughness, **z and w must be 0** |
| 2 | `uvTransform` (`extra[0]`) | unchanged |
| 3 | `alphaMask` (`extra[1]`) | x cutoff (unchanged) |
| 4 | `emission` (`extra[2]`) | rgb = emissive factor × strength, w reserved |
| 5 | `materialFlags` (`extra[3]`) | x normal-map scale, y debug checkerboard, zw reserved |

Moving the checkerboard flag is one commit. It touches `mesh_shading.glsl`,
`portal_view_shading.glsl`, the floor constants in
`src/vk_engine_resources.cpp:516`, and the editor constants in
`src/vk_engine_scene_materials.cpp:110-112`. The glTF loader already writes 0
there.

`traceParameters` stays a separate CPU value (`metallic, roughness,
transmission, IOR`) and must never again be copied from `metal_rough_factors`.

### 5. Descriptor set

Add binding 3, `normalTex`, a combined image sampler.

- **Default:** a new engine-owned 1x1 `_flatNormalImage`,
  `R8G8B8A8_UNORM (128, 128, 255, 255)`. `LoadedGLTF::clearAll` skips the
  engine's default images through an explicit list
  (`src/vk_loader.cpp:375-378`); add the new image to that list, or unloading a
  glTF will destroy it.
- **`MaterialResources`** gains `normalImage` and `normalSampler`.
- **Loader:** resolve `source->normalTexture` as linear (`srgb = false`) through
  the existing `resolveTexture`, and write its `scale` into `materialFlags.x`.
  Count bound normal maps in the per-scene log, as base colour and
  metallic/roughness already are.
- **Editor materials:** bind the flat normal. Replace the hand-written rewrite in
  `resolve_scene_material` with a shared helper, so the new binding cannot be
  forgotten in one of the two places that write this set.
- **Pools:** the glTF pool reserves 3 image samplers per material set
  (`src/vk_loader.cpp:576`), which exactly fits bindings 1-3. The global
  allocator reserves 4. No change is required, but the ratio comment should say
  why the number is 3.

### 6. Camera position

Append `vec4 cameraPosition` (xyz world space, w unused) to the end of
`GPUSceneData` and `scene_data.glsl`, keeping the std140 layouts byte-identical.
The main camera fills it in `build_scene_data`. Portal cameras fill it in
`build_portal_scene_data` with the **virtual** camera's position. Deriving the
position from `inverse(view)` in each fragment would cost a 4x4 inverse per
pixel.

### 7. Shared material evaluation

Create `shaders/material_brdf.glsl`, included by both `mesh_shading.glsl` and
`portal_view_shading.glsl`. It owns everything that must agree between the two
cameras.

**Inputs:**

```glsl
vec4  base      = inColor * texture(colorTex, inUV);   // unchanged
vec4  mr        = texture(metalRoughTex, inUV);         // glTF: G roughness, B metallic
float metallic  = clamp(materialData.metal_rough_factors.x * mr.b, 0.0, 1.0);
float roughness = clamp(materialData.metal_rough_factors.y * mr.g, 0.045, 1.0);
```

The roughness floor of 0.045 gives `alpha = 0.002025`, the path tracer's floor.

**Shading normal:**

```glsl
vec3 N = normalize(inNormal);                     // geometric
if (inTangent.w != 0.0 && sceneData.materialSettings.x > 0.5) {
    vec3 T = normalize(inTangent.xyz - N * dot(N, inTangent.xyz));
    vec3 B = cross(N, T) * inTangent.w;
    vec3 t = texture(normalTex, inUV).xyz * 2.0 - 1.0;
    t.xy *= materialData.materialFlags.x;
    N = normalize(mat3(T, B, N) * t);
}
```

Shadow lookup keeps the **geometric** normal for its normal-offset bias.
Normal-mapped offsets move samples inside the receiver and bring shadow acne
back.

**Sun term:** the path tracer's BRDF, copied formula for formula, scaled by π
per the light-unit convention above:

```glsl
vec3 V = normalize(sceneData.cameraPosition.xyz - inWorldPosition);
vec3 L = normalize(sceneData.sunlightDirection.xyz);
// ... ggxD, smithG1, fresnelSchlick exactly as path_trace_material.glsl ...
vec3 diffuseBRDF  = (1.0 - F) * (1.0 - metallic) * base.rgb / PI;
vec3 specularBRDF = F * D * G / max(4.0 * NoV * NoL, 1e-4);
vec3 sun = PI * sceneData.sunlightColor.rgb * NoL * visibility;
vec3 directDiffuse  = diffuseBRDF  * sun;
vec3 directSpecular = specularBRDF * sun;
```

For a rough dielectric this is today's term times `(1 - F)`: at most 4% darker
at normal incidence and more at grazing angles. The change is accepted and
recorded in the Milestone 4 captures.

This deliberately uses the **separable** Smith term, not the height-correlated
form suggested in section 3 of the Sponza design. Matching the reference
renderer is worth more than the small accuracy difference.

`N·V` uses `max(dot(N, V), 1e-4)`. Normal maps routinely produce back-facing
shading normals on silhouettes, and the specular term must not divide by zero
there.

**Specular anti-aliasing:** widen roughness from the screen-space variation of
the geometric normal, as in Tokuyoshi and Kaplanyan's geometric specular
anti-aliasing:

```glsl
vec3 dndu = dFdx(geometricNormal), dndv = dFdy(geometricNormal);
float variance = 0.25 * (dot(dndu, dndu) + dot(dndv, dndv));
float kernel = min(2.0 * variance, 0.18);
float alpha = sqrt(clamp(roughness * roughness * roughness * roughness + kernel, 0.002025 * 0.002025, 1.0));
```

This changes `alpha` in raster only. The difference from the tracer is confined
to high-curvature or distant pixels, and the toggle in section 10 disables it
for parity captures.

**Ambient, until image-based lighting exists:**

```glsl
float ambientScale = /* unchanged SSGI retention policy */;
vec3 ambientLight  = sceneData.ambientColor.rgb * occlusion * ambientScale;
vec3 F0            = mix(vec3(0.04), base.rgb, metallic);
vec3 ambientDiffuse  = ambientLight * base.rgb * (1.0 - metallic);
vec3 ambientSpecular = ambientLight * env_brdf_approx(F0, roughness, NoV);
```

`env_brdf_approx` is Karis's analytic fit of the split-sum BRDF integral
("Physically Based Shading on Mobile", 2014). It treats the flat ambient colour
as a uniform environment, which is exactly what that term already claims to be.
Without it, a fully metallic surface out of the sun would render black. The
portal shader applies the same split to its `environment_irradiance(N)`
substitute.

SSGI supplies diffuse indirect light only, so the specular ambient term is not
counted twice.

**Emission:** `materialData.emission.rgb`, added once and unaffected by
lighting, shadow, or occlusion.

### 8. Forward outputs

| Output | Today | After |
|---|---|---|
| `outFragColor` | `base · (ambient + direct)` | `directDiffuse + directSpecular + ambientDiffuse + ambientSpecular + emission` |
| `outAlbedo` | `base` | `base · (1 - metallic)` |
| `outDirectLighting` | `base · direct` | `directDiffuse + emission` |
| `outVelocity` | unchanged | unchanged |

**Why `outAlbedo` loses metals:** the SSGI composite multiplies filtered
incident light by this buffer. A metal has no diffuse lobe in the BRDF above or
in the tracer, so it must not receive diffuse indirect light.

**Why `outDirectLighting` excludes specular:** SSGI reads this buffer as the
radiance leaving a surface *toward some other surface*. Specular radiance
depends on the direction it leaves in, and only the camera direction was
evaluated. Including it would carry camera-facing highlights to other surfaces
and would make SSGI history flicker as the camera moves. Emission leaves in
every direction and belongs in the buffer; the path tracer already treats
emitters as indirect light sources.

The prepass keeps writing geometric normals, as the Sponza design already
requires for stable SSAO and SSGI rejection.

### 9. Portal parity

`portal_view.vert` gains the tangent output, and `portal_view_shading.glsl`
switches to `material_brdf.glsl`. Main and portal shaders then differ only in
their outputs and ambient policy, never in the BRDF. The portal pipelines
already share `materialLayout`, so binding 3 needs no pipeline-layout change.

### 10. Controls and debug views

**Render Settings toggles**, stored in a new `GPUSceneData::materialSettings`
vector:

- x `Normal maps`
- y `Metallic/roughness textures` (off samples white, leaving factors only)
- z `Specular anti-aliasing`
- w `Specular` (off leaves the Lambert-only term, useful for A/B comparisons)

A `Match reference renderer` button turns x, y and z off. Only in that state
must raster and path-traced direct lighting agree.

**New `RenderDebugView` entries.** They are written by the forward pass and
passed through by the debug-view pass. `RenderDebugViewNames` and its fixed
array size in `src/vk_engine_render_helpers.h` must be updated together.

- Shading normal (world, remapped to [0, 1])
- Geometric normal
- Tangent, and tangent handedness
- Roughness (after specular anti-aliasing)
- Metallic
- Direct diffuse only
- Direct specular only
- Emission

## Milestones

Each milestone is its own PR. Inside a PR every commit either moves code or
changes it, never both.

### Milestone 0: baselines

1. Add test-only raster capture tooling in its own commit. Before it, the only
   raster capture was `MIRABILIS_SSGI_CAPTURE`, which writes SSGI's filtered
   indirect image only and is skipped when SSGI is off, and nothing could turn
   SSGI off from the environment.
   - `MIRABILIS_RASTER_CAPTURE=<path>` writes the linear HDR draw image (after
     portal views and the SSGI composite, before tonemapping) to `<path>.pfm`,
     with metadata in `<path>.txt`.
   - `MIRABILIS_SSGI_DISABLE=1` turns SSGI off for the run.
2. Save Debug harness artifacts and Release benchmark numbers for the three
   baseline scenes.
3. Capture raster HDR images of `sponza_showcase.json`, `gi_material_lab.json`,
   and `shadow_showcase.json` at fixed cameras with SSGI on and off.
4. Record path-tracer capture hashes for `open`, `cornell`, `materials`, and
   `sandbox.json`. `sandbox.json` is the only one whose floor uses the built-in
   floor material, so it is the capture Milestone 1 changes. Sponza is left
   out because the test-only brute-force BVH check takes minutes there.

**Acceptance:** artifacts are stored under `tmp/material-baseline/`, every
capture matches a repeat run, and the paths are written into this document.

**Recorded 2026-09-14 at `material-model` 4f1deb9.** Run
`scripts/capture_material_baseline.ps1 -Label m0 -HarnessArtifacts <harness dir>`
to reproduce; later milestones pass `-Label mN -Baseline m0` and read
`compare-m0.txt`. All paths are relative to `tmp/material-baseline/m0/`.

| Artifact | Path | Notes |
|---|---|---|
| Harness (Debug, 21 cases) | `harness/` | copy of `tmp/gi-checkpoints/validation-20260914-201909` |
| Raster, SSGI on/off | `raster-sponza-ssgi-{on,off}.pfm` | Release, 64 frames, 960x540, camera `0 4.2 0 -0.05 1.5708` (down the atrium) |
| | `raster-material-lab-ssgi-{on,off}.pfm` | camera `0 2.5 7 -0.08 0` |
| | `raster-shadow-ssgi-{on,off}.pfm` | camera `0 6 32 -0.2 0` |
| Path trace, built-in floor | `trace-sandbox{,.direct,.indirect}.pfm` | Debug, 32 samples, camera `0 5 12 -0.35 0` |
| Path trace, harness | `harness/{open,cornell,materials}{,.direct,.indirect}.pfm` | |
| SHA-256 of every PFM above | `hashes.txt` | |
| Release benchmark | `benchmark.txt` | Sponza 4.239 ms, sandbox 3.660 ms, portal_bhop_course 2.330 ms |

Every raster and sandbox capture was byte-identical to a repeat run. The Debug
and Release raster captures of `gi_material_lab.json` are also byte-identical.
The sandbox trace's indirect mean is 0.0077 against a direct mean of 0.63,
consistent with light passing through the transmissive built-in floor.

### Milestone 1: separate the floor's trace parameters

- Build `_floorMaterial.traceParameters` explicitly as `(0, 0.8, 0, 1.5)`
  instead of copying `metal_rough_factors`. Do the same for the wall, player,
  and portal materials, even though their `z` is currently 0.

**Acceptance:** the `sandbox.json` path-trace capture changes, because its
floor stops being transmissive. The `open`, `cornell`, and `materials` hashes
do not change, because their floors use editor materials. Raster captures are
bit-identical.

### Milestone 2: vertex layout

1. Delete `colored_triangle_mesh.vert`.
2. Delete `loadGltfMeshes`.
3. Move `struct Vertex` into `shaders/vertex.glsl` without changing it.
4. Append `tangent`, with `static_assert`s, and fill it from glTF, the
   generator, and the built-in meshes. No shader reads it yet.

**Acceptance:**

- `grep "struct Vertex" shaders` finds only `vertex.glsl`.
- Raster captures and path-tracer hashes are identical to Milestone 0 (compare
  against the saved artifacts, not just a repeat run).
- All 21 harness cases pass.
- The vertex-count log line reports Sponza's added memory.

### Milestone 3: material data plumbing

1. Name the `MaterialConstants` slots in C++ and GLSL.
2. Move the checkerboard flag to `materialFlags.y`.
3. Add `cameraPosition` and `materialSettings` to `GPUSceneData`.
4. Add `_flatNormalImage` and binding 3; load normal textures and scale.

**Acceptance:**

- No visual change: captures are identical to Milestone 1.
- The loader log reports 24 normal maps bound for Sponza.
- No validation errors across harness, resize, scene reload, and portal
  placement.

### Milestone 4: BRDF, emission, and outputs

1. Add `material_brdf.glsl` with the sun term, the ambient split, and emission.
2. Sample metallic/roughness textures.
3. Change `outAlbedo` and `outDirectLighting` as specified.
4. Switch the portal shader to the shared include.
5. Add the toggles and the non-normal debug views.

Normal maps stay disabled.

**Acceptance:**

- **Reference parity.** With `Match reference renderer` on, shadows disabled in
  both renderers, SSGI off, and a fixed camera, the raster direct-diffuse plus
  direct-specular image of `gi_material_lab.json` matches the path tracer's
  direct image (`.direct` capture) within a tolerance chosen from the first
  measured result and then fixed in this document.
- A roughness sweep (cubes at roughness 0.05 to 1.0, metallic 0 and 1) shows
  highlight width growing with roughness, and no black metals in shadow.
- Main camera and portal camera views of the same material are visually
  identical with SSGI off.
- `raster-ssgi` and `ssao-buffer-lab` harness cases pass. New raster baselines
  are recorded, because this milestone intentionally changes the image.
- Sponza frame time stays within +0.6 ms of Milestone 0.

### Milestone 5: normal mapping

1. Add the tangent-frame outputs in `mesh.vert` and `portal_view.vert`.
2. Add normal-map sampling in `material_brdf.glsl`.
3. Add specular anti-aliasing.
4. Add the remaining debug views.

**Acceptance** (carried over from Sponza design section 2):

- Toggling normal maps shows clear relief on Sponza's capitals, bricks, and
  floor tiles, and silhouettes do not change.
- A mirrored UV island and a negatively scaled object both light from the
  correct side (the tangent-handedness view confirms it).
- `structure.glb`, with generated tangents, shows no inverted or swimming
  lighting.
- A slow camera move across Sponza's floor at distance shows no specular
  shimmer with specular anti-aliasing on.
- Masked materials still discard consistently in the forward, portal, prepass,
  and shadow passes.
- Sponza frame time stays within +1.0 ms of Milestone 0 in total.

### Milestone 6 (separate design): image-based lighting

Split-sum image-based lighting from the selected skybox: diffuse irradiance, a
prefiltered specular mip chain, and a BRDF lookup table. It replaces
`env_brdf_approx` over a flat colour with a real environment, and gives the
portal substitute the same data. It needs its own document because it touches
skybox loading, startup time, and the SSGI environment-miss convention.

### Milestone 7 (optional): editor material textures

Add normal and metallic/roughness texture paths to `SceneMaterial`, the
inspector, and scene JSON. Scenes that omit them keep loading unchanged.

## Verification summary

| Check | M1 | M2 | M3 | M4 | M5 |
|---|:-:|:-:|:-:|:-:|:-:|
| Debug and Release build | ✓ | ✓ | ✓ | ✓ | ✓ |
| `python -m unittest discover -s tests` | ✓ | ✓ | ✓ | ✓ | ✓ |
| Full harness, 21 cases | ✓ | ✓ | ✓ | ✓ | ✓ |
| Raster captures identical to baseline | ✓ | ✓ | ✓ | new baseline | new baseline |
| Path-tracer hashes identical | `sandbox.json` changes | ✓ | ✓ | ✓ | ✓ |
| Raster/reference direct parity | | | | ✓ | ✓ (toggles off) |
| Release benchmark recorded | | ✓ | | ✓ | ✓ |
| Manual: portal, resize, reload, debug views | | ✓ | ✓ | ✓ | ✓ |

## Risks and traps

| Risk | Effect | Mitigation |
|---|---|---|
| A shader keeps the 48-byte `Vertex` | Garbled geometry, no validation error | One `vertex.glsl`; grep gate in M2 acceptance |
| Tangent transformed by the inverse transpose | Wrong relief under non-uniform scale | Model matrix plus Gram-Schmidt (section 3) |
| Negative-determinant transforms | Relief lit from the wrong side | Sign flip by `determinant(model)` |
| Checkerboard flag reaches `traceParameters` again | Transmissive floor in the tracer | M1 builds trace parameters explicitly |
| Sun term missing π | Rough dielectrics darken by 1/π | Light-unit convention documented; M4 parity check |
| Specular written to `outDirectLighting` | SSGI carries highlights, history flickers | Output table in section 8 |
| `outAlbedo` still includes metals | Metals glow with diffuse indirect | Output table in section 8 |
| Normal-mapped normal used for shadow bias | Shadow acne returns | Geometric normal for `sunlight_visibility` |
| One descriptor write site forgets binding 3 | Validation error on editor materials | Shared write helper in section 5 |
| `RenderDebugViewNames` size not updated | Compile error, or wrong names in UI | Update enum and array in one commit |
| Normal maps make raster disagree with tracer | Parity checks fail spuriously | `Match reference renderer` toggle |
| MSBuild misses edits on OneDrive | Stale-object link errors | Touch the file (see Sponza handoff) |

## Files expected to change

| File | Change |
|---|---|
| `src/vk_types.h` | `Vertex::tangent`, `GPUSceneData::cameraPosition` and `materialSettings` |
| `src/vk_engine.h` | named `MaterialConstants`, `MaterialResources` normal fields, `_flatNormalImage`; remove `loadGltfMeshes` friend |
| `src/vk_engine.cpp`, `src/vk_engine_path_trace.cpp` | M0 test-only raster capture and SSGI disable |
| `src/vk_loader.cpp` | read `TANGENT`, generate tangents, bind normal texture; delete `loadGltfMeshes` |
| `src/vk_loader.h` | remove `loadGltfMeshes` |
| `src/vk_engine_resources.cpp` | built-in tangents, flat normal image, explicit trace parameters |
| `src/vk_engine_materials.cpp` | binding 3 in `materialLayout` and `write_material` |
| `src/vk_engine_scene_materials.cpp` | flag migration, shared descriptor write |
| `src/vk_engine_renderer.cpp` | camera position and material settings in `build_scene_data` |
| `src/vk_engine_portals.cpp` | virtual camera position in `build_portal_scene_data` |
| `src/vk_engine_render_helpers.h` | new debug view names |
| `src/vk_engine_editor.cpp` | toggles and debug view list |
| new `src/tangents.h/.cpp` | `generate_tangents` |
| new `shaders/vertex.glsl` | shared `Vertex` and buffer reference |
| new `shaders/material_brdf.glsl` | shared BRDF, ambient split, specular anti-aliasing |
| `shaders/scene_data.glsl` | new fields |
| `shaders/input_structures.glsl` | named material slots, `normalTex` |
| `shaders/mesh.vert`, `portal_view.vert` | tangent frame |
| `shaders/portal_mask.vert`, `depth_normal*.vert`, `shadow_depth*.vert` | include `vertex.glsl` |
| `shaders/mesh_shading.glsl`, `portal_view_shading.glsl` | use shared include and new outputs |
| `shaders/render_debug.frag` | pass through forward-written debug views |
| delete `shaders/colored_triangle_mesh.vert` | unused |

## Open questions

1. **MikkTSpace.** Vendor `mikktspace.c` now for exact glTF conformance, or
   wait until a seam is visible on real content?
2. **Path-tracer textures.** Should the tracer eventually sample normal and
   roughness maps? That requires linear CPU texel copies again, the memory cost
   `0487a85` removed; a lower-resolution copy is a possible middle ground.
3. **Diffuse model.** Lambert is chosen to match the tracer. Burley diffuse
   would need to land in both renderers at once.
4. **Parity tolerance.** The M4 threshold should be set from the first measured
   comparison, not guessed now. Who signs off on it?
5. **Sun intensity.** Should the raster sun colour and the tracer's
   `referenceLighting.sunRadiance` become one scene value once the BRDFs match?
