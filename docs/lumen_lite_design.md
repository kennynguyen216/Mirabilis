# Mirabilis Lumen-lite Global Illumination Design

## Document status

**Status:** Checkpoints 1–7 implemented and measured against exact
references: Lumen-lite diffuse GI runs end to end.  **Interiors currently
render about 3.8× brighter than a path-traced reference** — see *Start here*
below before building anything new.
**Date:** 2026-09-17
**Supersedes:** the 3D fallback tier of
[hybrid_ray_traced_gi_design.md](hybrid_ray_traced_gi_design.md), which
traced triangles through a BVH or ray queries.  This project follows Unreal's
Lumen instead: screen tracing first, then mesh distance fields, with surface
colour and lighting read from a surface cache.

## Start here

Read this section before changing anything. It records what the last session
established, what it ruled out, and what to work on next. Everything in it is
measured; where something is a guess it says so.

### The next job: interiors are ~3.8× too bright

Measured in the living room, at the camera below, against a 400-sample path
trace of the same view (interior pixels only, windows excluded):

| Render | Interior vs reference |
|---|---:|
| Peak preset (8 rays, full res) | 3.84× |
| Balanced preset (2 rays, half res) | 3.71× |
| SSGI off entirely | floor region 6.4× |

This is a scale error, not a sampling error. It is present **with GI switched
off**, so it does not originate in the gather, the cache or the field. The
leading suspect is the forward pass's IBL ambient being applied without any
occlusion term: every surface receives full sky irradiance whether or not it
can see sky, and the living room scene has SSAO disabled. `ambientRetention`
(default 0.5) is a slider that exists to fudge exactly this.

The cache's new sky pass already computes the missing quantity per texel — how
much sky a point can see, by a march through the scene field. The same idea
applied to the raster ambient term is the obvious fix to try first.

**Before trusting 3.8× precisely:** the software path tracer lights its misses
from the analytic gradient rather than the HDR panorama, so the two are not in
environment parity. `traceEnvironmentMap` exists to match them. A gap this
large is not explained by that, but the comparison should be re-run matched
before the number is quoted as exact.

### Settled, do not re-investigate

- **The dark ceiling in the living room is correct.** It measures 1.08× a
  400-sample path trace. The room is genuinely dark: the floor is wood with
  albedo 0.27/0.15/0.09, reflecting about 17%, and the ceiling is lit almost
  entirely by bounce from it. A whole session was spent hunting a bug here
  that does not exist.
- **The dotted lines along mouldings and window bars are geometric aliasing,
  not GI.** They appear identically with SSGI disabled (4606 edge pixels off,
  4844 on) and full resolution does not remove them. Those lips are about a
  pixel wide, so FXAA cannot recover them. Fixing them means temporal
  anti-aliasing or supersampling, in the raster path, not here. Toggling GI
  changes the brightness around them, which is why they look GI-related.
- **Ray count is not the current bottleneck.** Peak does 16× Balanced's work
  (8 rays at full resolution against 2 at half) for 3.84× vs 3.71× against the
  reference and 7.0% vs 7.3% speckle. Until the brightness error is resolved,
  spending rays — including building screen probes — is premature.

### Tried and reverted; the measurement is in the commit history

Each of these was implemented, measured and reverted. Do not retry without a
new reason:

- Widening the temporal history clamp: no measurable change.
- Firefly suppression by `1/(1 + luminance)`: no change. The raw samples max
  out at 0.383 with a median of 0.0023 — there are no bright outliers to
  suppress. The speckle is large *relative* variation in a very dim signal.
- The same weighting made relative to each pixel's mean: removed 43% of the
  indirect light, left speckle identical, made outliers worse.
- Reading full cache radiance on screen hits instead of the screen's direct
  buffer plus cache indirect: lost light (indirect 0.0110 → 0.0085). The
  cache's card-resolution direct light is dimmer than the screen's
  shadow-mapped direct buffer.
- Averaging the bilateral upsample's fallback neighbourhood rather than taking
  the nearest sample: no change; that branch almost never fires.
- Dropping the SSGI start offset from 8 cm to 5 mm: indirect +3.6%, speckle
  unchanged. Arguably more correct, not a fix for anything.

### How to measure anything here

Guessing from the code failed repeatedly last session; measuring first
succeeded every time. The tools:

- **Press F9 in the running engine** to print the current camera as a
  `MIRABILIS_TEST_CAMERA` string. An artifact visible from one viewpoint
  cannot be measured from a guessed one.
- Cameras that are actually useful (scene defaults are not — Sponza's faces a
  wall):

  | View | Camera |
  |---|---|
  | Living room, interior | `'0 1.6 -3 0 3.14'` |
  | Living room, window corner | `'2.217 1.587 -0.677 0.216 -4.115'` |
  | Sponza arcade | `'0 10 0 -0.5 0'` |
  | Cornell box | `'0 2 3.5 0 0'` |

- A headless capture: `MIRABILIS_TEST_SCENE`, `MIRABILIS_TEST_FRAMES`,
  `MIRABILIS_TEST_CAMERA`, `MIRABILIS_RASTER_CAPTURE` (which appends `.pfm`).
  `MIRABILIS_TEST_TRACE=1` with `MIRABILIS_CAPTURE` gives the path-traced
  reference; 400 frames accumulate 400 samples.
- **PFM rows run bottom to top.** Flip before indexing regions, and confirm
  each region against the albedo debug view (`MIRABILIS_RENDER_DEBUG_VIEW=8`)
  before trusting a number. An unflipped array produced a confident, entirely
  wrong account of which surfaces had gained light.
- Debug views that answered real questions: 8 albedo (is it dark paint or
  missing light), 12 SSGI raw (is it noise or a denoiser problem), 18 filtered,
  29 surface cache atlas with `MIRABILIS_SURFACE_CACHE_PAGE`.
- Compare against a reference, not against the previous build. The reference
  settled in one run what five hypotheses could not.

## Why a distance field and a surface cache

A distance field answers "how far is the nearest surface" everywhere, so a
ray can leap through empty space safely, it runs as plain compute on both
the RTX 3060 and the RX 5700, and it needs no ray-tracing extensions.  What a
distance field cannot answer is what the surface it hit looks like or how
brightly it is lit.  The surface cache answers that: every mesh is captured
from a few directions into "cards" packed into an atlas, lighting is computed
per card texel, and a trace hit is resolved to the card that saw that point.
Lighting the cache from the cache itself, over successive frames, is what
gives multiple bounces without recursive tracing.

## Pipeline

```text
screen trace (existing SSGI march)
    miss or unreliable
        -> sphere trace the scene distance field
            hit  -> surface cache radiance at the hit
            miss -> environment
    -> existing temporal + bilateral denoise -> composite
```

## Checkpoint 1–3: mesh and scene distance fields (implemented)

### Baking (`scripts/bake_sdf.py`)

- The engine writes each scene instance's **opaque** triangles, in the mesh's
  local space, to `assets/sdf/queue/<hash>.obj`.  The hash covers positions
  and indices only, so the cache follows geometry, not names or load order,
  and a mesh shared between files bakes once.  Transparent surfaces are left
  out, matching the raster G-buffer: otherwise window glass would block the
  light the render shows passing through.
- `python scripts/bake_sdf.py --queue assets/sdf/queue assets/sdf/cache`
  bakes each at 5 cm voxels in a grid shaped to the mesh, spending a voxel
  budget per mesh (4 M voxels, 1024 per axis) rather than a fixed cap per
  axis, so a mesh thin on one axis keeps its resolution on the others.
  Output format is documented in the script.
- **Every mesh is baked as a two-sided shell**: `|distance| − ½ voxel`, from
  distances to surface points sampled directly on the triangles.  This was
  measured, not assumed.  mesh_to_sdf's inside/outside test estimates normals
  from its virtual scans, so a zero-thickness wall seen from both sides gets
  opposite normals at the same place and the sign flickers, leaving holes and
  drips.  Closed meshes baked signed still tore open inside concave crevices
  (between sofa cushions).  Shells need no sign.
- The shell puts the surface half a bake voxel in front of the real one.  At
  5 cm that is a consistent ~3 cm early hit, measured as bias below.

### Scene field (`src/vk_engine_sdf.cpp`, `shaders/sdf_composite.comp`)

- Each instance's volume is merged into one R32F scene field (at most 384 per
  axis by default, adjustable to 512) by a compute pass that keeps the minimum
  distance.  Outside an
  instance's box it writes a conservative lower bound, and distances are
  converted with the transform's smallest scale so non-uniform scale never
  overstates free space.
- Distances are truncated at 8 field voxels, as Lumen's global field is.
- The merge is submitted in batches of bounded voxel work so a large scene
  cannot hit the Windows GPU timeout.
- It rebuilds when the set of drawn opaque instances or their transforms
  change.

### Verification

`scripts/check_sdf_agreement.py` compares each pixel's traced hit distance
with the raster depth, reporting bias (the median signed difference) and
scatter around it, in centimetres.

Re-measured 2026-09-17, after the per-mesh voxel budget replaced the
128-per-axis cap and the field grew to 384.  The camera is part of the
measurement, so each row records one.

| View | Camera | Field voxel | Bias | p90 scatter | Within 5 cm of bias |
|---|---|---:|---:|---:|---:|
| Sponza arcade | scene default | 9.7 cm | −8.4 cm | 3.5 cm | 95.8% |
| Living room, inside | `0 1.5 1.0 0 0` | 1.5 cm | −3.1 cm | 15.7 cm | 78.9% |

The superseded numbers, measured under the 128-voxel cap and a 256 field,
were Sponza −12.9 cm bias with 17.2 cm p90 scatter and 52.4% within 5 cm.
Sponza's scatter is now inside the script's 5 cm limit; its bias is not, and
the script still exits non-zero for that reason.

- Orientation (all 48 axis permutations and flips), world scaling and bounds
  are separately verified by `bake_sdf.py --verify-axes`.
- **Bias tracks the field's voxel size, not placement.** The merge thickens
  every shell to at least ¾ of a field voxel, so the expected early hit is
  `max(½ bake voxel, ¾ field voxel)`: 2.5 cm for the living room at 1.5 cm
  voxels (−3.1 cm measured), 7.3 cm for Sponza at 9.7 cm (−8.4 cm measured).
  Both scenes land within about a centimetre of that prediction, which is
  what rules out a placement or orientation error.  Raising the field
  resolution lowers this bias directly; clipmaps are the way to raise it only
  where the camera is, which is the scaling step the gather will want.
- The living room row is not comparable to the superseded one: the camera the
  original used was never recorded, and the one above looks partly through
  the windows, where the raster pass back-face culls surfaces the two-sided
  field keeps.  That is what the p99 of 6 m and the wide p90 are — a handful
  of pixels seeing through the set, not a regression in the field.  A fixed
  interior camera is needed before this row means anything.
- Debug and synchronization validation layers are clean for these passes.

### Known limitations

- Alpha-masked surfaces (foliage) are baked as solid cards.
- One field over the whole scene; no clipmaps, no near-camera mesh tracing.
- Static: moving an object rebuilds the whole field.

## Checkpoints 4–6: surface cache

### Cards

For each opaque instance, six cards, one per local axis direction (±X, ±Y,
±Z), each an orthographic view of the instance's local bounding box looking
back along that direction.  A card only keeps surfaces facing it, so a
room's inward-facing walls are each captured by the card on the opposite
side of the box.

Six box cards miss surfaces hidden behind others facing the same way (an
arcade's upper and lower floors).  Lumen clusters surfaces into many cards;
the simpler first step here is to measure coverage and, where it falls
short, add depth-peeled layers per direction.

### Atlas

One atlas, rectangles allocated per card at a target of one texel per 5 cm,
scaled down globally if the scene does not fit.  Pages:

| Page | Format | Holds |
|---|---|---|
| Albedo | RGBA8 | base colour × factors, alpha = coverage |
| Normal | RGBA8 | world normal |
| Emissive | RGBA16F | emitted radiance |
| Depth | R16F | position along the card axis, 1 = nothing captured |
| Direct | RGBA16F | sun and emissive lighting |
| Indirect | RGBA16F | accumulated bounce lighting |

### Capture (checkpoint 4)

Rasterise each instance into its card rectangles with its real materials,
using the forward pass's material descriptor sets and push-constant layout
(the three spare rows carry the orthographic card projection).  Static
scenes capture once, on scene change.

Acceptance: an atlas debug view shows recognisable albedo and normals; a
coverage metric reports the fraction of each mesh's surface area captured by
some card.

**Implemented** (`src/vk_engine_surface_cache.cpp`, debug view "Surface cache
atlas", `MIRABILIS_SURFACE_CACHE_COVERAGE=1`).  The capture reuses the forward
pass's pipeline layout and material sets; the three previous-transform rows
of its push constants carry the card projection, and the fragment shader
discards surfaces facing away from the card from their normals, because the
engine draws with culling off and glTF winding is not reliable.  Coverage
samples points over every instance's opaque triangles and checks the depth
page:

| Scene | Cards | Atlas | Capture | Surface area covered |
|---|---:|---:|---:|---:|
| Living room | 372 | 512² at 5 cm | 14 ms | 91.8% of 148 m² |
| Sponza | 672 | 4096² at 5 cm | 88 ms | 88.6% of 9,681 m² |

The misses are concentrated in large concave meshes (one Sponza chunk is at
65%), exactly the case six box cards cannot see; depth-peeled layers are the
planned fix once lighting is running end to end.  Debug and synchronization
validation are clean.

### Lighting (checkpoint 5) — implemented

`shaders/surface_cache_direct.comp` lights every captured texel with the sun,
shadowed by a soft sphere-traced march through the scene field, and stores
the light arriving (`sunColor × N·L × visibility`, no albedo) so the forward
pass's Lambert term and the cache agree by construction.

A shadow map is the wrong referee for this: its depth bias leaks light past
edges the field correctly blocks.  The referee is exact: sample sun-facing
cache texels, rebuild their world positions from the depth page, and cast one
ray per sample through the path tracer's CPU BVH of the real triangles
(`MIRABILIS_SURFACE_CACHE_SHADOW_CHECK=1`).

| Scene | Lit by exact rays | Lit in cache | Agree |
|---|---:|---:|---:|
| Sponza | 49.0% | 48.8% | 97.2% |
| Living room | 30.3% | 27.1% | 96.6% |

Errors are one-sided toward shadow: thin geometry (window bars) thickened by
its shell narrows the light passing through it.

### Lookup — implemented

`shaders/surface_cache_lookup.glsl` answers "what does the cache say about this
world point": a uniform card grid lists overlapping cards per cell; each card
facing the query normal projects the point, reads its depth page, and the
surface nearest behind the point wins.  Cards whose surface lies further back
fade by a Gaussian of a quarter texel, so a second card that captured the same
surface still blends in but a different surface two centimetres behind (a
light panel under a ceiling) does not.  At G-buffer positions a card is found
for 94–95% of pixels, and the looked-up albedo matches the G-buffer's (median
ratio 1.07).

### Radiosity (checkpoint 6) — implemented

`shaders/surface_cache_radiosity.comp`: each update, the next cards in
round-robin order up to a texel budget trace cosine-weighted rays through the
field; hits read `albedo × (direct + indirect) + emissive` from the cache and
rays that leave the field read the sky under SSGI's policy.  Averaging is
progressive (`1/n` per card, down to a floor) so a static scene keeps
converging.  Graded against the software path tracer in the Cornell box
(emitter only, sealed), with its depths 2–4 extrapolated to infinite bounces:

| Quantity | Cache | Path traced | Ratio |
|---|---:|---:|---:|
| One bounce (emitter light) | 0.0736 | 0.0781 | 0.94 |
| All bounces | 0.1955 | 0.2229 (extrapolated) | 0.88 |

The sealed box with no emitter converges to 0.0006, the same as with radiosity
off.  Noise on a flat wall after 150 updates: 18% standard deviation over
mean.

### Bugs the measurements found

Each of these produced plausible-looking images; each was found by a number
that disagreed with a reference.

1. **Per-axis bake cap.** 128 voxels per axis coarsened Sponza's 18 m roof
   lattice to 14.5 cm; its shells closed the gaps and shadowed the courtyard.
   Baking now spends a voxel budget per mesh.
2. **Single-sided thin surfaces.** A ceiling light built from an upward-facing
   plane was captured only on its top card, so rays from below read the dark
   ceiling.  Instances thinner than two texels are captured two-sided.
3. **Step exhaustion counted as sky.** A radiosity ray that ran out of march
   steps was treated as having left the scene.
4. **Shells thinner than the field.** A unit cube scaled to a 20 cm wall has
   5 mm shells, invisible at 2.5 cm field voxels, and rays passed through the
   walls of a sealed room.  The merge thickens every shell to at least three
   quarters of a field voxel.
5. **Blending across surfaces.** The lookup averaged every card within its
   depth tolerance, so the ceiling light was averaged with the ceiling above
   it and the room received half its light.
6. **`AllocatedImage` has no member initialisers**, so new members must be
   value-initialised (`AllocatedImage x{};`); found by the Debug build.

## Checkpoint 7: final gather — implemented

`shaders/ssgi_lumen.comp` is the SSGI trace (`shaders/ssgi_body.glsl`) compiled
with `LUMEN_LITE`; `ssgi.comp` compiles the same body without it, to
byte-identical SPIR-V, so plain SSGI is unchanged.  With the Lumen-lite world
fallback on (Render Settings checkbox, `MIRABILIS_LUMEN_LITE=1`):

- a ray the screen cannot answer traces the scene field and reads the cache at
  its hit, or the sky if it leaves the scene;
- a ray the screen does answer adds the cache's bounce light at the hit to the
  direct light the screen reads, so screen hits carry as many bounces as
  world-traced misses;
- the scene field, cache capture and cache lighting are each gated on a hash
  of what they depend on, so a still scene rebuilds none of them; only
  radiosity runs every frame, within its texel budget.

Graded in the Cornell box against the path tracer (albedo × SSGI's filtered
incident light, emitter pixels excluded):

| SSGI | Preset | Result ÷ extrapolated path-traced reference | SSGI GPU time |
|---|---|---:|---:|
| Plain (environment for misses) | Validation, full res | 0.054 | 3.0 ms |
| Lumen-lite, misses only | Validation, full res | 0.571 | — |
| Lumen-lite, misses and screen hits | Validation, full res | 0.767 | 21.9 ms |
| Lumen-lite, misses and screen hits | Balanced, half res | 0.692 | 3.0 ms |

Balanced timings on the RTX 3060 laptop, whole frame including radiosity:
Cornell box 7.7 ms, living room 7.0 ms, Sponza 8.4 ms (SSGI alone 3.0–4.4 ms,
against 0.5–0.7 ms for plain SSGI).  Debug and synchronization validation are
clean in the Cornell box and the living room.

## Stage: screen probes — implemented (#6)

Off by default: Render Settings "Screen Probes", or `MIRABILIS_SSGI_PROBES=1`
with Lumen-lite on. `shaders/ssgi_probe.comp` is the SSGI body compiled with
`SSGI_PROBE_TRACE`.

- One probe per 8×8 tile, on a pixel of that tile chosen by a hash of tile and
  frame, so placement jitters every frame and no placement buffer exists.
- Each probe traces the rays its tile's pixels would have (64 × rays per
  pixel), stratified uniform over its hemisphere, through the same
  `trace_incident()` the per-pixel path uses, and stores band-2 SH.
- Each pixel gathers from its four surrounding probes, weighted by the bilinear
  weight, distance off its plane and normal agreement, and evaluates
  irradiance at its own normal. A pixel no probe serves traces its own rays:
  2.1% of geometry pixels in the living room and 0.1% in the Cornell box, so
  the ray budget is 1.02× and 1.001×.
- The temporal clamp's 3×3 neighbourhood is taken at probe stride when probes
  are on. At pixel stride, a probe's output is smooth across its tile however
  noisy the probe was, so the clamp pins history to the current frame and
  nothing accumulates.

Acceptance, preset 0 (full res, 4 rays), Lumen-lite, 32 frames, filtered
indirect. Speckle is luminance std/mean after removing a least-squares
quadratic; with a plane instead the Cornell region reads flat at 11.8% → 11.7%,
because its green bleed is curved lighting and not noise:

| Region (full-res px), camera | Probes off | Probes on |
|---|---:|---:|
| Living room ceiling x 20–440 y 5–70, `'0 1.6 -3 0 3.14'` | 12.97% | **5.97%** |
| Cornell back wall x 450–650 y 190–370, `'0 2 3.5 0 0'` | 8.30% | **5.20%** |

With temporal accumulation disabled, the same regions read 40.1% → 16.4% and
29.7% → 14.7%. SSGI GPU time rises 20.1 → 22.0 ms (living room) and 22.0 →
22.6 ms (Cornell), Debug build.

**Found on the way: the existing temporal pass darkens indirect light by
~31%.** With accumulation off, the per-pixel region means are 0.01269 (living
room) and 0.3687 (Cornell); with it on, 0.00871 and 0.2530. The probe means
(0.01190, 0.3582) sit 6% and 3% below the unbiased figure, so turning probes on
*brightens* indirect light by ~37–42% against the shipped path. That bears on
the interior brightness error (#5): the shipped figure is lower than the trace
it comes from.

Not built: importance sampling, probe-space spatial filtering, adaptive
placement, per-probe temporal accumulation. Each waits for a measurement that
shows the simple version falls short.

## Stage: HZB screen tracing — implemented (#7)

Off by default: Render Settings "Hierarchical Screen Trace", or
`MIRABILIS_SSGI_HZB=1`. It works with or without Lumen-lite and with probes.

- There was no depth pyramid. `shaders/hzb_build.comp` builds one per frame,
  8 levels, RG32F: nearest and farthest depth of each 2×2, padded to a multiple
  of 128 px so every level halves exactly. It costs **0.11 ms** at 960×540 and
  is timed inside the trace pass.
- `hzb_march()` in `ssgi_body.glsl` walks the ray in screen pixels and NDC
  depth. A cell is skipped a level up when the ray is in front of its nearest
  depth, or behind its farthest by more than a hit's thickness. Otherwise the
  walk drops a level. At level 0 the fixed march's thickness test decides a
  hit, and a ray behind a thin surface carries on, so a hit means what it did
  before. The iteration cap is 4 × the preset's step count.
- Without the farthest channel, rays behind Sponza's columns walked a pixel at
  a time: 22.4% hits for 14.2 ms.
- A ray that starts behind the depth buffer is handed straight to the
  fallback. The Cornell emitter's normal points into the ceiling, so its rays
  began behind it. They found the panel's own emission and pushed the frame to
  1.46× the path-traced reference. Rejecting back-facing hits instead also cost
  real hits (0.895× → 0.858× off the panel).
- Rays the screen cannot answer go to `lumen_trace` exactly as before.

Acceptance: screen-only SSGI (no world fallback), preset 0, Release, RTX 3060
laptop, 300 frames. The fixed march's step count is its quality knob, so HZB is
compared against it at matched hit rate:

| Scene, camera | March | Hit rate | Screen trace |
|---|---|---:|---:|
| Sponza `'0 10 0 -0.5 0'` | fixed, 32 steps (shipped) | 17.2% | 1.71 ms |
| | fixed, 128 steps | 21.0% | 5.55 ms |
| | fixed, 256 steps | 21.7% | 10.69 ms |
| | **HZB** | **21.3%** | **3.44 ms** |
| Living room `'0 1.6 -3 0 3.14'` | fixed, 32 steps (shipped) | 23.2% | 0.96 ms |
| | fixed, 128 steps | 32.2% | 2.56 ms |
| | fixed, 256 steps | 34.8% | 4.47 ms |
| | **HZB** | **34.3%** | **2.03 ms** |

At matched hit rate HZB is 1.6× cheaper in Sponza and 2.2× in the living room.
The fixed march gets there only by taking far more steps. Against the shipped
32 steps, HZB costs about 2× the screen time for 24% (Sponza) and 48% (living
room) more hits. Under Lumen-lite those extra hits are rays the world trace no
longer takes. The whole trace pass goes 20.46 → 19.37 ms in the living room
and 34.04 → 34.39 ms in Sponza. The indirect mean moves +2.6% and −1.0%.

Cornell box against its 400-sample reference: frame 1.027× → 0.993×, deep
0.873× → 0.869×, identical off the emitter panel (0.895× / 0.894×).
With HZB off, captures are bit-identical to before in the living room and the
Cornell box.

## Next

In order. The first item blocks meaningful judgement of everything below it,
because a 3.8× scale error swamps every other difference.

1. **Resolve the interior brightness error** (see *Start here*). Re-run the
   comparison with environment policies matched, then occlude the raster
   ambient term. Expect `ambientRetention` to fall toward zero once the bounce
   chain is carrying the light the fudge was standing in for.
2. **Re-grade every scene against a reference afterwards.** The Cornell box
   numbers below were measured before the sky entered the cache and before
   this error was known.
3. **Anti-aliasing**, if the moulding stipple matters for how the renderer
   looks. It is a raster-path job — the velocity buffer and temporal
   reprojection TAA needs already exist, built for SSGI.
4. **Then** the architectural work, in this order: screen probes (basic
   version done, above; importance sampling and per-probe accumulation when
   measured to matter), HZB screen tracing (done, above), field clipmaps,
   world radiance cache, reflections. Each needs a written acceptance
   measurement before it starts, as checkpoints 1–7 had.
5. Coverage: depth-peeled card layers for concave meshes (Sponza ~89%). Do it
   when missing coverage visibly limits results.
6. VRAM: the Sponza atlas is ~1 GB, on a 6 GB laptop card. Measure before
   optimising.

Checkpoint 8 is not written yet. Whatever it turns out to be, it needs the
same thing the earlier checkpoints had and this document lost for a while: a
reference measurement decided in advance, and a camera recorded beside it.
