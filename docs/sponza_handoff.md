# Sponza Rendering Quality — Implementation Handoff

Companion to `docs/sponza_rendering_quality_design.md`. That document is the
spec; this one records which of its numbered steps are **done**, and what the
next agent needs to know to continue without re-deriving it.

Target: `assets/scenes/sponza_showcase.json`, RTX 3060 Laptop GPU.

---

## Status against the design document's implementation order

| # | Step | Status |
|---|---|---|
| 1 | Base-color uploads to sRGB | **Done, verified** |
| 2 | Bind metallic/roughness, classify texture roles | **Done, verified** |
| 3 | Split `vk_engine_renderer.cpp` | Not started |
| 4 | Request anisotropy with device-limited fallback | **Done, verified** |
| 5 | Tangents and normal mapping | Not started |
| 6 | Metallic-roughness GGX direct-light BRDF | Not started |
| 7 | Diffuse and specular IBL | Not started |
| 8 | Stabilize additive SSGI + history invalidation | Not started |
| 9 | Decide on Stage B / portal MRTs | Not started |
| 10 | Fit single shadow map to visible bounds | Not started |
| 11 | Cascaded shadows | Not started |
| 12 | UI-free post-tonemap PNG capture | Not started |
| 13 | Tune and save capture preset | Not started |

---

## What was changed

### Step 1 — base color is now sRGB

`src/vk_loader.cpp` uploaded **every** glTF image as
`VK_FORMAT_R8G8B8A8_UNORM`, so all Sponza base color was lit as though sRGB
bytes were linear values. This was the single largest color error in the raster
path.

`loadImage` now takes a `VkFormat` from the caller, because the correct format
is a property of the *role the material assigns the image*, not of the file.

Worth knowing: the software path tracer was already correct here —
`shaders/path_trace_material.glsl:1` has `decodeSRGB` and applies it to base
color. The raster path was the only consumer treating base color as linear, so
this change makes raster agree with the path tracer rather than changing both.
`create_image` stores raw bytes in `traceSource` regardless of format, so the
path tracer is unaffected by this change.

The engine's own non-glTF material path (`vk_engine_scene_materials.cpp:64`) and
the skybox already used `VK_FORMAT_R8G8B8A8_SRGB`. Only the glTF loader was wrong.

### Step 2 — metallic/roughness textures are bound; images cached by role

Two separate problems:

1. `resources.metalRoughImage` was left at `engine->_whiteImage` **even when the
   glTF supplied a metallicRoughness texture**. The descriptor slot
   (`metalRoughTex`, set 1 binding 2) existed and was never populated from the
   asset. Now wired via `source->pbrData.metallicRoughnessTexture`.
2. Images are no longer loaded eagerly into a flat `std::vector`. There is now a
   lazy cache keyed by `(image index, color-space class)`:

   ```cpp
   const uint64_t key = uint64_t(imageIndex) * 2 + (srgb ? 1 : 0);
   ```

   This is required because one glTF image may legitimately be referenced as
   both base color and as a normal map, needing two GPU images with different
   formats. Sharing one would decode normals through the sRGB curve.
   Side benefit: images no material references now cost nothing.

A `resolveTexture` helper resolves a `fastgltf::TextureInfo` into image +
sampler, leaving the caller's default intact when the reference is absent or
dangling.

**Note for step 6:** the metallic/roughness texture is now *bound but not
sampled*. `shaders/mesh_shading.glsl` does not read `metalRoughTex` yet. This
has no visual effect until the BRDF lands.

### Step 4 — anisotropic filtering

`samplerAnisotropy` was never requested anywhere, so no sampler could use it.

- `src/vk_engine.cpp` requests it via `physicalDevice.enable_features_if_present`
  — *requested, not required*, per the design doc's fallback requirement, so a
  device lacking it still runs on trilinear instead of failing device selection.
- `_maxSamplerAnisotropy` is read from `VkPhysicalDeviceProperties::limits` and
  forced to `1.0f` when the feature is off (a device reports a limit of 16 even
  when the feature is disabled, and a sampler asking for it would be invalid).
- `VulkanEngine::material_anisotropy()` in `vk_engine.h` clamps the preset's
  `_textureAnisotropy` to the device limit and returns `1.0f` when unsupported,
  so callers can assign it unconditionally — `1.0` means "off".
- Applied to the glTF samplers (`vk_loader.cpp`) and to `_defaultSamplerLinear`,
  which is the fallback for glTF textures with no sampler index.
- Only enabled for `VK_FILTER_LINEAR` minification; `NEAREST` textures stay crisp.

**Not yet done:** `_textureAnisotropy` is applied at sampler-creation time only.
The quality presets do not set it, and changing it needs a scene reload to take
effect. Wiring it into `apply_*_settings` belongs with the preset work (step 13).

### Texture memory reporting

Added to `vk_loader.cpp` because the design document asks for estimated
allocation before upload, and because it makes the format classification
observable. Per scene it prints sRGB/linear image counts, estimated MB including
mip chains (base level × 4/3), and how many materials bound each role.

---

## Verified output

Build: clean. Run: `MIRABILIS_TEST_SCENE=sponza_showcase.json MIRABILIS_TEST_FRAMES=8`
from `bin/Debug`, 8 frames, no validation errors.

```
GPU: NVIDIA GeForce RTX 3060 Laptop GPU (anisotropy up to 16x)
GLTF textures: 25 sRGB + 24 linear, ~1045.3 MB with mips;
  25/28 materials bound base colour, 24/28 metallic-roughness
  (NewSponza_Main_glTF_003.gltf)
```

Two things this measures:

- **24 of 28 Sponza materials have a metallic/roughness texture that was
  previously never bound.** Under the lazy cache a linear image only exists if a
  material actually requested it, so the 24 linear images are themselves the
  proof the new binding path resolves.
- **~1.05 GB of material textures for Sponza alone**, at the 2048 cap, with mips.
  This is the first real measurement against design goal 6 (fit the 3060's VRAM
  budget) and it is large. BC7/BC5 compression (section 1) would cut this ~4x.
  Worth treating as a priority rather than a later nicety.

---

## Useful facts for whoever continues

### Automation hooks (already exist — do not rebuild them)

- `MIRABILIS_TEST_SCENE=<file.json>` — startup scene (`vk_engine_resources.cpp:489`)
- `MIRABILIS_TEST_FRAMES=<n>` — run n frames and exit; the only way to verify
  rendering changes without driving the GUI
- `MIRABILIS_RENDER_DEBUG_VIEW=<n>` — open directly on a `RenderDebugView`
- `MIRABILIS_SSGI_PRESET`, `MIRABILIS_MAX_FIDELITY`, `MIRABILIS_TEST_CAMERA`
- `MIRABILIS_CAPTURE`, `MIRABILIS_SSGI_CAPTURE` — PFM + metadata sidecar

Working directory must be `bin/Debug` or `bin/Release`; asset paths are
`../../assets/...`.

### Build note

MSBuild intermittently fails to notice edited sources in this tree (OneDrive
timestamps), producing a stale-object `LNK2019` for a function that plainly
exists. If that happens, touch the file:

```powershell
(Get-Item src\vk_engine_resources.cpp).LastWriteTime = Get-Date
```

Also: two concurrent builds will collide on `vc145.pdb` (`error C1041`). Only run
one at a time.

### Step 3 (renderer split) — mapping is already worked out

`vk_engine_renderer.cpp` is 3378 lines. Functions map to the doc's six proposed
TUs with no ambiguity:

- **shadows**: `compute_sun_view_projection`, `init_shadow_resources`,
  `init_shadow_pipeline`, `init_shadow_mask_pipeline`, `draw_shadow_map`
- **prepass**: `init_depth_normal_resources`, `init_depth_normal_pipeline`,
  `init_depth_normal_mask_pipeline`, `draw_depth_normal_prepass`
- **ssao**: `active_ssao_extent`, `ssao_active`, `init_ssao_resources`,
  `init_ssao_pipelines`, `build_ssao_push_constants`, `draw_ssao`
  (contiguous, lines 2798-3289)
- **ssgi**: `init_ssgi_resources`, `init_ssgi_pipelines`, `draw_ssgi`,
  `draw_ssgi_composite`, `active_ssgi_extent`
- **postprocess**: `init_post_process_resources`, `init_tonemap_pipeline`,
  `init_fxaa_pipeline`, `draw_tonemap`, `draw_fxaa`
- **render_debug**: `init_render_debug_pipeline`, `draw_render_debug`,
  `draw_collider_debug_bounds`

Staying in `vk_engine_renderer.cpp`: `draw_background`, `init_descriptors`,
`init_pipelines`, `draw_geometry`, `build_scene_data`,
`init_background_pipelines`, `init_gpu_timestamps`, `read_gpu_timestamps`.

Watch for: the anonymous namespace at the top holds `normalized_sun_direction`
(needed by shadows) and `set_previous_world_rows` (needed by geometry/prepass).
`is_visible` is non-static and used by several. Add each new `.cpp` to
`src/CMakeLists.txt`. Verify "no change to rendered output" with an
`MIRABILIS_SSGI_CAPTURE` PFM diff before and after.

**This step was deliberately not started** because `vk_engine_renderer.cpp` had
382 uncommitted insertions at the time. Commit or stash before splitting.

### Step 5-6 traps

- `Vertex` (`vk_types.h:54`) is `position, uv_x, normal, uv_y, color` — adding
  tangent changes the vertex buffer layout and every vertex shader's input.
- `MaterialConstants` (`vk_engine.h:289`) is `colorFactors`,
  `metal_rough_factors`, `extra[14]` = **256 bytes**, almost certainly sized for
  `minUniformBufferOffsetAlignment`. Do not shrink it. `extra[0]` is the UV
  transform, `extra[1].x` is the alpha cutoff.
- The checkerboard flag currently lives in `metal_rough_factors.z`
  (`mesh_shading.glsl:33`) and must move before that component means roughness.
- `material->data.traceParameters` is copied from `metal_rough_factors`
  (`vk_loader.cpp`), feeding `path_trace_material.glsl`. Any field migration
  must update the path tracer in the same change or it silently misreads.
- `shaders/mesh_shading.glsl` and `shaders/portal_view_shading.glsl` are
  near-duplicates by design; `mesh.frag`/`mesh_mask.frag` are two-line wrappers.
  Keep shared BRDF code in a new include used by both.
- `portal_view_shading.glsl` writes **one** color attachment — no albedo,
  velocity, or direct-lighting MRT. Section 2's "identical material response"
  criterion needs an explicit decision here.

### Step 8 trap

`ssgi.comp` already stores `incident * albedo` (lines 196-198). Stage A requires
changing the trace to store incident radiance *without* albedo and applying
albedo once at composite time. If you skip that and add the doc's composite
formula, albedo gets squared.
