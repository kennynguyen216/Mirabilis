# Mirabilis Lumen-lite Global Illumination Design

## Document status

**Status:** Checkpoints 1–7 implemented and measured against exact
references: Lumen-lite diffuse GI runs end to end.
**Date:** 2026-09-17
**Supersedes:** the 3D fallback tier of
[hybrid_ray_traced_gi_design.md](hybrid_ray_traced_gi_design.md), which
traced triangles through a BVH or ray queries.  This project follows Unreal's
Lumen instead: screen tracing first, then mesh distance fields, with surface
colour and lighting read from a surface cache.

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
  bakes each at 5 cm voxels, capped at 128 per axis, in a grid shaped to the
  mesh.  Output format is documented in the script.
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

- Each instance's volume is merged into one R32F scene field (at most 256 per
  axis) by a compute pass that keeps the minimum distance.  Outside an
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

| View | Bias | p90 scatter | Within 5 cm |
|---|---:|---:|---:|
| Living room, inside | −3.5 cm | 2.5 cm | 98.1% |
| Living room, inside facing windows | −3.7 cm | 2.9 cm | 95.2% |
| Living room, outside the set | −2.9 cm | 11.3 cm | 85.6% |
| Sponza arcade | −12.9 cm | 17.2 cm | 52.4% |

- Orientation (all 48 axis permutations and flips), world scaling and bounds
  are separately verified by `bake_sdf.py --verify-axes`.
- The living room is an interior-only set: from outside, the raster pass
  back-face culls surfaces the two-sided field keeps, and every disagreeing
  pixel there has the field in front.
- Sponza's largest meshes hit the 128-voxel cap at 25–27 cm voxels and the
  scene field is 14.8 cm, so its bias and silhouette thickening are
  resolution, not placement.  Lumen gets near-camera detail from screen
  tracing; field clipmaps are the scaling step if GI shows leaks.
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
- the scene field, cache capture, lighting and radiosity update every frame.

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

## Next

- Close the remaining gap to the path tracer (cache ~12%, gather ~11% more):
  glossy energy, false screen hits, filter bias.
- Coverage: depth-peeled card layers for concave meshes (Sponza ~89%).
- Reflections, HZB screen tracing, field clipmaps, VRAM (Sponza atlas ~1 GB).
