# Portal Lighting Continuity: Known Issue and Proposed Fix

## Document status

**Status:** Milestone 1 (shared environment approximation) implemented; the
issue itself remains open until portal cameras run their own hybrid GI  
**Date:** 2026-09-12, mitigation landed 2026-09-13  
**Affected renderer:** Raster mode with SSGI enabled  
**Related design:** [Hybrid Screen-Space and Ray-Traced Global Illumination](hybrid_ray_traced_gi_design.md)

## Summary

The world visible through a portal can have a different color and brightness
from the same world after the player crosses the portal. A small difference in
screen-space indirect lighting can be expected because the visible information
changes with the camera. The broad whole-scene shift currently visible in
Mirabilis is not an acceptable SSGI limitation: the two views are being shaded
by different lighting paths.

The main camera removes the legacy constant ambient term and adds filtered
SSGI. Portal cameras do not run SSGI; they restore the legacy constant ambient
term instead. Crossing a portal changes which path shades the destination:

```text
Before crossing, looking through portal
    portal camera = raster direct sunlight + constant ambient

After crossing, looking directly at destination
    main camera = raster direct sunlight + filtered SSGI
```

This explains the observed blue/gray, saturation, and brightness discontinuity.
The correct long-term fix is to shade visible portal cameras with the same
hybrid GI system used by the main camera.

## User-visible symptoms

- A room seen through a portal changes overall tint after crossing.
- Neutral surfaces may appear blue-gray from one side and more neutral from the
  other.
- Shadowed surfaces can become noticeably brighter or darker.
- The sky can make the difference easier to perceive even though the sky image
  itself is not the cause.
- The transition occurs at the moment the destination changes from a portal
  camera view to the main camera view.
- The difference affects broad regions rather than only newly revealed edges.

## Expected behavior

For a static scene, fixed lights, matching exposure, and equivalent camera
position/orientation, the destination should retain approximately the same
linear-HDR lighting before and after crossing a portal.

Some localized differences remain reasonable:

- newly visible surfaces have no established temporal history;
- screen-space hits can change when the camera frustum changes;
- disocclusions may be noisy for several frames;
- portal recursion may use a lower quality preset;
- tone-mapped pixels can differ slightly because of filtering and resolution.

A broad scene-wide color or ambient-level change is not reasonable.

## Reproduction procedure

1. Build and run the Debug engine from `bin/Debug`.
2. Open a scene with a linked portal pair and differently colored or checker
   surfaces in the destination area.
3. Enable SSGI and use `Final lighting`.
4. Stand in front of one portal and capture the destination through it.
5. Walk through without editing the lights or materials.
6. Turn toward the same destination surfaces and capture them directly.
7. Compare broad neutral surfaces, shaded faces, and checker tiles.

The issue is confirmed when the portal image and direct main-camera image show
a broad luminance or color shift that cannot be explained by viewpoint alone.

## Verified code cause

### Main-camera shading

`shaders/mesh.frag` checks `sceneData.screenSpaceSettings.z`. When SSGI is
active for the main camera, the shader suppresses the flat ambient term:

```glsl
vec3 ambient = sceneData.screenSpaceSettings.z > 0.5
    ? vec3(0.0)
    : sceneData.ambientColor.rgb * occlusion;
```

The main camera subsequently receives filtered indirect light from
`VulkanEngine::draw_ssgi_composite()` in `src/vk_engine_renderer.cpp`.

The resulting main-camera lighting is conceptually:

```text
baseColor * rasterDirectSun + filteredSSGI
```

### Portal-camera shading

`VulkanEngine::build_portal_scene_data()` in `src/vk_engine_portals.cpp`
explicitly clears portal-camera screen-space flags:

```cpp
data.screenSpaceSettings.x = 0.0f;
data.screenSpaceSettings.z = 0.0f;
data.screenSpaceSettings.w = 0.0f;
```

This is intentional for SSAO because the main camera's depth and normal images
cannot describe the foreign portal camera. Reusing them would project nearby
occlusion from the wrong room into the portal.

`shaders/portal_view.frag` then applied the whole constant ambient term:

```glsl
vec3 ambient = sceneData.ambientColor.rgb * occlusion;
```

`VulkanEngine::build_scene_data()` sets that ambient value to `vec4(0.28)`,
and `_ssgiAmbientRetention` defaults to 0.5, so the main camera was spending
0.14 of it and the portal camera 0.28 -- with no indirect light arriving at
the portal camera to account for the difference.

The resulting portal-camera lighting was conceptually:

```text
baseColor * (rasterDirectSun + constantAmbient0.28)
```

No portal-camera SSGI pass replaces this approximation.  Milestone 1 below
now substitutes the shared environment for it; the mismatch described here is
what that substitution corrects.

### Frame ordering

The main SSGI pass and composite run before portal views are composited in
`VulkanEngine::draw()`:

```text
main forward rendering
main SSGI
main SSGI composite
portal masks and portal views
post-processing
```

Because portal content is drawn after main SSGI, it cannot receive the main
camera's SSGI composite accidentally. This protects the portal from using the
wrong depth buffer, but it also leaves the portal on the legacy ambient path.

## What is not causing the issue

The inspected implementation does not indicate that the current broad shift is
primarily caused by accidental sRGB conversion:

- offscreen portal color targets use the same draw-image format;
- the portal composite shader samples the already lit target without lighting
  it a second time;
- the main and portal sky paths use the same skybox source;
- portal scene data inherits the same sun direction, sun color, shadow map, and
  shadow settings as the main camera.

Color-space handling should still be regression-tested, but the verified
lighting-path mismatch is sufficient to explain the observed behavior.

## Why main-camera buffers cannot simply be reused

Every screen-space buffer is camera-dependent. A depth texel from the main
camera describes a different ray and often a different room from the same
texel in a portal camera. The same applies to:

- view-space normals;
- motion vectors;
- direct-lighting history;
- portal masks;
- temporal depth/normal metadata;
- SSGI hit coordinates.

Running portal SSGI with main-camera inputs would create incorrect occlusion,
false ray hits, edge leaks, and temporal ghosts. Each virtual camera needs its
own consistent input set or a truly view-independent lighting representation.

## Short-term mitigation

### Objective

Reduce the visible color jump before full portal-camera hybrid GI is available.

### Proposed behavior

Replace the portal camera's hard-coded constant ambient term with the same
explicit analytic environment convention used by ordinary SSGI misses. The
portal shader should evaluate a diffuse environment approximation from the
world-space surface normal and multiply it by base color.

Both paths must use the same:

- environment source;
- linear color space;
- environment intensity;
- black-environment override;
- indirect-intensity policy.

### Limitations

This is only an approximation. It cannot reproduce:

- screen-space color bleeding;
- local indirect shadowing;
- light from hidden emissive surfaces;
- geometry outside the portal camera;
- linked-portal indirect transport;
- the main camera's converged temporal history.

It should therefore be labeled `Portal GI approximation`, not portal SSGI or
ray-traced GI.

### Rejected quick fixes

Do not apply any of these as the final solution:

- **Reuse main-camera SSGI textures:** they represent the wrong camera.
- **Keep constant ambient in both views and add SSGI on top:** this double
  counts environment/indirect energy and changes existing validation semantics.
- **Set portal ambient to zero:** this removes the color shift in some lit
  regions but makes shaded portal interiors unnaturally black.
- **Color-grade the portal texture until it looks similar:** a fixed tint cannot
  correct spatially varying lighting and will fail in other scenes.
- **Reset exposure on crossing:** exposure is not the verified root cause.

## Correct long-term solution: portal-camera hybrid GI

Every visible portal camera should produce the same lighting decomposition as
the main camera:

```text
finalPortalLighting = portalRasterDirect + portalHybridIndirect
finalMainLighting   = mainRasterDirect   + mainHybridIndirect
```

Hybrid indirect means:

1. try SSGI using that camera's own screen-space buffers;
2. send unreliable hits, viewport exits, depth misses, and portal entries to
   complete-scene tracing;
3. merge the screen-space and traced estimates;
4. temporally accumulate and spatially filter the merged result;
5. composite it into that camera's linear-HDR image.

Complete-scene tracing is especially important for portals because a virtual
camera frequently exposes geometry and illumination not represented in the
main camera's buffers.

## Required portal-camera resources

Each actively rendered portal view needs a coherent set of resources:

| Resource | Recommended format | Purpose |
|---|---|---|
| Portal color/direct target | existing linear-HDR draw format | direct raster result and final portal composite |
| Portal reversed depth | existing prepass depth format | position reconstruction and history rejection |
| Portal view normal | `R16G16B16A16_SFLOAT` | SSGI sampling and bilateral filtering |
| Portal albedo | `R8G8B8A8_UNORM` | diffuse throughput |
| Portal velocity | `R16G16_SFLOAT` or existing velocity format | temporal reprojection |
| Portal direct lighting | `R16G16B16A16_SFLOAT` | incident radiance for screen hits |
| Nested portal mask | `R8_UNORM` | classify rays entering another portal |
| Raw hybrid indirect | `R16G16B16A16_SFLOAT` | merged noisy estimate |
| Temporal histories | two `R16G16B16A16_SFLOAT` images | accumulated indirect light |
| History metadata | existing plus source/portal fields | reject invalid history |
| Filter scratch/output | `R16G16B16A16_SFLOAT` | edge-aware denoising |

Resources should be allocated per simultaneously active virtual view, not for
every portal ever authored. A pool indexed by stable portal-view identity can
reuse allocations when visibility changes.

## Stable portal-view identity

Temporal history cannot be keyed only by array index because portal visibility
and draw order can change. Each history entry needs a stable key containing at
least:

```text
source portal ID
destination portal ID
recursion level
view side/orientation
```

History must be invalidated when:

- either portal moves or rotates;
- the link changes;
- the destination becomes newly visible after eviction;
- render extent or quality changes;
- the scene/environment/light revision changes;
- portal recursion depth changes;
- camera teleport continuity cannot be established.

## Portal-camera motion vectors

Portal motion vectors require the current and previous virtual-camera
view-projection matrices plus current and previous object transforms.

For an unchanged linked pair, derive the previous portal camera from the
previous main-camera transform through the same portal mapping. This allows
ordinary camera movement to reproject portal history.

If a portal transform/link changed, do not attempt to reuse old velocity;
invalidate that view's history.

## Rendering sequence

The recommended dependency order is:

```text
main shadow map

for visible portal views, deepest recursion first:
    portal depth/normal prepass
    portal background
    portal forward MRT/direct lighting
    portal nested-mask pass
    portal hybrid candidate generation
    portal complete-scene trace fallback
    portal merge
    portal temporal accumulation
    portal spatial filtering
    portal indirect composite
    compose already-finished deeper portal image if applicable

main camera:
    main depth/normal prepass
    main background
    main forward MRT/direct lighting
    main portal mask
    main hybrid candidate generation
    main complete-scene trace fallback
    main merge
    main temporal accumulation
    main spatial filtering
    main indirect composite
    compose finished portal images

post-process and present
```

Deepest-first rendering prevents a parent view from sampling an unfinished
child. The existing stencil recursion path may need to be converted to pooled
offscreen portal targets for GI, because each recursive camera requires
independent buffers and compute passes.

## Complete-scene tracing and portals

The ray-traced fallback must be view-independent. Both main and portal cameras
emit world-space fallback rays into the same shared trace scene.

For each ray segment:

1. find the closest opaque geometry hit;
2. find the closest linked portal aperture hit;
3. if the portal is closer, transform the ray through the link;
4. continue with remaining distance and increment the portal count;
5. otherwise shade the geometry hit;
6. sample the environment only after a genuine complete-scene miss;
7. terminate safely at the configured portal traversal limit.

This solves two different problems:

- portal-camera GI makes the image inside the aperture use equivalent lighting;
- traced portal transport lets indirect rays carry light through linked portals.

Implementing only the second does not fix the current portal-image mismatch.

## Crossing the portal

At the crossing frame, the destination virtual camera becomes the main camera.
There are two acceptable history strategies.

### Initial safe strategy

Invalidate main-camera hybrid history on teleport. The first direct frame uses
fresh noisy indirect light and converges normally. This may create a brief noise
increase but avoids incorrect ghosting.

### Later continuity strategy

Transfer the matching destination portal-view history into main history when:

- the source/destination IDs match;
- the portal transform has not changed;
- extents and quality match;
- the virtual and teleported camera matrices agree within tolerance;
- scene and lighting revisions match.

This can make crossing nearly seamless but must be proven with metadata and
capture comparisons before becoming default.

## Recursion and performance policy

Running full-resolution hybrid GI for every recursive portal view can multiply
frame cost rapidly. Quality must be budgeted by projected importance.

Recommended initial policy:

| View | Resolution | SSGI rays | Trace candidates | Temporal history |
|---|---:|---:|---:|---|
| Main camera | selected main preset | selected main preset | up to one per GI pixel | full |
| Primary visible portal | half portal-target resolution | 1-2 | checkerboard, up to one | persistent while visible |
| Recursion level 1 | quarter resolution | 1 | sparse checkerboard | optional |
| Deeper recursion | analytic environment approximation | 0 | 0 | none |

Portal quality should also scale with on-screen aperture area. A portal
covering only a few pixels does not need a large GI target. Add hysteresis so
target size does not oscillate around a threshold.

Expose measured timing per portal level and a total portal-GI budget. Never
call the maximum setting real-time without measured results.

## Proposed implementation milestones

### Portal Fix Milestone 0: capture the mismatch

- Add a fixed-camera before/after portal test fixture.
- Capture linear-HDR portal and direct destination images.
- Record mean luminance, per-channel means, MAE, and maximum error.
- Confirm ordinary raster behavior remains unchanged with SSGI disabled.

**Acceptance:** the current issue is reproducible and quantified.

### Portal Fix Milestone 1: shared environment approximation

- Replace constant portal ambient with the same explicit GI-environment
  convention used by ordinary SSGI fallback.
- Add an environment/fallback-only portal debug view.
- Capture all environment settings with test artifacts.

**Acceptance:** broad color discontinuity decreases in the test fixture without
double counting main-camera ambient. This milestone is mitigation, not final
completion.

**Implemented 2026-09-13.** The environment convention moved out of
`shaders/ssgi.comp` into `shaders/environment.glsl`, which now owns the
analytic gradient, the equirectangular mapping, the environment intensity, the
black-environment override and the sun-disk ceiling. The panorama moved from
the SSGI-only descriptor set to binding 3 of the per-camera scene set, so a
portal camera reads the same image from its forward shader;
`update_skybox_descriptors()` fills that binding for every frame's main and
portal sets, because `init_descriptors()` runs before the panorama is loaded.

`GPUSceneData::portalIndirectSettings.x` carries the main camera's SSGI flag
into each portal camera. When it is set, `portal_view_shading.glsl` applies the
same `indirectSettings.x` ambient retention the main camera applies, and adds
`environment_irradiance(normal)` -- the cosine-weighted hemisphere average of
the same environment SSGI misses fall back to -- in place of the screen-space
estimate it cannot compute. With SSGI off nothing changes: the flag is zero and
the whole flat ambient term is kept.

Still outstanding from this milestone: the environment/fallback-only portal
debug view, and captured test artifacts quantifying the remaining difference.

### Portal Fix Milestone 2: one portal-camera G-buffer

- Extend the existing offscreen portal-camera experiment with depth, normal,
  albedo, velocity, direct-lighting, and nested-mask targets.
- Add portal-camera G-buffer debug views.
- Verify projection, clipping, and orientation independently.

**Acceptance:** every buffer matches the visible portal image and contains no
main-camera data.

### Portal Fix Milestone 3: portal-camera SSGI

- Run candidate generation, temporal accumulation, bilateral filtering, and
  composite for one primary portal view.
- Maintain history under ordinary main-camera motion.
- Invalidate on portal edits and visibility loss.

**Acceptance:** the portal view shows filtered SSGI without foreign-camera
occlusion or persistent trails.

### Portal Fix Milestone 4: portal-camera traced fallback

- Feed portal-camera unresolved rays into the shared software/hardware hybrid
  backend.
- Evaluate off-screen lighting and true environment misses.
- Add trace-source and distance diagnostics.

**Acceptance:** an off-screen emitter affects the destination consistently both
through the portal and after crossing.

### Portal Fix Milestone 5: recursion and nested masks

- Render virtual cameras deepest-first into pooled targets.
- Add bounded linked-portal ray transport.
- Apply decreasing quality by recursion level/aperture area.

**Acceptance:** one and two linked pairs work; cycles terminate; no cross-view
buffer reuse or validation errors occur.

### Portal Fix Milestone 6: crossing continuity

- Begin with safe history reset on teleport.
- Optionally implement validated portal-history transfer.
- Measure the crossing frames separately from stationary convergence.

**Acceptance:** no broad color jump, stale-room ghost, persistent magenta
history rejection, or multi-frame brightness pulse.

### Portal Fix Milestone 7: performance and final acceptance

- Add portal-stage GPU timestamps and memory reporting.
- Add aperture-area quality selection and budget hysteresis.
- Run desktop and laptop validation.
- Document actual supported recursion and preset costs.

**Acceptance:** all required visual, numerical, Vulkan, lifecycle, and
performance checks pass on the explicitly tested hardware.

## Debug views and statistics

Required portal-specific views:

- portal albedo;
- portal depth and reconstructed position;
- portal normals;
- portal velocity;
- portal direct lighting;
- portal SSGI screen-hit/miss classification;
- portal trace-only indirect;
- portal merged raw indirect;
- portal temporal indirect;
- portal history rejection;
- portal filtered indirect;
- portal versus post-crossing absolute difference.

Required statistics:

- visible portal views by recursion level;
- portal target extents and memory;
- per-view screen-hit and traced-fallback percentages;
- trace queue counts and overflow;
- temporal accepted/rejected counts;
- prepass, direct, SSGI, trace, filter, composite, and total portal GPU time;
- history resets/transfers and their reasons;
- non-finite and invalid traversal counts.

## Acceptance tests

### Visual

- Neutral surfaces retain approximately the same tint before and after
  crossing.
- Shadowed regions do not switch between constant ambient and GI.
- Portal edges have no lighting halo or unrelated-room occlusion.
- Moving sideways in front of the portal does not create persistent trails.
- Newly exposed pixels converge without a broad full-image flash.
- Nested portals degrade gracefully according to the documented quality policy.

### Numerical

- Compare pre-crossing portal-view HDR against an equivalent post-crossing
  main-camera HDR capture.
- Mask pixels whose geometry legitimately changes due to near-plane or viewport
  differences.
- Report per-channel mean, luminance mean, MAE, RMSE, percentile error, and
  maximum error.
- Grade stable interior regions separately from aperture/silhouette edges.
- Establish pass thresholds only after collecting evidence from representative
  scenes.

### Regression and safety

- Ordinary SSGI mode retains its documented behavior.
- SSGI-disabled raster portals remain unchanged unless the mitigation is
  intentionally enabled there.
- Resize, minimize/restore, portal placement, relinking, scene reload, and
  renderer switching are clean under Vulkan synchronization validation.
- Resource pools do not leak or retain stale descriptor/image references.
- Portal cycles and disappearing portals remain bounded.
- Black-environment sealed scenes remain effectively black.
- Captures contain zero non-finite pixels.

## Files expected to change

| File | Responsibility |
|---|---|
| `src/vk_engine.h` | portal GI resources, histories, stable view keys, settings |
| `src/vk_engine.cpp` | revised frame ordering and teleport history policy |
| `src/vk_engine_portals.cpp` | virtual-camera targets, scene data, recursive scheduling, compositing |
| `src/vk_engine_renderer.cpp` | reusable camera-specific G-buffer and GI dispatch interfaces |
| `src/vk_engine_editor.cpp` | portal GI status, quality, timings, and debug views |
| `shaders/portal_view.frag` | remove the unmatched constant-ambient path |
| `shaders/mesh.frag` | share lighting decomposition where practical |
| `shaders/ssgi.comp` | accept camera-specific descriptors and matrices |
| `shaders/ssgi_temporal.comp` | camera/view-key-aware history metadata |
| `shaders/ssgi_bilateral.comp` | portal boundary and source-aware filtering |
| hybrid trace shaders | consume both main and portal fallback requests |
| `scripts/validate_hybrid_gi.ps1` | portal continuity fixtures and capture comparisons |

## Definition of done

The portal lighting issue is fixed only when:

- the same destination no longer exhibits a broad color or brightness change
  solely because the player crossed the portal;
- portal and main views use the same direct/indirect lighting decomposition;
- each portal camera uses its own valid screen-space buffers;
- unresolved portal-view samples trace the complete scene in hybrid mode;
- portal traversal is bounded and linked correctly;
- temporal history cannot leak between unrelated virtual cameras;
- crossing either safely resets history or transfers a proven-compatible
  history;
- required HDR comparisons meet an approved tolerance;
- visual checks show no foreign-camera AO, lighting halos, stale ghosts, or
  recursion flashes;
- GPU cost and recursion limits are measured and documented;
- Vulkan validation and the full raster/SSGI/reference/hybrid regression suite
  pass on the tested machines.

Until then, the accurate status is: **portal rendering works geometrically, but
portal GI lighting continuity is incomplete.**

