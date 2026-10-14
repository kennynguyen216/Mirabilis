# Mirabilis Path-Traced Global Illumination Reference Design

## Document status

This is the final reviewed design for the first Mirabilis path-traced reference renderer. It consolidates the original proposal, codebase review, implementation guardrails, and the desktop/laptop hardware policy.

The initial implementation is a slow, progressive, path-traced reference renderer. Its purpose is to produce a trustworthy linear-HDR reference against which a later screen-space global illumination implementation can be evaluated. It is not a production real-time GI effect.

## Proposal in plain language

Mirabilis will keep its existing renderer and add a setting that selects how the world is rendered:

```text
Renderer
    Raster
    Path Traced Reference (Software)
    Path Traced Reference (Hardware) [future and optional]
```

The first two choices will be implemented now:

- **Raster** runs the engine exactly as it does today, including shadows, SSAO, portals, and optional FXAA.
- **Path Traced Reference (Software)** replaces the world rendering with a slow compute-shader path tracer. It builds its own image over many frames and runs on both the RX 5700 desktop and RTX 3060 laptop.

The hardware option is reserved for a separate future project. It will only be selectable when the active Vulkan device reports all required ray-tracing extensions and features. It will be disabled with an explanation on unsupported hardware instead of causing device creation or startup to fail.

Switching renderer mode resets path-trace accumulation. Path-tracing controls and debug views appear only in a path-tracing mode. Raster settings retain their values while path tracing is active.

## Goals

- Generate primary rays from the active camera for every pixel.
- Trace those rays against the complete opaque scene rather than only visible screen data.
- Evaluate direct and indirect light transport.
- Accumulate independent Monte Carlo samples while the camera and scene remain fixed.
- Expose intermediate buffers early enough to validate each stage separately.
- Produce direct-only, indirect-only, and combined reference images.
- Keep the implementation understandable and inspectable while it is being learned.

## Initial non-goals

- Real-time performance.
- Temporal reprojection while the camera moves.
- Half-resolution tracing or learned/upscaled reconstruction.
- Denoising.
- Transparent and alpha-blended surfaces.
- Full glTF metallic-roughness shading in the first milestone.
- Light transport through linked portals in the first milestone.
- GPU-side BVH construction.
- Vulkan hardware ray-tracing extensions as the initial backend.

## Architectural decision

The reference renderer will be a separate **Path Trace** render mode rather than a GI pass inserted into the existing forward renderer.

A camera-ray path tracer computes its own primary visibility. It therefore does not need the raster depth/normal prepass or an albedo G-buffer before tracing. Requiring the geometry pass first would turn the feature into a hybrid ray-traced GI effect and would mix the reference design with the later SSGI architecture.

The two render paths should remain explicit:

```text
Raster mode
    shadow map
    depth/normal prepass
    SSAO
    background
    forward geometry
    portal composition
    raster debug views
    FXAA when enabled
    present

Path Trace mode
    generate and trace one sample per pixel
    update progressive accumulation
    resolve linear HDR result into the draw image
    path-tracer debug view or optional editor overlay
    display transform
    present
```

The existing raster order is implemented in `src/vk_engine.cpp` around `VulkanEngine::draw()`. FXAA currently runs after world and portal composition, which is correct for raster mode.

FXAA must be disabled for reference captures. Jittered primary rays provide antialiasing as the path trace converges, while FXAA would modify and blur the reference result.

## Backend decision

### Compute shader with a software BVH

The first implementation will use a Vulkan compute shader and a software BVH.

This matches the current engine:

- Compute pipelines, storage images, buffer device addresses, descriptor indexing, and synchronization2 are already used.
- The shader build currently recognizes vertex, fragment, and compute shaders.
- The desktop AMD Radeon RX 5700 does not advertise `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, or `VK_KHR_ray_tracing_pipeline`.
- The laptop RTX 3060 supports hardware ray tracing, but it has less VRAM and runs under laptop power and cooling limits.
- The current Vulkan device creation does not enable ray-tracing extensions or their feature structures.

Using the hardware ray-tracing pipeline would require device feature discovery, extension loading, acceleration-structure resources, shader-binding tables, new shader types, and CMake changes. It would also prevent the implementation from running on the desktop. That is poor leverage for the first educational reference implementation.

The software implementation is the authoritative cross-machine reference. A future hardware backend may be compared against it, but it must not replace it.

### Iterative path loop

The CPU path tracer's recursive mental model remains useful, but the compute shader will express the path as a bounded loop:

```text
radiance = 0
throughput = 1
ray = primary camera ray

for each bounce up to maxDepth:
    find closest hit
    if miss:
        radiance += throughput * environment(ray.direction)
        stop

    radiance += throughput * emittedRadiance(hit)
    evaluate explicitly sampled lights
    sample the surface BSDF
    throughput *= BSDF * abs(dot(normal, outgoing)) / PDF
    offset and continue the ray
```

Shader recursion is not required. The bounded loop also makes runtime and Windows GPU timeout risk easier to control.

## Current codebase constraints

### Geometry ownership

The current loader creates temporary CPU vertex and index vectors, uploads them through `VulkanEngine::uploadMesh()`, and retains GPU raster buffers. The CPU triangle arrays are then discarded.

The retained draw representation provides:

- An index-buffer handle.
- A vertex-buffer device address.
- A draw transform.
- A pointer to a raster material instance.

It does not provide everything needed for a CPU-built trace scene:

- Persistent CPU positions and indices.
- A stable trace material index.
- An index-buffer device address or storage-buffer access path.
- A material table suitable for one global compute descriptor set.

Scene extraction must therefore be implemented before BVH traversal. Reading geometry back from GPU memory is not the desired solution.

### Initial trace-scene representation

For the first version, create a world-space triangle array from the visible opaque scene and build one BVH over it.

```cpp
struct TraceTriangle {
    vec3 p0;
    uint materialIndex;
    vec3 p1;
    uint flags;
    vec3 p2;
    uint padding;
    vec3 n0;
    float uv0x;
    vec3 n1;
    float uv0y;
    vec3 n2;
    float padding2;
    vec2 uv1;
    vec2 uv2;
};

struct TraceBVHNode {
    vec3 boundsMin;
    uint firstChildOrTriangle;
    vec3 boundsMax;
    uint triangleCount;
};

struct TraceMaterial {
    vec4 baseColor;
    vec4 emission;
    vec4 parameters;
};
```

The exact layout should be simplified and aligned after shader/C++ layout validation. The important design properties are contiguous arrays, integer material references, and no C++ pointers in GPU data.

World-space triangle baking duplicates shared meshes such as unit cubes, but it avoids object-space ray transforms and distance comparisons under nonuniform scaling. That is an acceptable first-stage tradeoff for the current scene sizes.

A later optimization can introduce a two-level structure:

```text
Bottom-level BVH: one per unique mesh in object space
Top-level BVH: one entry per transformed scene instance
```

### BVH construction

The first BVH will be built on the CPU and uploaded to GPU storage buffers whenever the trace scene changes.

Start with a median split along the longest centroid axis and small leaves. A binned surface-area-heuristic builder can replace it later without changing shader-facing node semantics.

A BVH does not guarantee `O(log n)` ray traversal. A well-built hierarchy often approaches that behavior, while degenerate geometry or poor splits can still approach linear traversal. Debugging should therefore include node-visit and triangle-test counts.

Traversal in the compute shader will be iterative with an explicit stack. The implementation must guard against stack overflow and validate child indices before relying on rendering results.

## Hit record and intersection rules

A trace hit must contain enough data to continue the path:

- World-space hit position.
- Ray parameter `t`.
- Geometric normal.
- Interpolated shading normal when available.
- UV coordinates.
- Barycentric coordinates when useful for debugging.
- Stable material index.
- Front-face flag.

The geometric normal determines front/back orientation, ray-origin offset, and valid hemispheres. The interpolated shading normal determines the BRDF appearance but must not allow scattering below the geometric surface.

For each accepted hit:

```text
frontFace = dot(ray.direction, geometricNormal) < 0
orientedGeometricNormal = frontFace ? geometricNormal : -geometricNormal
orientedShadingNormal   = frontFace ? shadingNormal   : -shadingNormal
```

The next ray must start slightly off the surface in the correct geometric-normal direction. Moller-Trumbore intersection also needs a positive `tMin` to prevent a ray from immediately hitting the triangle it just left.

## Materials and light transport

### First material scope

The first complete milestone will support diffuse Lambertian materials only. This is sufficient to validate intersections, occlusion, multi-bounce transport, and color bleeding.

The current engine stores metallic and roughness values, but its raster fragment shader presently evaluates base color, ambient illumination, and Lambertian directional sunlight. It does not yet implement a metallic-roughness BRDF. RTIOW-style perfect/fuzzy metal and dielectric scattering would also not match glTF's metallic-roughness material model.

After the diffuse reference works, the material progression should be:

1. Constant diffuse color.
2. Base-color textures and interpolated UVs.
3. Emissive materials.
4. GGX metallic-roughness reflection matching the declared glTF model.
5. Dielectric transmission with an explicit index of refraction.
6. Alpha masking and transparency as separate, deliberately designed features.

The trace renderer needs its own packed material table. Existing per-material Vulkan descriptor sets and C++ `MaterialInstance*` pointers are raster-specific and should not become the trace-scene ABI.

### Sampling strategy

Use cosine-weighted hemisphere sampling for Lambertian surfaces.

For a Lambertian BRDF:

```text
BRDF = albedo / pi
PDF  = cos(theta) / pi
```

The Monte Carlo throughput factor simplifies to albedo:

```text
throughput *= BRDF * cos(theta) / PDF
throughput *= albedo
```

Uniform hemisphere sampling is valid but noisier. Its PDF is `1 / (2*pi)`, making the throughput factor `2 * albedo * cos(theta)`.

### Explicit lights

Environment sampling alone cannot reproduce the existing directional sunlight. An ideal directional light has zero angular extent, so a random hemisphere direction has zero practical probability of finding it.

At a diffuse hit, explicitly evaluate the sun direction:

1. Cast a shadow ray toward the sun.
2. If unoccluded, add the Lambertian direct-light estimate.
3. Continue the independently sampled bounce ray for indirect transport.

When an emissive rectangle is introduced, sample a point on its area and divide by the corresponding area or solid-angle PDF. Multiple importance sampling can wait until both BSDF and light sampling work independently.

### Environment

Begin with a procedural sky gradient because it validates primary rays and miss handling without texture plumbing. Upgrade later to an HDR environment texture stored in a floating-point format.

The existing skybox loader is an 8-bit display texture path and is not an HDRI ingestion pipeline.

## Accumulation design

### Progressive accumulation

The reference uses progressive accumulation only while all rendering inputs remain unchanged. It will not reproject history between camera positions.

Use a running average rather than an exponential moving average. Every sample should retain equal weight so that the estimate converges toward the expected value.

Recommended resources:

- `R32G32B32A32_SFLOAT` accumulation image.
- `R16G16B16A16_SFLOAT` resolved display image, using the existing draw-image format.
- One global unsigned sample count while every pixel receives one sample per dispatch.

If adaptive or tiled sampling later gives pixels different sample counts, add an `R32_UINT` per-pixel count image.

The engine must query storage-image support for the selected accumulation format, following the capability-selection pattern already used by SSAO.

### Reset policy

Reset the accumulation fully when any input that changes the integrand or camera sampling changes:

- Camera transform, FOV, or projection.
- Draw extent or render scale.
- Scene load, object creation/deletion, visibility, hierarchy, or transform.
- Material color, texture, metallic, roughness, or emission.
- Light direction, radiance, environment, or exposure when exposure is folded into accumulation.
- Maximum depth or sampling mode.
- Trace geometry or BVH revision.

Use a dedicated monotonically increasing trace-scene revision. The current `_sceneDirty` flag represents unsaved editor changes and should not double as GPU trace-resource validity.

The simplest correct behavior during camera movement is to reset and display the new low-sample result. Reprojection, clamping, and adaptive history weights belong to later real-time GI work, not the ground-truth renderer.

### Work granularity

Dispatch one sample per pixel per frame initially. Do not put hundreds of samples into one long-running shader invocation. A large full-screen dispatch with many bounces can exceed the Windows GPU timeout even though interactive frame rate is not a goal.

If one sample per full frame is still too slow, trace tiles across multiple submissions while preserving exact per-pixel counts.

## Random-number generation and camera rays

Each sample needs a deterministic seed derived from at least:

- Pixel coordinates.
- Accumulated sample index.
- A user-controlled base seed.

A small integer generator such as PCG is preferable to sine-based hashes for repeated bounce sampling. Every bounce must advance the generator rather than hash only the original pixel again.

Primary rays should use subpixel jitter. Ray direction must match the current 70-degree vertical FOV, aspect ratio, Vulkan Y convention, and the camera's negative-Z forward convention. The existing sky compute shader is a useful reference for camera basis construction.

A fixed seed mode is required for reproducible comparisons and debugging.

## Two-machine development policy

Development takes place on two materially different systems:

| | Desktop | Laptop |
| --- | --- | --- |
| CPU | Ryzen 5 5600X, 6 cores / 12 threads | Ryzen 9 5900HS, 8 cores / 16 threads, if the laptop is the identified 2021 G14 configuration |
| GPU | Radeon RX 5700, 8 GB | RTX 3060 Laptop GPU, 6 GB |
| Hardware ray tracing | Unavailable | Available with a current driver |
| Primary role | Main software-tracer development and long-running captures | NVIDIA cross-checks and possible future hardware-RT experiments |

Both machines must run the software path. Optional hardware-ray-tracing features must be discovered at runtime and must never be required to start the engine.

The tracer must avoid assumptions tied to one vendor:

- Query storage-image formats and optional features.
- Do not hardcode subgroup size or depend on subgroup operations initially.
- Bounds-check incomplete workgroups at image edges.
- Use finite-value checks around PDFs, normalization, and intersections.
- Keep random-number generation integer-based and deterministic.
- Label timings with GPU model and driver version.

Checkpoints 1, 3, and 5 must be verified on both machines before they are accepted. The same committed source, scene revision, build configuration, seed, camera, and settings must be used.

Cross-vendor images are not required to be bit-identical. They must agree on geometry, materials, and expected radiance within documented tolerances and Monte Carlo noise. Primary hit/miss and material assignment should agree except for deliberately identified floating-point boundary cases.

Every retained reference capture must record:

- GPU model and driver version.
- Vulkan API version and relevant queried capabilities.
- Build configuration and source revision.
- Scene and trace-scene revision.
- Image dimensions and render scale.
- Base seed, sample count, and maximum depth.
- Light, environment, and material settings.

## Image formats and color pipeline

All tracing, accumulation, and SSGI comparison must occur in linear HDR space.

The current renderer uses an `R16G16B16A16_SFLOAT` draw image and blits it to a `B8G8R8A8_UNORM` swapchain. There is no explicit exposure, tone-mapping, and output-gamma stage. A path-traced image should not be evaluated visually until that display transform exists.

Add a resolve/display pass with this conceptual order:

```text
linear accumulated radiance
    exposure
    tone mapping
    linear-to-display encoding as required by the chosen swapchain path
    presentation
```

Quantitative comparison against SSGI should read the linear HDR buffers before this transform.

Base-color textures must also enter the shader in the correct color space. The repository currently has a mismatch: editor scene textures use an sRGB image format, while glTF images are uploaded as UNORM. This must be corrected or explicitly decoded before material comparisons are meaningful.

## Direct and indirect reference outputs

The tracer should track these values separately:

```text
directRadiance   = light reaching the first visible surface directly
indirectRadiance = contribution reaching it after one or more additional surface bounces
combinedRadiance = directRadiance + indirectRadiance
```

The exact classification must remain consistent when emissive hits and specular chains are introduced. The initial diffuse-only implementation can classify the contribution by bounce index.

The later SSGI implementation should be graded primarily against `indirectRadiance`, using the same camera, geometry, materials, environment, and exposure-independent linear values.

## Debug infrastructure

The existing renderer already exposes depth, normals, reconstructed positions, and individual SSAO stages through `RenderDebugView`. Extend the same UI pattern with path-tracing views:

- Primary-hit geometric normal.
- Primary-hit shading normal.
- Linear hit distance.
- Albedo.
- Material ID rendered as a stable false color.
- BVH node visits.
- Triangle intersection tests.
- One unaccumulated noisy sample.
- Accumulated radiance.
- Direct radiance only.
- Indirect radiance only.
- NaN and infinity detection.

Debug views that describe ray hits should come from the ray tracer. The raster depth/normal views remain valuable as a separate visual cross-check of primary visibility.

## Validation scenes

Use two stages rather than one contradictory Cornell setup.

### Open environment box

- Red and green opposing walls.
- Neutral floor, ceiling pieces, and central objects.
- A deliberate opening through which the procedural sky is visible.
- Diffuse materials only.

This scene validates environment misses, diffuse bouncing, and color bleeding before explicit area-light sampling exists.

### Closed Cornell box

- Fully enclosed diffuse room.
- Red and green opposing walls.
- Neutral objects.
- Emissive ceiling rectangle.
- Black environment initially.

This scene validates emissive materials, explicit light sampling, shadow rays, and indirect illumination without exterior light leaking into the result.

An enclosed box with only an exterior environment must render black. Increasing `maxDepth` cannot fix that because no valid light path connects the interior surfaces to the environment.

## Portal policy

Portals are excluded from the initial tracer but must not be silently treated as normal colored walls in final reference images.

The eventual physically meaningful behavior is:

1. Intersect a portal aperture.
2. Transform the ray origin and direction through the linked portal frames.
3. Continue traversal in the same scene.
4. Count the traversal against a separate portal limit to prevent infinite cycles.

Portal transport should be implemented only after ordinary BVH traversal, materials, accumulation, and direct lighting are validated in portal-free scenes.

## Scene and lighting data changes

The existing scene format stores sun direction and shadow tuning, while ambient and sunlight color/intensity are hardcoded in `build_scene_data()`. The reference renderer needs explicit scene-authored radiometric inputs.

Add scene fields for:

- Sun radiance or color plus intensity.
- Environment selection and intensity.
- Emissive material color and strength.

Exposure is a display setting and should not affect accumulated physical radiance. Changing exposure therefore should not require an accumulation reset if it is applied only in the display pass.

## Implementation milestones

### Milestone 0: clean baseline

- Remove the stray backtick currently present in `src/camera.cpp`.
- Confirm the Debug build succeeds.
- Confirm Vulkan validation reports no new errors during the existing raster path.

### Milestone 1: compute and camera-ray plumbing

- Add the render-mode switch.
- Add path-trace image resources and descriptors.
- Generate camera rays in a compute shader.
- Render a procedural environment gradient directly into the draw image.
- Validate orientation, FOV, aspect ratio, render extent, and image transitions.
- Compare GPU ray directions at the center, edges, and corners with CPU-calculated expected directions.

Visible result: the compute path shows the same stable world-oriented gradient as the camera rotates.

### Milestone 2: trace-scene extraction

- Preserve or reconstruct CPU mesh data at load time.
- Create the trace triangle and material arrays.
- Bake current world transforms.
- Upload the arrays to GPU storage buffers.
- Add a dedicated trace-scene revision.
- Verify selected GPU records through a diagnostic shader, readback, or CPU/GPU checksum.

Verified result: GPU-visible record counts, values, bounds, and material indices agree with the CPU source. Spatial false-color rendering waits until primary intersection exists.

### Milestone 3: BVH and primary visibility

- Build a CPU BVH.
- Implement ray/AABB and Moller-Trumbore intersection.
- Traverse with an explicit GPU stack.
- Add hit normal, depth, albedo, node-visit, and triangle-test views.
- Cross-check silhouettes against the raster prepass.
- Compare deterministic BVH intersections against brute-force triangle intersections.

Visible result: correct primary-hit normals and albedo with no lighting.

### Milestone 4: diffuse light transport

- Add cosine-weighted Lambertian sampling.
- Add ray-origin offsets and maximum depth.
- Add environment miss radiance.
- Define maximum depth as the allowed number of surface scattering events.
- Validate depth 1 for first-surface environment illumination.
- Validate depth 2 for the first possible diffuse color bleeding path.

Visible result: depth 1 illuminates visible diffuse surfaces from the environment, and depth 2 produces noisy but recognizable color bleeding in the open environment box.

### Milestone 5: progressive reference accumulation

- Add `RGBA32F` running-average accumulation.
- Add sample count and deterministic seeds.
- Implement complete reset invalidation.
- Add unaccumulated and accumulated debug views.
- Feed known constant samples through the accumulator and verify the numerical running average.

Visible result: a fixed camera converges progressively and resets immediately after any relevant edit.

### Milestone 6: explicit direct lighting

- Add sun shadow rays.
- Add emissive material data.
- Add emissive rectangle sampling.
- Separate direct, indirect, and combined outputs.

Visible result: a sealed Cornell box is lit correctly and shows stable color bleeding as samples accumulate.

### Milestone 7: material fidelity

- Correct texture color-space handling.
- Add base-color texture sampling.
- Add GGX metallic-roughness reflection.
- Add dielectric transmission.
- Add Russian roulette and, if needed, multiple importance sampling.

### Milestone 8: portal transport

- Trace through linked apertures.
- Add a portal traversal limit.
- Validate one portal pair before recursion and authored multi-pair scenes.

## Acceptance criteria

The initial reference renderer is ready for use when:

- A fixed camera produces deterministic results for a fixed seed.
- Primary-hit debug views agree with raster visibility on opaque portal-free scenes.
- The open-box scene demonstrates expected red/green diffuse color bleeding.
- The sealed Cornell box remains black without an internal emitter and becomes illuminated when its emitter is enabled.
- Direct and indirect components add to the combined linear-HDR result.
- Accumulation resets on every relevant camera, scene, material, lighting, and resolution change.
- No NaN or infinity values survive into the accumulation image.
- FXAA, SSAO, raster ambient light, and raster shadow maps do not contaminate reference captures.
- The result can be inspected before tone mapping for later SSGI comparisons.
- Compute dispatch duration stays below the operating system's GPU timeout threshold.

## Risks and mitigations

| Risk | Consequence | Mitigation |
| --- | --- | --- |
| CPU mesh data is unavailable after upload | BVH cannot be built without readback | Preserve traceable geometry during loading and primitive creation |
| Raster and trace materials diverge | Comparisons measure material differences instead of GI | Define a shared material meaning and begin with Lambertian-only scenes |
| Incorrect color spaces | Brightness and color bleeding comparisons become invalid | Keep accumulation linear, fix base-color decoding, and add an explicit display transform |
| Long compute dispatch triggers Windows TDR | Driver reset or application loss | Use one sample per pixel per dispatch or tiled submissions |
| Scene edits leave stale accumulation | Mixed images from different scenes | Use a dedicated trace-scene revision and full reset policy |
| BVH stack overflows | Missing geometry or device fault | Bound the stack, instrument maximum depth, and validate node indices |
| Portal behavior is undefined | Reference disagrees fundamentally with Mirabilis scenes | Exclude portals initially and later implement ray teleportation explicitly |
| Small lights converge poorly | Excessive noise hides GI correctness | Add explicit light sampling before judging the closed Cornell box |

## Resolved design questions

| Question | Decision |
| --- | --- |
| Can ray tracing be turned on and off? | Yes. Use a renderer-mode setting with Raster and Software Path Trace initially |
| Compute or hardware ray tracing? | Software compute is the shared reference; hardware ray tracing is a future optional backend |
| CPU or GPU BVH build? | CPU build and GPU upload for the initial static trace scene |
| Accumulation format? | `RGBA32F` accumulation, `RGBA16F` resolved display |
| Where does direct lighting live? | Inside the standalone reference tracer; expose direct and indirect separately |
| Camera movement policy? | Full accumulation reset |
| Half-resolution GI? | No for reference captures; full resolution at render scale 1.0 |
| Hemisphere sampling? | Cosine-weighted Lambertian sampling with correct PDF accounting |
| Reprojection? | Deferred to later real-time GI work |
| FXAA on the reference? | Disabled |
| Portals in the first milestone? | Excluded until ordinary path tracing is validated |

## Checkpoint protocol

Complete one checkpoint at a time. Implement the smallest compilable vertical slice that satisfies its pass conditions, including the instrumentation required to verify it. Changes explicitly listed in the checkpoint are in scope. Minimal shared scaffolding is allowed when it is required to keep that checkpoint compilable, but later checkpoint behavior must not be implemented early.

The restriction on the raster path applies to its behavior and output. Shared initialization, cleanup, UI, and `VulkanEngine::draw()` may be edited to introduce the render-mode branch, provided the existing Raster mode remains behaviorally unchanged and is retested.

Before editing:

1. Record `git status --short` and preserve all pre-existing user changes.
2. State the checkpoint, pass condition, planned files, and why each file is involved.
3. Identify assumptions that cannot be verified from the repository.

Before presenting a checkpoint:

1. Build the Debug configuration.
2. Run appropriate CPU invariants and numerical tests.
3. Run with Vulkan validation enabled and report any messages introduced by the change.
4. Capture the specific visible or readback result required by the checkpoint.
5. Report every changed file and any scope expansion.
6. Explain the most likely subtle remaining defect and how it would appear later.

A checkpoint is complete only when its pass conditions are observed. Debug instrumentation may be built alongside the feature it measures, but the feature is not accepted before that instrumentation is working.

Stop and request a design decision when:

- Meeting the checkpoint requires behavior outside its approved scope.
- A difficult-to-reverse architecture choice remains genuinely ambiguous.
- Raster output would need to change.
- Vulkan validation errors remain after a reasonable attempt to localize them.
- CPU and GPU reference tests disagree without an understood tolerance.
- A compute dispatch cannot be kept comfortably below the Windows GPU timeout through bounded work or tiling.

Planned loader, descriptor, buffer, and resource changes named by the active checkpoint do not require a separate stop. Unplanned refactors do.

After every checkpoint, the implementation report must answer:

- What changed, by file, and why?
- What exact result was observed?
- How can the user verify it?
- What assumptions remain?
- What is most likely to be subtly wrong?
- Did the work expand beyond the checkpoint?

After each significant milestone, answer a short conceptual check about the new component before moving to the next one. The purpose is to ensure the implementation remains understood rather than merely integrated.

## Codex implementation recommendation

Use **GPT-6 Astra with high reasoning effort** as the primary implementation model. This work combines Vulkan synchronization, GPU memory layout, C++ ownership, shader math, BVH correctness, and long multi-file checkpoints, so the highest-capability coding and reasoning model is appropriate.

Use `xhigh` reasoning for the BVH/intersection checkpoint or for a difficult cross-vendor defect. `high` should be sufficient for normal milestone implementation and will usually be more efficient.

If GPT-6 Astra is unavailable, use **GPT-5.6 Sol with high reasoning effort**. Use **GPT-5.6 Terra** for smaller mechanical follow-ups such as UI labels, documentation, and isolated cleanup after the architecture is settled.

Current official model guidance describes GPT-6 Astra as the flagship for the hardest end-to-end reasoning and coding work, GPT-5.6 Sol as a flagship for complex professional work, and GPT-5.6 Terra as the balance of intelligence and cost: <https://developers.openai.com/api/docs/models>.

Give the model this document as the controlling implementation specification. Start each request with exactly one milestone and require it to stop after producing and verifying that milestone's pass condition.

## Desktop implementation addendum (2026-09-12)

The desktop software path through Milestones 0-8 has been implemented. The detailed evidence, commands, source/worktree state, remaining acceptance work, and file inventory are in [gi_desktop_implementation_report.md](gi_desktop_implementation_report.md). This addendum records final implementation choices; the earlier architecture and physical-lighting requirements remain controlling.

The current delivery is restricted to the AMD Radeon RX 5700 desktop. Laptop acceptance for checkpoints 1, 3, and 5 remains pending; it is not a prerequisite for carrying out the user's authorized desktop milestones. No hardware RT, RTX backend, laptop-specific implementation, or cross-machine portal transport is included. Existing same-scene portal links are supported in Milestone 8.

### Concrete desktop decisions

- Startup defaults to Raster. In the editor (Tab), Render Settings > Renderer selects Raster or Path Traced Reference (Software). Missing shader/pipeline or unavailable compute/RGBA32F storage keeps Raster with a status message. Returning to trace mode starts fresh accumulation.
- Reference defaults: one sample per pixel per dispatch, two surface scattering events (maximum four), two portal traversals (maximum four), seed 1337, zero exposure EV, full render scale 1.0, accumulated view, GGX plus perfect dielectric materials. Lambertian mode remains selectable. Full resolution is intentional for reference captures. Lower render scale is an explicit user quality/performance tradeoff.
- CPU-built world-space BVH uses four-triangle leaves, median longest-centroid-axis splits, maximum build depth 48, and checked 64-entry traversal stacks. Immutable shared CPU mesh/texture snapshots preserve geometry without GPU readback. CPU scene replacement waits for all GPU readers.
- Final packed ABI sizes are triangle 176 bytes, material 80 bytes, BVH node 32 bytes, lighting 64 bytes, portal 192 bytes, and push constants 112 bytes. CPU assertions and GPU readback diagnostics check these interfaces.
- Combined, direct and indirect histories are RGBA32F. Combined RGB is explicitly the sum of the component running averages. Combined alpha retains invalid-sample counts until reset; invalid radiance is suppressed and the diagnostic view marks faults. Captures reject faults/nonfinite pixels. A single per-frame sample index gives deterministic fixed-seed runs on the tested desktop.
- Exposure/Reinhard/sRGB conversion writes RGBA16F display pixels. PFM files retain linear HDR RGB before that conversion; BMP files provide a display preview. `scripts/validate_software_trace.ps1` saves settings, source/scene hashes, the working patch, and executable/shader hashes with the captures.
- The draw and history allocation follows the existing startup 1280x720 draw allocation. Smaller/odd window extents and render-scale changes reset history; larger windows upscale the existing maximum draw extent. Reallocating the raster targets is outside this implementation. Minimize pauses submissions; restore resumes through the existing swapchain lifecycle.
- Reference lighting is scene-authored in `referenceLighting`; material emission color/strength, transmission and IOR are optional backward-compatible JSON fields. Existing raster shading and shader formats are preserved. Base-color textures are explicitly decoded from sRGB before repeating bilinear level-zero filtering. Other glTF texture channels and advanced material extensions are not claimed.
- Emissive triangles are sampled with area/selection PDFs, and the directional sun has explicit visibility rays. Non-delta BSDF emitter hits are omitted to avoid counting area lighting twice; this is a next-event estimator without MIS. Environment illumination uses BSDF sampling. Perfect-specular chains can converge poorly and nested dielectric media are not represented by a medium stack.
- Portal apertures use the engine's rigid linked-frame transform, conserve throughput, and consume a separate traversal budget. The cap terminates in black. Unlinked apertures are explicitly invalid, not opaque walls. Direct-light sampling includes uniformly sampled bounded portal chains and their discrete PDFs. High chain counts can have substantial variance.

### Acceptance boundary

Desktop builds, GPU/CPU numerical checks, bounded runtime/raster regressions, and reference captures are recorded in the report. These do not establish cross-vendor equivalence, pixel-identical raster appearance, full glTF conformance, arbitrary-scene TDR safety, or production real-time performance. The required human visual review and laptop checks remain listed as pending acceptance tasks. The next step is to review the desktop reference captures and establish longer-convergence quantitative baselines before using them to judge SSGI.
