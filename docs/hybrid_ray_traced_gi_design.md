# Mirabilis Hybrid Screen-Space and Ray-Traced Global Illumination Design

## Document status

**Status:** Proposed design for review; not yet implemented  
**Date:** 2026-09-12  
**Intended reviewers:** project owner, Codex, Claude, and future renderer contributors  
**Depends on:** completed SSGI Milestones 0-9 and the completed software
path-traced reference renderer

This document defines the next global-illumination project for Mirabilis. It
does not claim that hybrid ray-traced GI already exists. The current raster
renderer has a complete screen-space GI path, and its miss path uses an
analytic environment estimate. The current software path tracer traces the
complete opaque scene, but it is a separate progressive reference renderer,
not a real-time fallback inside raster mode.

The project described here joins those systems deliberately:

1. SSGI remains the inexpensive first attempt for indirect diffuse rays.
2. Rays that cannot be answered reliably in screen space are sent to a
   complete-scene tracing backend.
3. The traced fallback evaluates off-screen geometry, emissive surfaces,
   directional sunlight, the environment, and linked portals.
4. Screen-space and traced samples are merged into one unbiasedly weighted
   noisy indirect-light buffer.
5. Existing temporal accumulation and edge-aware spatial filtering denoise the
   merged result before it is composited with raster direct lighting.

The same high-level estimator must work through two tracing backends:

- **Software BVH fallback:** required and supported on both the AMD Radeon RX
  5700 desktop and the RTX 3060 laptop. It reuses the reference tracer's
  validated scene extraction, CPU BVH builder, and compute traversal.
- **Vulkan ray-query fallback:** optional acceleration on devices exposing the
  required Vulkan extensions and features. It uses ray queries from compute
  shaders, not a ray-tracing pipeline or shader binding table.

Hardware support changes performance, not lighting semantics. Captures from
both backends should agree within a declared floating-point tolerance when
given the same deterministic ray requests.

## Plain-language objective

Screen-space GI can only find geometry represented in the current camera's
depth buffer. It loses information when a ray goes behind an object, leaves
the screen, crosses a portal, or needs a surface that the camera cannot see.
Today those cases receive an environment approximation.

Hybrid GI will keep the fast screen-space answer when it is trustworthy and
trace only the unresolved rays against the complete scene. A surface just
outside the camera can then bounce light into the visible image, and an
off-screen emissive object can affect visible surfaces through indirect light.

This is an **indirect-light system**. It does not replace the existing raster
sun/shadow pass for primary direct lighting, and it must not add the sun twice.

## Terminology

- **Primary surface:** the surface visible at a raster pixel.
- **Indirect ray:** a hemisphere ray leaving the primary surface.
- **Screen hit:** an indirect ray matched to a surface in the current raster
  depth buffer.
- **Fallback candidate:** one representative unresolved indirect ray selected
  for complete-scene tracing.
- **Trace hit:** the closest complete-scene geometry intersection returned by
  the software BVH or Vulkan ray query.
- **Secondary point:** the surface reached by the indirect ray.
- **Incident radiance:** light arriving at the primary surface from the
  indirect-ray direction.
- **One-bounce GI:** the primary surface receives light from a secondary point
  whose emission and direct illumination are evaluated. Further diffuse
  scattering is not followed in the production default.
- **Portal segment:** one straight section of a ray path between portal
  teleports.
- **Fallback backend:** software BVH or hardware ray query. It is unrelated to
  the current analytic environment fallback.

## Goals

- Preserve the completed SSGI path and use it before full-scene tracing.
- Replace the analytic approximation for unresolved samples with actual scene
  intersection whenever hybrid GI is enabled.
- Allow off-screen geometry and emissive surfaces to contribute indirect light.
- Evaluate sunlight and emissive lighting at off-screen secondary points.
- Retain environment lighting only for a genuine complete-scene ray miss.
- Run on the RX 5700 through software BVH traversal.
- Accelerate tracing with `VK_KHR_ray_query` where the active device supports
  it, without making that support a startup requirement.
- Keep raster mode available if either hybrid backend fails to initialize.
- Preserve the existing portal rule in ordinary SSGI mode and add true linked
  portal transport in hybrid mode.
- Reuse the current motion vectors, geometry metadata, temporal accumulation,
  bilateral filtering, debug views, HDR capture, and reference comparison.
- Provide enough diagnostic output to prove where every contribution came
  from.
- Bound ray count, traversal work, and portal traversal so a pathological scene
  cannot create an unbounded shader workload.
- Define measurable quality and performance acceptance gates.

## Non-goals for the first production version

- Replacing primary raster visibility with ray tracing.
- Replacing the directional shadow map for camera-visible direct sunlight.
- Multiple diffuse fallback bounces in the default real-time mode.
- Caustics, participating media, spectral rendering, or volumetric GI.
- Transparent or alpha-blended geometry in the trace scene.
- Skinned/deformed mesh BLAS updates in the first milestone set.
- Perfect equality with the progressive path tracer at low sample counts.
- A Vulkan ray-tracing pipeline, ray-generation shader, miss shader, closest-hit
  shader, or shader binding table. Ray queries are sufficient for the proposed
  hardware backend.
- Claiming 60 FPS before scene-specific measurements exist.
- Treating maximum numeric settings as automatically best-looking settings.

## Current implementation baseline

### Raster and SSGI path

The raster frame sequence is controlled by `VulkanEngine::draw()` in
`src/vk_engine.cpp`. Its relevant resources and functions are declared in
`src/vk_engine.h`, and the SSGI passes are implemented in
`src/vk_engine_renderer.cpp`.

The current SSGI contract already provides:

| Input or output | Current representation | Intended hybrid use |
|---|---|---|
| Reversed depth | sampled prepass depth | reconstruct the primary view position and validate temporal history |
| View normal | `R16G16B16A16_SFLOAT` | sample the indirect hemisphere and guide filtering |
| Albedo | forward MRT | primary diffuse throughput |
| Velocity | forward MRT | temporal reprojection |
| Direct lighting | forward MRT plus ping-pong history | incident radiance for valid screen hits |
| Portal mask | `R8_UNORM` | classify screen rays that enter a portal |
| Raw indirect | `R16G16B16A16_SFLOAT` | replaced by the merged screen/traced estimate |
| Temporal history | two `R16G16B16A16_SFLOAT` images | accumulate the merged estimate |
| History metadata | depth, octahedral normal, portal state | extended with hybrid source/confidence data |
| Filtered indirect | `R16G16B16A16_SFLOAT` | unchanged composite input |

`shaders/ssgi.comp` currently generates one to eight cosine-weighted rays per
pixel, marches each ray through the view-space depth buffer, samples previous
direct lighting for screen hits, and uses the analytic environment for all
other cases. It also identifies portal termination. This shader is the main
algorithmic integration point.

The SSGI composite adds filtered indirect radiance to the main linear-HDR draw
image. The flat raster ambient term and SSAO ambient modulation are bypassed
while SSGI is active, avoiding double counting.

### Complete-scene software tracer

`src/vk_engine_path_trace.cpp` already supplies:

- persistent CPU trace mesh data;
- world-space `TraceTriangle` and `TraceMaterial` arrays;
- a CPU-built BVH in `TraceBVHNode` form;
- GPU storage buffers for triangles, materials, nodes, texels, emitters, and
  portals;
- scene revision and input hashes;
- compute dispatch and timestamp infrastructure;
- validated CPU/GPU intersection diagnostics;
- direct, indirect, and combined linear-HDR captures.

`shaders/path_trace_intersect.glsl` contains the current iterative 64-entry
stack BVH traversal. `shaders/path_trace_transport.glsl` contains emission,
sun/environment, BRDF, and next-event-lighting logic. Portal traversal lives in
`shaders/path_trace_portal.glsl`.

These pieces must be refactored into shared trace-scene infrastructure. Hybrid
GI must not initialize a second copy of every triangle and material or maintain
a second, subtly different intersection implementation.

### Hardware reality

The validated RX 5700 system supports compute traversal but does not advertise
the Vulkan acceleration-structure, ray-query, or ray-tracing-pipeline features
required for hardware tracing. Therefore hardware-only hybrid GI would regress
the primary development desktop.

The RTX 3060 is expected to support ray queries with an appropriate driver,
but the engine must query the actual device. Product name alone is not a
capability check.

## Rendering modes and controls

The renderer selector remains:

```text
Renderer
    Raster
    Path Traced Reference (Software)
```

Hybrid GI is a raster lighting setting, not a third primary renderer:

```text
Raster Global Illumination
    Off
    SSGI
    Hybrid SSGI + Ray-Traced Fallback

Hybrid Trace Backend
    Auto
    Software BVH
    Vulkan Ray Query              [disabled with reason when unsupported]
```

`Auto` selects Vulkan ray query when initialization and a startup self-test
succeed; otherwise it selects software BVH. It must never silently fall back to
the analytic environment while still labeling the mode “Hybrid.” If tracing is
unavailable, the UI must say so and select ordinary SSGI.

Recommended controls:

| Control | Initial range | Default | Meaning |
|---|---:|---:|---|
| Hybrid enabled | boolean | off until accepted | enables complete-scene fallback |
| Backend | Auto/Software/Ray Query | Auto | selects traversal implementation |
| Fallback resolution | Full/Half/Quarter | Half | resolution of candidate generation and tracing |
| Fallback rays per pixel | 0-2 | 1 | complete-scene candidates per active GI pixel |
| Max trace distance | 1-200 m | 50 m | maximum complete-scene segment length |
| Sun shadow ray | boolean | on | evaluates sunlight visibility at the secondary point |
| Emissive next-event sample | boolean | on | samples one emissive triangle at the secondary point |
| Portal traversal limit | 0-4 | 2 | maximum teleports per fallback path |
| History weight | 0-0.99 | preset-owned | temporal response/noise tradeoff |
| Dynamic ray budget | boolean | on | reduces candidate density after a GPU-time overrun |
| Target hybrid GPU time | 0.5-20 ms | 4 ms | budget used by adaptive quality |

The quality presets should configure coherent groups instead of merely setting
every slider to its maximum:

| Preset | SSGI | Fallback | Candidate pattern | Target use |
|---|---|---|---|---|
| Performance | half, 1 ray | quarter, checkerboard | one candidate per 2x2 quad over two frames | low-end software BVH |
| Balanced | half, 2 rays | half, checkerboard | one candidate per 2 pixels each frame | default software BVH |
| High | half, 4 rays | half, every pixel | one candidate per GI pixel | fast software or ray query |
| Peak | full, 8 rays | full, every pixel | one candidate per GI pixel | screenshots/reference comparison |

“Peak” is not a frame-rate promise. The existing full-resolution eight-ray SSGI
alone has measured roughly 36 ms on the RX 5700 in one validation scene.

## Proposed frame graph

```text
directional shadow map
        |
depth/normal prepass
        |
background
        |
forward opaque MRT
    + final direct HDR
    + albedo
    + velocity
    + direct-light HDR
        |
portal mask
        |
hybrid candidate generation
    + accepted screen-space contribution
    + compact fallback-ray queue
    + per-pixel miss weight/source metadata
        |
complete-scene fallback trace
    + software BVH compute OR Vulkan ray-query compute
    + environment only on true trace miss
    + portal continuation when applicable
        |
merge screen and traced contributions
        |
temporal reprojection + rejection + neighborhood clamp
        |
depth/normal-aware spatial filter
        |
add indirect to final direct HDR
        |
portal composition
        |
FXAA / display transform / present
```

The fallback trace must finish before temporal accumulation reads the merged
raw result. Vulkan barriers must make candidate-buffer writes visible to the
indirect dispatch and trace-output writes visible to the merge dispatch.

## Hybrid estimator

### Shared ray generation

The exact same cosine-weighted direction used for the screen march must be
retained for fallback. Generating a new direction after a screen miss would
change the sampling distribution and make source comparisons misleading.

For a primary diffuse surface with albedo `a`, cosine-weighted hemisphere
sampling cancels the Lambertian `1/pi` BRDF against the cosine PDF. The current
SSGI convention therefore estimates a ray contribution as:

```text
contribution = a * incidentRadiance
```

The complete-scene fallback must use the same convention. It must not multiply
albedo twice or introduce an extra `pi`.

### Screen-space classification

Each SSGI ray ends in exactly one class:

1. `ScreenHit`: a depth intersection passed thickness and bounds tests.
2. `ViewportExit`: projection left the active rendered region.
3. `BehindCamera`: projected `w` became non-positive.
4. `DepthRangeExit`: projected depth left the Vulkan depth range.
5. `NoDepthHit`: the march consumed its step/distance budget.
6. `PortalEntry`: the ray crossed the portal mask.
7. `InvalidPrimary`: background, invalid normal, or non-finite reconstruction.

Only `ScreenHit` uses previous-frame direct lighting. Classes 2-6 are eligible
for complete-scene fallback. `InvalidPrimary` writes zero and emits no ray.

Thickness uncertainty matters. A marginal screen hit can be worse than a trace
fallback because a thick depth slab can attach the ray to the wrong surface.
The candidate stage should calculate confidence from:

- distance to the accepted thickness boundary;
- number of march steps;
- whether the hit is near a screen edge;
- hit-surface velocity;
- depth and normal discontinuity around the hit;
- portal-mask proximity.

Low-confidence screen hits may be routed to full-scene tracing. This begins as
an opt-in diagnostic and becomes default only after reference comparison shows
a lower error.

### One fallback candidate per pixel

Tracing every miss from an eight-ray SSGI pixel would be prohibitively costly,
especially in software. The initial estimator selects at most one fallback
candidate from the unresolved rays at each active GI pixel.

Use reservoir selection across the unresolved rays:

```text
screenSum = 0
unresolvedCount = 0
selectedRay = none

for each of R SSGI rays:
    march ray
    if reliable screen hit:
        screenSum += albedo * previousDirect(hit)
    else:
        unresolvedCount += 1
        replace selectedRay with probability 1 / unresolvedCount

screenContribution = screenSum / R
fallbackWeight = unresolvedCount / R
```

The trace result for `selectedRay` is multiplied by `fallbackWeight`. This
preserves the mean contribution of the sampled unresolved set while bounding
complete-scene work to one ray per GI pixel. If no ray is unresolved, no queue
entry is emitted.

For checkerboard fallback, a skipped pixel writes its screen contribution and
zero current fallback confidence; temporal reprojection supplies the traced
component. Checkerboard selection must rotate deterministically by frame and
pixel parity.

### Compact ray queue

The first implementation may use one atomic append counter. It is simpler to
validate than a prefix-sum compactor. If profiling identifies atomic contention,
replace it with workgroup-local reservation or a count/scan/scatter pipeline.

Proposed shader-compatible layout:

```cpp
struct HybridRayRequest {
    glm::vec4 originAndTMin;       // xyz world origin, w minimum t
    glm::vec4 directionAndTMax;    // xyz normalized world direction, w max t
    glm::vec4 throughputAndWeight; // rgb primary albedo, w unresolvedCount / R
    glm::uvec4 pixelAndFlags;      // x/y pixel, class, deterministic RNG state
};
static_assert(sizeof(HybridRayRequest) == 64);
```

Queue capacity is at most the active fallback pixel count. The counter buffer
also stores overflow and invalid-request counts. Overflow is never silent: the
affected pixel receives environment fallback for that frame and the debug view
marks it red.

Approximate maximum request storage:

| Active extent | Pixels | 64-byte maximum queue |
|---|---:|---:|
| 320x180 | 57,600 | 3.52 MiB |
| 640x360 | 230,400 | 14.06 MiB |
| 1280x720 | 921,600 | 56.25 MiB |

Allocate for the selected preset rather than permanently reserving the
full-resolution maximum on memory-constrained systems.

## Complete-scene fallback lighting

### Trace miss

If the complete-scene ray misses all geometry and portals, sample the same
linear-HDR environment convention used by the reference tracer. This is the
only point at which the analytic/environment miss is authoritative in hybrid
mode.

The raster skybox and reference gradient are currently different sources.
Before final acceptance, select one of these policies:

- sample the raster equirectangular skybox in both hybrid and reference paths;
  or
- use an explicitly authored GI environment independent of the visible sky.

The choice must be visible in the UI and captured in metadata. Quietly using
different environments invalidates numerical comparison.

### Trace hit

At the closest secondary hit, reconstruct:

- world position;
- oriented geometric normal;
- shading normal constrained to the geometric hemisphere;
- UV and base color;
- emission;
- stable material and triangle identifiers.

The default one-bounce incident radiance is:

```text
Li = emittedRadiance
   + directlyVisibleSunRadiance
   + oneSampleEmissiveNextEventEstimate
```

The primary-pixel fallback result is:

```text
fallbackContribution = primaryAlbedo * Li * fallbackWeight
```

Do not multiply by the secondary surface's base color merely because it was
hit. Its BRDF belongs in the direct-light evaluation at that secondary point.

### Directional sun at the secondary point

Evaluate the Lambertian or selected material response to the existing sun
radiance and direction. Fire a bounded any-hit shadow ray from the offset
secondary point toward the sun. The shadow ray uses the complete scene, not the
main-camera shadow map, because the secondary point may be outside that map or
behind camera-visible geometry.

The primary direct sun remains rasterized. Only sun reflected from the
secondary point is indirect and belongs in hybrid GI.

### Emissive surfaces

Reuse the reference tracer's emissive-triangle table. Initially sample one
emissive triangle by area, convert the area PDF to solid-angle measure, evaluate
the secondary BRDF, and trace a visibility ray. The estimator must include the
selection probability when multiple emitters exist.

An indirect ray that directly hits an emitter also receives emitted radiance.
If next-event estimation is enabled, follow the reference tracer's existing
double-count prevention policy for non-delta paths.

Future alias-table or power-weighted light selection can reduce variance, but
uniform-area selection is acceptable for the first parity milestone.

### Extra bounces

The production default is one secondary hit. Optional second-bounce mode may
be exposed only after the one-bounce path is stable. Each extra bounce greatly
increases software traversal cost and denoising difficulty.

If implemented, use bounded iterative transport and Russian roulette only
after at least two scattering events. Never allow shader recursion or an
unbounded loop.

## Shared trace-scene architecture

Create a renderer-independent owner, conceptually `TraceScene`, containing:

```text
CPU state
    triangles
    materials
    texels
    emissive triangle indices/distribution
    portals
    software BVH nodes
    scene revision/hash

GPU common buffers
    triangle SSBO
    material SSBO
    texel SSBO
    emitter SSBO
    portal SSBO
    software BVH node SSBO

Optional hardware state
    one or more BLAS objects
    TLAS instance buffer
    TLAS object
    scratch buffers
    device addresses
```

The path-traced reference and raster hybrid pass consume the same owner. Scene
extraction and material conversion occur once per scene revision.

### First-stage geometry policy

Retain the existing world-space baked triangle array for the software backend.
This supports non-uniform transforms without per-instance ray-space errors and
is already validated.

For hardware acceleration structures, prefer one BLAS per unique immutable
mesh and one TLAS instance per visible opaque draw. This avoids rebuilding all
triangles when only an object transform changes. The software backend may keep
world-space baking until profiling justifies a matching two-level BVH.

### Dirty-state categories

Track changes separately:

| Change | Software action | Hardware action | History action |
|---|---|---|---|
| camera only | none | none | use velocity reprojection |
| material value | upload material table | upload material table | reset or aggressively reject affected history |
| object transform | rebuild world BVH | TLAS update/rebuild | reject affected pixels; initially reset all |
| mesh topology | rebuild triangles/BVH | rebuild BLAS and TLAS | reset all |
| visibility | rebuild active scene | rebuild TLAS instances | reset all |
| emitter change | rebuild emitter table | no AS change unless geometry changed | reset all |
| portal transform/link | upload portal table | upload portal table | reset all |
| environment | no AS change | no AS change | reset all |

The first implementation may conservatively reset all hybrid temporal history
for any scene revision. Fine-grained invalidation is an optimization.

## Software BVH backend

The software backend is mandatory for compatibility and validation.

Refactor `path_trace_intersect.glsl` so traversal no longer depends on the
path tracer's push-constant block. Provide explicit functions such as:

```glsl
TraceHit traceClosestSoftware(vec3 origin, vec3 direction,
                              float tMin, float tMax);
bool traceAnySoftware(vec3 origin, vec3 direction,
                      float tMin, float tMax);
```

Requirements:

- iterative traversal with a fixed, checked stack;
- near-child-first ordering where practical;
- a maximum distance supplied by the request;
- explicit stack-overflow, node-index, and triangle-range fault reporting;
- closest-hit and cheaper early-out any-hit variants;
- geometric ray offset based on hit scale and orientation;
- no dependency on progressive path-tracer accumulation resources;
- deterministic results for a fixed scene and request buffer.

The existing 64-node stack remains the starting value. Capture maximum stack
use during validation before changing it.

## Vulkan ray-query backend

### Why ray query

Ray query permits complete-scene intersection inside a compute shader. That
matches the candidate/merge architecture and avoids introducing a ray-tracing
pipeline and shader binding table. The CPU still builds acceleration
structures through Vulkan.

### Required capability checks

At physical-device selection, query and record at least:

- `VK_KHR_acceleration_structure` extension;
- `VK_KHR_ray_query` extension;
- `VK_KHR_deferred_host_operations` extension;
- `VK_KHR_buffer_device_address` extension or core equivalent;
- `VK_KHR_spirv_1_4` and `VK_KHR_shader_float_controls` where required by the
  selected Vulkan version;
- `VkPhysicalDeviceAccelerationStructureFeaturesKHR::accelerationStructure`;
- `VkPhysicalDeviceRayQueryFeaturesKHR::rayQuery`;
- buffer-device-address feature support;
- acceleration-structure build sizes and relevant device limits.

Enable the feature chain only when all dependencies are supported. Function
pointers must be loaded and checked before use. A failed optional initialization
must not prevent raster or software-hybrid startup.

Because logical-device extensions are fixed at device creation, capability
selection belongs in Vulkan initialization, not in the editor button handler.

### Acceleration structures

For immutable meshes:

1. Create vertex/index buffers with acceleration-structure build-input and
   shader-device-address usage.
2. Query BLAS build sizes.
3. Build BLAS with fast-trace preference.
4. Compact BLAS when the measured memory win justifies the extra copy.
5. Create TLAS instances with stable custom indices identifying draw/material
   data.
6. Build or update the TLAS after instance changes.

Scratch buffers and source geometry must remain alive until build commands
finish. Acceleration structures and their backing buffers require explicit
destruction after GPU idle or deferred retirement.

The first implementation may rebuild TLAS after any transform change. Add
`ALLOW_UPDATE` and refit only after validation confirms correct transform and
visibility updates.

### Ray-query shader behavior

The ray-query compute shader consumes the same `HybridRayRequest` and writes
the same result format as the software backend. It should use opaque triangle
flags and commit the nearest generated triangle intersection. Instance custom
index plus primitive index resolves the shared material/triangle record.

Provide wrappers with the same signatures as the software functions:

```glsl
TraceHit traceClosestHardware(...);
bool traceAnyHardware(...);
```

Backend-specific shaders may be compiled separately. Do not require ray-query
extensions in a shader loaded on unsupported hardware.

## Portal transport

Ordinary SSGI continues to terminate portal-mask rays to its environment
fallback. Hybrid mode changes `PortalEntry` into a trace request.

Ray transport alone does not make the image inside a portal use the same
lighting as the main camera. Each visible portal camera must also render its
own G-buffer and run a camera-correct hybrid GI pass before its image is
composited. Otherwise the current constant-ambient versus SSGI color shift will
remain. Resource pooling, temporal identity, recursive ordering, crossing
history, and acceptance requirements are specified in
[Portal Lighting Continuity: Known Issue and Proposed Fix](portal_gi_lighting_continuity_issue.md).

For each complete-scene segment:

1. Find the nearest geometry hit.
2. Analytically intersect linked portal apertures.
3. If a portal is nearer than geometry, transform origin and direction through
   the linked portal and continue with the remaining distance.
4. Increment the portal count.
5. Stop in black or a clearly configured fallback after the traversal limit.

The software backend can reuse `intersectPortalScene`. The hardware backend
performs one ray query per segment and compares its committed distance with the
analytic portal distance. A portal teleport starts a new ray query; the TLAS
does not need procedural portal geometry.

The portal ray offset must move the origin to the destination's outgoing side
without skipping nearby real geometry. Unlinked portals are diagnostic errors,
not environment misses.

Portal state belongs in temporal metadata. History must reject when a pixel's
source changes between ordinary space and a portal-transported result.

## GPU resources

Proposed new resources, names illustrative:

| Resource | Format/type | Lifetime | Purpose |
|---|---|---|---|
| `_hybridRayQueue` | storage buffer of `HybridRayRequest` | extent-dependent | compact unresolved rays |
| `_hybridQueueCounters` | small storage/indirect buffer | persistent | count, overflow, invalid, optional dispatch args |
| `_hybridScreenIndirect` | `R16G16B16A16_SFLOAT` | window-sized | accepted screen contribution and unresolved weight |
| `_hybridTraceIndirect` | `R16G16B16A16_SFLOAT` | fallback extent | traced contribution, hit distance/confidence as needed |
| `_hybridMergedRaw` | `R16G16B16A16_SFLOAT` | SSGI extent | input to temporal stage |
| `_hybridSourceMetadata` | `R16G16B16A16_SFLOAT` or packed integer image | SSGI extent | source class, trace distance, portal count, confidence |
| trace-scene buffers | existing SSBO layouts | scene lifetime | software traversal and shading |
| BLAS/TLAS buffers | Vulkan AS storage | scene lifetime | optional hardware traversal |

Avoid full-resolution `RGBA32F` lighting images in the real-time path unless
measurement demonstrates an unacceptable `RGBA16F` error. Keep HDR reference
captures in 32-bit float on CPU.

### Descriptor organization

Do not append hardware-AS bindings to descriptor layouts loaded on every
device. Recommended layouts:

- set 0: existing `GPUSceneData` and global raster resources;
- set 1: SSGI G-buffer and current/history images;
- set 2: common trace-scene buffers and hybrid queue/output resources;
- set 3: hardware-only TLAS for the ray-query pipeline.

The software and hardware trace pipelines can share sets 0-2 while using
different pipeline layouts for the optional set 3.

## Synchronization and dispatch

Initial simple sequence:

1. Clear queue counters with `vkCmdFillBuffer`.
2. Barrier transfer write to compute read/write.
3. Dispatch candidate generation over the SSGI extent.
4. Barrier queue and counter shader writes to trace shader reads.
5. Read the count through an indirect dispatch argument generated on GPU, or
   dispatch queue capacity and early-out by count for the first milestone.
6. Dispatch software or hardware fallback trace.
7. Barrier trace image writes to merge reads.
8. Dispatch merge.
9. Barrier merged writes to temporal reads.
10. Run temporal and bilateral passes.

Prefer `vkCmdDispatchIndirect` once the counter-to-command path is validated;
it avoids a CPU readback and submission break. Clamp generated group counts to
the allocated queue capacity.

All barriers should use Synchronization2 and precise stage/access masks. The
existing generic same-layout image transition helper may be used initially,
but buffers need explicit `VkBufferMemoryBarrier2` coverage.

Acceleration-structure build writes must be visible to ray-query reads before
the first trace dispatch. TLAS replacement must retire the old object only
after every in-flight reader completes.

## Merge rules

For `R` SSGI samples:

```text
mergedRaw = screenSum / R
          + tracedSelectedMiss * unresolvedCount / R
```

No analytic environment term is added for an unresolved request that was
actually traced; the trace shader already returns environment on a true miss.

If hybrid tracing is disabled, preserve the completed SSGI behavior exactly:
unresolved rays use the current environment approximation. This provides an
A/B mode and prevents the hybrid project from silently changing ordinary SSGI.

If the selected backend faults or the queue overflows, use one of two explicit
policies:

- validation mode: output magenta/fault metadata and fail the bounded test;
- interactive fail-soft mode: use the analytic environment for that sample and
  show a nonzero fallback-fault counter.

Never reuse stale traced output for a newly emitted request.

## Temporal accumulation

The existing velocity reprojection, reversed-depth comparison, normal
comparison, portal comparison, and 3x3 neighborhood clamp remain the base.

Extend metadata with:

- screen versus trace source;
- trace hit versus true environment miss;
- quantized trace distance;
- portal traversal count/bit;
- fallback checkerboard validity;
- optional material or object ID when available.

History rejection rules:

- always reject invalid or off-screen reprojection;
- reject depth/normal/portal discontinuities as today;
- reject on scene revision or backend switch;
- reject when traced distance changes beyond a relative/absolute threshold;
- lower history weight when source changes between screen and trace;
- reject queue-overflow/fault samples;
- do not interpret a checkerboard skip as black current radiance.

The neighborhood clamp must operate on the merged current-frame estimate. To
avoid clamping away rare but valid emissive contributions, consider luminance
moments or a wider confidence-dependent clamp after the first correct version.

Camera cuts, resize, render-scale changes, preset changes, portal edits,
environment changes, and trace-scene rebuilds invalidate hybrid history.

## Spatial filtering

Reuse the existing separable depth/normal bilateral filter first. Add source
and trace-distance weighting only if edge tests expose leaking:

```text
weight = spatialGaussian
       * depthSimilarity
       * normalSimilarity
       * traceDistanceSimilarity
       * sourceConfidence
```

Do not blur through portal boundaries. A traced off-screen contribution may be
much brighter than neighboring SSGI hits; clamp/filter tuning must preserve
real color bleeding without spreading fireflies across silhouettes.

Future upgrades may use variance-guided atrous filtering, but that is outside
the minimum hybrid implementation.

## Materials and coordinate conventions

- All lighting calculations and history images use linear HDR values.
- Texture sampling must decode sRGB base color exactly once.
- Primary position starts in view space but must be transformed to world space
  before queue emission.
- Normals must use the inverse-transpose convention already used by scene
  extraction under non-uniform scaling.
- The queued direction is normalized world space.
- Vulkan screen projection and reversed depth retain the current SSGI
  conventions.
- Ray `tMin` must be large enough to avoid self-intersection but scale-aware
  enough not to skip nearby geometry.
- Degenerate triangles, singular transforms, and non-finite values are rejected
  and counted.
- First implementation remains opaque-only, matching the current trace scene.

## Debug views and counters

Required debug views:

1. Hybrid classification: screen hit green, trace candidate blue, portal
   candidate magenta, invalid red.
2. Queue density: grayscale requests per tile plus red overflow.
3. Traced hit/miss: green geometry, blue environment, magenta portal fault.
4. Software node visits.
5. Software triangle tests.
6. Trace distance heat map.
7. Screen-only indirect.
8. Trace-only indirect.
9. Merged raw indirect.
10. Temporal hybrid result.
11. History rejection reason.
12. Filtered hybrid result.
13. SSGI versus hybrid absolute difference.
14. Hybrid versus loaded path-traced reference difference.
15. Backend parity difference from a captured deterministic request set.

Required statistics:

- generated SSGI rays;
- reliable screen hits and percentage;
- low-confidence reroutes;
- fallback candidates and percentage;
- queue high-water mark and overflow count;
- complete-scene geometry hits, environment misses, portal traversals;
- invalid traversal count;
- average/max software node visits and triangle tests;
- candidate, trace, merge, temporal, filter, and total GPU milliseconds;
- active backend and reason;
- BLAS/TLAS build/update milliseconds and memory;
- trace-scene revision and triangle/node counts.

## Capture and quantitative comparison

Extend the bounded-run interface with names such as:

```text
MIRABILIS_HYBRID_GI=1
MIRABILIS_HYBRID_BACKEND=auto|software|ray_query
MIRABILIS_HYBRID_PRESET=0..3
MIRABILIS_HYBRID_CAPTURE=<prefix>
MIRABILIS_HYBRID_REFERENCE=<indirect.pfm>
MIRABILIS_HYBRID_CAPTURE_REQUESTS=<file>
MIRABILIS_HYBRID_REPLAY_REQUESTS=<file>
```

A capture should include:

- screen indirect PFM;
- traced indirect PFM;
- merged raw PFM;
- temporal and filtered indirect PFM;
- final combined PFM;
- display previews;
- classification/source image;
- settings, device, driver, source hash, scene hash, backend, timings, counters,
  and non-finite totals.

For backend parity, capture the compact request buffer once, replay that exact
buffer through software and ray-query backends, and compare hit identity,
distance, barycentrics, and radiance. This removes SSGI RNG and temporal history
from the backend comparison.

Reference comparison must use the same camera, scene, materials, sun,
environment, portal limit, exposure-independent HDR units, and a converged
indirect-only reference. Report at least MAE, RMSE, relative MAE with a guarded
denominator, maximum error, and percentile error. Also report metrics for
screen-hit and fallback regions separately.

## Performance policy

Performance targets are budgets to test, not claims:

| Device/path | Initial target at 1280x720 | Degradation response |
|---|---:|---|
| RX 5700 software, Balanced | hybrid trace <= 6 ms | checkerboard then quarter resolution |
| RTX 3060 ray query, Balanced | hybrid trace <= 3 ms | checkerboard before resolution reduction |
| Total Balanced GI | <= 8 ms | reduce SSGI rays/steps and fallback density coherently |
| Peak | uncapped for inspection | report actual time; never call it real-time automatically |

The existing reference dispatch has measured from a few milliseconds to over
30 ms depending on scene. A real-time software fallback therefore requires
strictly fewer rays, shallower transport, and temporal reuse.

Adaptive quality should use a moving average with hysteresis. It must not
change resolution every frame. Suggested order when over budget:

1. enable/strengthen checkerboard candidate selection;
2. reduce fallback resolution;
3. reduce SSGI ray count;
4. reduce SSGI step count;
5. disable emissive next-event sampling only as a clearly visible last resort.

Never shorten the ray distance so aggressively that the feature quietly stops
covering off-screen geometry while still reporting high quality.

## Failure behavior

- Missing ray-query support: Auto uses software; forced ray-query is disabled
  with a reason.
- Software pipeline missing: hybrid is disabled; ordinary SSGI remains active.
- Acceleration-structure build failure: destroy partial hardware resources and
  use software if available.
- Empty scene: trace requests miss to the configured environment.
- Empty emitter table: skip emissive next-event sampling without invalid access.
- Queue overflow: count, visualize, fail validation, and use explicit fail-soft
  environment interactively.
- Traversal stack overflow or invalid index: zero the unsafe result, mark fault,
  and fail validation.
- Device loss or allocation failure: retain the engine's existing fatal-device
  policy unless a broader recovery project changes it.
- Resize/minimize: do not dispatch zero extents; rebuild extent-dependent
  hybrid resources after the relevant frame fences complete.

## Milestone plan

### Milestone 0: freeze and measure the completed baseline

Work:

- retain current ordinary SSGI and reference captures;
- add no hybrid lighting yet;
- record representative miss ratios and current SSGI timings;
- select fixed cameras for open, enclosed, emissive, portal, and off-screen
  occluder cases.

Acceptance:

- existing 18-case regression passes unchanged;
- ordinary SSGI captures are byte-stable where already expected;
- baseline artifacts record device, driver, resolution, preset, and hashes.

### Milestone 1: shared trace-scene ownership

Work:

- extract trace-scene buffers and revision tracking from path-trace-only state;
- allow the raster renderer to update the shared scene without activating the
  progressive path tracer;
- refactor shader intersection helpers away from path-tracer push constants;
- retain reference-renderer behavior.

Visible result:

- none required; diagnostic counters show identical triangle/BVH counts in
  raster and reference modes.

Acceptance:

- reference validation remains 18/18;
- CPU/GPU BVH diagnostics remain exact within the established tolerance;
- no duplicate scene buffers are leaked when switching modes.

### Milestone 2: miss classification and compact queue

Work:

- split SSGI screen contribution from its analytic fallback;
- classify every screen ray;
- reservoir-select one unresolved candidate per GI pixel;
- emit and count compact requests;
- add queue-density and classification views.

Visible result:

- screen hits are green; off-screen/depth misses blue; portal candidates
  magenta; no traced light is merged yet.

Acceptance:

- queue count never exceeds capacity;
- fixed seed produces deterministic classification;
- ordinary SSGI mode remains visually and numerically unchanged.

### Milestone 3: software closest-hit fallback

Work:

- consume requests with the software BVH;
- return closest hit ID, distance, barycentrics, and fault state;
- add any-hit shadow traversal;
- initially display geometric diagnostic colors only.

Visible result:

- trace hit/miss and trace-distance views reveal off-screen geometry.

Acceptance:

- replayed requests match CPU brute force;
- invalid, overflow, and stack counters are zero in fixtures;
- bounded runs complete without Vulkan validation errors.

### Milestone 4: one-bounce traced lighting

Work:

- evaluate emission at secondary hits;
- evaluate secondary-point sunlight with a complete-scene shadow ray;
- add one emissive next-event sample;
- sample environment only on true complete-scene misses;
- write trace-only HDR output.

Visible result:

- trace-only debug view shows off-screen illumination and color contribution.

Acceptance:

- sealed black environment/emitter-free scene is effectively black;
- off-screen emitter changes visible trace-only radiance;
- occluding that emitter or sun path reduces the expected contribution;
- no direct-sun double counting at the primary surface.

### Milestone 5: estimator merge and composite

Work:

- apply unresolved-count weighting;
- merge screen and trace components;
- feed the merged raw result into the existing temporal/filter pipeline;
- add hybrid mode and backend controls.

Visible result:

- final lighting retains SSGI detail while off-screen contributions remain as
  the camera turns away from their source.

Acceptance:

- `merged = screen + trace` has zero or float-rounding-only residual;
- disabling hybrid exactly restores ordinary SSGI behavior;
- traced fallback disappears only when its physical source/visibility changes,
  not merely when it leaves the camera view.

### Milestone 6: hybrid temporal metadata and denoising

Work:

- store source class, trace distance, checkerboard validity, and portal state;
- tune source-aware history rejection;
- prevent skipped checkerboard pixels from becoming black samples;
- tune bilateral filtering on traced edges and bright emitters.

Visible result:

- stationary fallback noise converges; motion reveals bounded noise without
  long trails, magenta flashes, or cross-silhouette bleeding.

Acceptance:

- camera motion rejects stale disocclusions;
- stationary history is accepted;
- emissive light removal does not leave persistent light ghosts;
- no non-finite values in long captures.

### Milestone 7: linked portal fallback transport

Work:

- convert portal candidates into world-space trace requests;
- compare portal and geometry distances per segment;
- teleport and continue up to the limit;
- extend metadata/debugging.

Visible result:

- off-screen light through a linked portal contributes to the main view.

Acceptance:

- one and two linked pairs match reference direction/origin diagnostics;
- unlinked portals are explicit faults;
- cycles stop at the configured limit;
- disabling portal transport restores the documented ordinary SSGI behavior.

### Milestone 8: Vulkan capability plumbing and ray-query backend

Work:

- add optional extension/feature discovery before device creation;
- add BLAS/TLAS creation, update, destruction, and synchronization;
- compile a ray-query-only compute shader;
- implement common request/result semantics;
- expose backend status.

Visible result:

- supported hardware can switch between software and ray-query output.

Acceptance:

- forced unsupported mode is safely disabled with a reason;
- deterministic replay agrees with software on hit identity/distance and within
  the declared radiance tolerance;
- device/AS lifetime passes Vulkan validation through reload and shutdown.

### Milestone 9: dynamic scene updates

Work:

- distinguish material, transform, topology, visibility, emitter, and portal
  dirtiness;
- rebuild/refit only the required structures;
- safely retire in-flight resources;
- invalidate temporal history correctly.

Visible result:

- moving an object or emitter updates off-screen indirect lighting without
  restarting the engine.

Acceptance:

- no one-frame use-after-free/stale-AS flashes;
- moved geometry changes both software and hardware results consistently;
- rebuild/update timing and memory are reported.

### Milestone 10: performance and adaptive presets

Work:

- add timestamps for every hybrid stage;
- implement checkerboard and fallback-resolution presets;
- add hysteretic dynamic budget control;
- optimize queue compaction only if measurement shows it matters.

Visible result:

- Balanced mode remains responsive while Peak remains available for inspection.

Acceptance:

- report labeled results on RX 5700 and RTX 3060;
- no dispatch approaches Windows timeout durations in test scenes;
- preset transitions invalidate/reconstruct history without stale frames.

### Milestone 11: reference grading

Work:

- capture long-converged indirect references;
- add screen-region and fallback-region metrics;
- create off-screen emitter, behind-camera bounce, portal, sealed-black, and
  rapid-disocclusion fixtures;
- set error budgets from observed evidence.

Acceptance:

- hybrid improves fallback-region error relative to ordinary SSGI in every
  targeted fixture;
- no regression beyond the agreed budget in reliable screen-hit regions;
- all captures include reproducibility metadata.

### Milestone 12: production acceptance and documentation

Work:

- complete human visual checks;
- document known limitations and recommended presets;
- update build/run scripts and the main renderer documentation;
- retain ordinary SSGI and software-reference escape hatches.

Acceptance:

- clean Debug build and SPIR-V validation;
- Vulkan synchronization validation is clean;
- full raster/reference/hybrid regression passes;
- desktop and laptop results are reviewed;
- no claim exceeds the tested devices, scenes, or settings.

## Required test scenes

1. **Sealed black box:** no environment and no emitter; result must be black.
2. **Off-screen emissive wall:** emitter outside the camera but visible to a
   secondary surface; hybrid must retain its contribution when camera framing
   excludes it.
3. **Behind-camera bounce:** brightly lit colored wall behind the camera
   reflects into a visible neutral receiver.
4. **Occluded emitter:** same as case 2 with a blocker inserted; verifies shadow
   rays.
5. **Sunlit hidden wall:** secondary hit is outside the directional shadow-map
   coverage; complete-scene sun visibility must remain correct.
6. **Thin occluder:** tests ray offset, any-hit stability, and light leaks.
7. **Large non-uniform transform:** validates shared geometry conventions.
8. **Rapid camera turn:** forces SSGI hit/trace-source transitions and temporal
   rejection.
9. **One linked portal:** light reaches the receiver only through the portal.
10. **Portal cycle:** confirms traversal cap and fault-free termination.
11. **Many emitters:** exercises selection PDF and variance.
12. **Empty scene/environment:** all requests safely miss to environment.

## Code touchpoints

Expected files to modify or add:

| File | Planned responsibility |
|---|---|
| `src/vk_engine.h` | hybrid mode/settings/resources, backend capability state |
| `src/vk_types.h` | queue/result structs and shared trace-scene ABI |
| `src/vk_engine.cpp` | frame-graph ordering, mode fallback, environment controls |
| `src/vk_engine_renderer.cpp` | resource creation, descriptors, candidate/trace/merge dispatches, timestamps |
| `src/vk_engine_path_trace.cpp` | refactor shared trace-scene ownership without changing reference output |
| `src/vk_engine_editor.cpp` | hybrid controls, status, counters, debug views |
| `src/vk_engine_scene.cpp` | trace-scene dirty tracking and update timing |
| `src/vk_engine_resources.cpp` | optional AS-compatible buffer usage as needed |
| `shaders/ssgi.comp` | classification, reservoir candidate selection, screen-only result |
| `shaders/hybrid_trace_software.comp` | software queue consumption and lighting |
| `shaders/hybrid_trace_ray_query.comp` | optional ray-query queue consumption |
| `shaders/hybrid_merge.comp` | weighted screen/trace merge |
| `shaders/path_trace_intersect.glsl` | backend-independent software traversal helpers |
| `shaders/path_trace_transport.glsl` | shared secondary-point light evaluation helpers |
| `shaders/path_trace_portal.glsl` | shared bounded portal segment logic |
| `shaders/ssgi_temporal.comp` | source/distance-aware temporal validation |
| `shaders/ssgi_bilateral.comp` | optional trace-distance/source filtering |
| `shaders/render_debug.frag` | hybrid diagnostic views |
| `CMakeLists.txt` | new compute shaders and optional ray-query shader target |
| `scripts/validate_hybrid_gi.ps1` | bounded regression, capture, parity, and preservation checks |

Names may change during implementation, but responsibilities must not be
silently collapsed in ways that hide backend or estimator behavior.

## Review decisions that must be explicit

The reviewers should approve or change these before implementation:

1. Is one complete-scene fallback candidate per GI pixel the correct initial
   cost ceiling?
2. Should low-confidence screen hits be traced, or only definite misses?
3. Should the hybrid/reference environment use the visible raster skybox or an
   independent authored GI environment?
4. Is one secondary bounce sufficient for the first production target?
5. Is the software backend a shipping fallback or only a compatibility/debug
   mode after ray query exists?
6. Should portal-limit termination be black, environment-lit, or configurable?
7. What measured frame-time target defines “real-time” on each target machine?
8. What fallback-region error budget against the path-traced reference is
   acceptable?
9. Are dynamic/skinned meshes required for the first accepted release?
10. Should emissive next-event sampling be mandatory in every quality preset?

## Risks and mitigations

| Risk | Consequence | Mitigation |
|---|---|---|
| Software misses dominate the screen | excessive GPU time | compact one candidate/pixel, checkerboard, half/quarter resolution, adaptive budget |
| SSGI false hits prevent tracing | missing/off-color indirect light | confidence metric and reference-driven reroute threshold |
| Source switches create temporal ghosts | trails during camera motion | source/distance metadata, lower weight or reject on transition |
| Bright emissive sample creates firefly | unstable hot pixels | correct PDFs, robust bounds, temporal moments/clamp after correctness |
| World BVH rebuild stalls edits | poor editor responsiveness | categorized dirtiness; later two-level software BVH |
| Hardware and software disagree | untrustworthy backend | deterministic request replay and hit-level comparison |
| Portal ray loops | hang or extreme cost | strict segment/traversal limit and fault counters |
| Ray origin bias leaks or detaches | missing contact illumination | scale-aware offset and thin-geometry fixtures |
| Different sky/environment conventions | invalid reference metrics | single explicit environment policy and captured metadata |
| “Hybrid” silently uses analytic fallback | misleading feature claims | backend/status UI, trace counters, validation failures |
| Maximum preset exceeds usable frame time | perceived engine failure | label Peak as inspection mode and display measured GPU cost |

## Definition of done

Hybrid ray-traced GI is complete only when all of the following are true:

- Raster Hybrid mode visibly and measurably traces unresolved SSGI rays against
  complete-scene geometry.
- An off-screen emitter or sunlit off-screen surface affects visible indirect
  lighting even when absent from the camera buffers.
- A true complete-scene miss samples the configured environment.
- Ordinary SSGI remains available and retains its documented analytic fallback.
- The software backend passes on the RX 5700.
- The ray-query backend either passes on supported hardware or remains clearly
  marked as pending; software success cannot be presented as hardware success.
- Backend replay comparisons meet an approved tolerance.
- Linked portal fallback transport is bounded and validated.
- Temporal accumulation and filtering do not create persistent motion trails,
  portal leakage, or cross-silhouette bleeding in the required fixtures.
- Sealed-dark and non-finite tests pass.
- GPU timings, queue usage, traversal faults, backend, and scene revision are
  observable.
- Captured hybrid indirect improves the approved fallback-region metrics over
  ordinary SSGI against the path-traced reference.
- Build, SPIR-V, synchronization-validation, resize, minimize/restore, scene
  edit, and mode-switch regressions pass.
- Documentation states tested devices, settings, limitations, and actual
  performance without implying broader coverage.

Until those conditions are satisfied, the correct project status is
“hybrid ray-traced GI in progress,” not “GI finished.”

## Recommended implementation order

Implement Milestones 0-7 with the software backend first. That proves the
hybrid estimator, lighting split, temporal behavior, and portals on the primary
RX 5700 without mixing algorithm bugs with Vulkan acceleration-structure
bring-up. Then implement the ray-query backend against captured deterministic
requests. This ordering treats hardware RT as an acceleration of a known
answer, not as a second independent lighting design.
