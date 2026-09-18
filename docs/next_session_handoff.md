# Next session handoff

**Date:** 2026-09-17
**Read with:** [lumen_lite_design.md](lumen_lite_design.md). That document holds
the architecture, the checkpoint measurements and the *Settled, do not
re-investigate* list. This one holds where things stand right now, what moved
last session, and what to do first. Where something here is measured it says
so; where it is inference it says that too.

## Do this first

**The interiors are ~3.8× too bright.** Nothing below it is worth judging until
that is closed, because the error swamps every other difference. The design
doc's *Start here* has the measurements.

Last session narrowed the mechanism from "leading suspect" to something
concrete and checkable:

- `mesh_shading.glsl:49` scales the ambient/IBL term by
  `ambient_occlusion(gl_FragCoord.xy)`.
- `input_structures.glsl:37` returns **1.0** from that function when SSAO is
  off (`screenSpaceSettings.x < 0.5`).
- `assets/scenes/living_room_showcase.json` has `"ssao": {"enabled": false}`.

So in the living room every surface receives **full sky irradiance with no
occlusion at all**, whether or not it can see sky. That is a mechanism, traced
through the code — what has *not* been measured is whether it accounts for the
full 3.8× or only part of it. Measure before assuming it is the whole story.

The fix the design doc proposes is already half-built: `sky_visibility()` in
`surface_cache_direct.comp:107` computes how much sky a point can see by
marching the scene field. The cache already uses it. The raster ambient term
does not.

Expect `ambientRetention` (`vk_engine.h:1270`, default 0.5) to fall toward zero
once the bounce chain carries the light that fudge stands in for.

**Before quoting 3.8× as exact:** the software path tracer lights misses from
the analytic gradient, not the HDR panorama. `traceEnvironmentMap` exists to
match them. A gap this size is not explained by that, but re-run matched before
publishing the number.

## State of the tree

Clean, builds, **12 commits unpushed** on `main`. Debug build at
`bin/Debug/engine.exe`.

| Commit | What |
|---|---|
| `c948097` | Restored the surface cache measurement passes |
| `16c5c73` | "refactor" — the over-engineering pass, see below |
| `c3687d5` | The living room investigation writeup |

`16c5c73` also swept in a `living_room_showcase.json` shadow-bias tweak
(`shadowDepthBias` 0 → 0.0012, `shadowNormalBias` 0.174 → 0.15, ssao bias
0.05 → 0.075) that was uncommitted at the time. Not where you would look for
it later.

## What moved last session

A complexity pass over the GI code removed ~510 lines, then ~300 came back.
Net about −210.

**Removed and staying removed:**

- Dead descriptor bindings in `ssgi_body.glsl` (4, 9–13) and its unused
  octahedron pair — declared, never read or called.
- `_ssgi.fallbackImage` — a full-res RGBA16F written every frame, sampled by
  nothing but the debug view that displayed it.
- `_ssgi.referenceImage` and its PFM loader. **The reference comparison is not
  lost:** `capture_ssgi` in `vk_engine_path_trace.cpp:843` independently loads
  the same PFM and reports absolute error, RMS, relative magnitude, max error
  and a difference image. That is the better tool. Only the on-screen
  false-colour version went.
- Two debug views: "SSGI fallback only" and "SSGI vs loaded reference".
- Twelve same-layout `transition_image` calls in `draw_ssgi`, replaced by four
  global memory barriers with identical stage/access masks.
- Duplicated `live_uv` and octahedron code across four shaders, now in
  `shaders/ssgi_common.glsl`.
- The five-case preset switch, now a table in `vk_engine_ssgi.cpp`.

**Restored after review** (`c948097`) — deleting these was a mistake, they are
the ground truth the brightness hunt needs:

- `measure_surface_cache_coverage` (`MIRABILIS_SURFACE_CACHE_COVERAGE=1`)
- `measure_surface_cache_shadows` (`MIRABILIS_SURFACE_CACHE_SHADOW_CHECK=1`)

Nothing else measures what they measure. `check_surface_cache_lighting.py`
reports coverage over *drawn screen pixels*; the C++ one reports it over each
mesh's *opaque surface area*. The shadow check casts exact rays through the
CPU BVH because a shadow map's depth bias leaks light past edges the field
correctly blocks.

### One behaviour change

The composite's half-res upsample hardcoded depth falloff 800 and normal power
32, ignoring the `filterDepthFalloff` / `filterNormalPower` sliders that the
bilateral filter honours. It now reads them from push constants. Presets 0, 2
and 4 *are* 800/32 so nothing moves; **preset 1 (700/28) and preset 3 (900/36)
now upsample differently than before.** That is the fix, not a regression, but
know it if a half-res preset looks changed.

### Trap: debug views renumbered

Removing those two views shifted everything above them. **Any
`MIRABILIS_RENDER_DEBUG_VIEW=<n>` for n ≥ 21 in old notes now points somewhere
else.**

| n | View | | n | View |
|---:|---|---|---:|---|
| 8 | Albedo (linear) | | 19 | Material roughness |
| 12 | SSGI raw | | 20 | Material metallic |
| 13 | SSGI hit/miss | | 21 | Direct diffuse only |
| 14 | SSGI steps | | 22 | Direct specular only |
| 15 | SSGI temporal | | 23 | Emission |
| 16 | SSGI history rejection | | 24 | Shading normal |
| 17 | SSGI reprojection | | 25–27 | Geometric normal, tangent, handedness |
| 18 | SSGI filtered | | 28 | SDF sphere trace |
| | | | **29** | **Surface cache atlas** |

`scripts/check_sdf_agreement.py` (30→28), `check_surface_cache_lighting.py`
(31→29) and `lumen_lite_design.md:116` are updated. Surface cache atlas *pages*
(`MIRABILIS_SURFACE_CACHE_PAGE`, 0–12) are unchanged.

## Verified working

Run from `bin/Debug/`. All of these were run last session and produced the
output shown.

```sh
# Coverage: 92.3% of 148.3 m^2 (doc records 91.8% of 148)
MIRABILIS_TEST_FRAMES=8 MIRABILIS_TEST_SCENE=living_room_showcase.json \
  MIRABILIS_LUMEN_LITE=1 MIRABILIS_TEST_DISABLE_TRACE=1 \
  MIRABILIS_SURFACE_CACHE_COVERAGE=1 ./engine.exe

# Shadow referee: exact rays lit 30.3%, cache 27.5%, agree 96.8%
#   (doc records 30.3% / 27.1% / 96.6%; cache is closer now the sky lights it)
MIRABILIS_TEST_FRAMES=8 MIRABILIS_TEST_SCENE=living_room_showcase.json \
  MIRABILIS_LUMEN_LITE=1 MIRABILIS_SURFACE_CACHE_SHADOW_CHECK=1 ./engine.exe

# SSGI capture (writes <path>.indirect.pfm and <path>.txt with settings+camera)
MIRABILIS_TEST_FRAMES=32 MIRABILIS_TEST_SCENE=living_room_showcase.json \
  MIRABILIS_LUMEN_LITE=1 MIRABILIS_TEST_DISABLE_TRACE=1 \
  MIRABILIS_SSGI_CAPTURE=out.pfm ./engine.exe
```

Cameras worth using (scene defaults are not — press **F9** in the running
engine to print the current one):

| View | `MIRABILIS_TEST_CAMERA` |
|---|---|
| Living room, interior | `'0 1.6 -3 0 3.14'` |
| Living room, window corner | `'2.217 1.587 -0.677 0.216 -4.115'` |
| Sponza arcade | `'0 10 0 -0.5 0'` |
| Cornell box | `'0 2 3.5 0 0'` |

## Two gotchas found last session

**SSGI capture nondeterminism — fixed, see #12.** Two GPU races, neither to do
with frame timing (a 1-frame capture differed too). The scene SDF merge ran
every instance's load-min-store with no barrier between dispatches, so where
regions overlap a minimum was lost at random. Radiosity read the indirect page
it was writing. Captures are now bit-identical run to run in the living room
and the Cornell box, so **a capture diff validates a change again**. Radiosity
now reads last update's page, which converges slower, not elsewhere: Cornell
indirect mean is 5.7% lower than before at 32 frames, 1.0% at 256.

**Validation warnings — fixed, see `f37b2d1`.** Ten of them in the living room,
recorded here as the portal view pass's doing. That attribution was wrong:
`portal_view_shading.glsl` declares one output, and `draw_portal_views` returns
early in a scene with no portals. The source was the main camera's *transparent*
pass, which restarts at `colorAttachmentCount = 1` while `mesh.frag` still
declares four outputs. `mesh_transparent.frag` now compiles the same body with
`MIRABILIS_COLOR_ONLY`. Living room, portal pair and Cornell box are all at 0.

Worth remembering as a method note: the attribution was written from reading the
code and looked obviously right. It took a second pass over the actual pipeline
construction to find that the blamed pass never runs in that scene.

## After the brightness error

Straight from the design doc's *Next*, unchanged:

1. Re-grade every scene against a reference. The checkpoint 7 Cornell numbers
   predate both the sky entering the cache and this error being known.
2. Anti-aliasing, if the moulding stipple matters. Raster-path job; the
   velocity buffer and reprojection already exist.
3. Then the architecture, in order: screen probes, HZB screen tracing, field
   clipmaps, world radiance cache, reflections. None started — no `clipmap`,
   `probe` or `hzb` anywhere in `src/` or `shaders/`. Each needs a written
   acceptance measurement decided **before** it starts, as checkpoints 1–7 had.
4. Depth-peeled card layers for concave meshes (Sponza ~89%), when missing
   coverage visibly limits results.
5. VRAM: Sponza's atlas is ~1 GB on a 6 GB card. Measure before optimising.

Rough completion against that full list: **about half**, with the five
architectural stages making up most of what remains. Treat that as soft — the
remaining items are each roughly a checkpoint's worth of work, and estimating
unstarted architecture is the least reliable estimate there is. What the number
undersells is that the load-bearing risks are retired: shell-baked fields trace
cleanly, six box cards cover a real scene, and cache-feeds-cache radiosity
converges instead of exploding. All measured.

## The method that works here

From the design doc, repeated because last session proved it again: **measure
first, guess never.** Compare against a reference, not against the previous
build. Record the camera beside every number. PFM rows run bottom to top — flip
before indexing regions, and confirm each region against the albedo view
(`MIRABILIS_RENDER_DEBUG_VIEW=8`) before trusting a number.
