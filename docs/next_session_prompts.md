# Next session prompts

Paste **Block 1** as the first message of a new session. Everything else here is
a follow-on, to use when the one before it lands.

Bias for this stretch: **build the architecture.** The last three sessions
produced a complexity refactor, a correction to it, and documentation. Real, but
none of it was a Lumen stage. Checkpoints 1–7 are done; stages 8 onward are not
started. That is the work.

---

## Block 1 — the opener

```
Read docs/next_session_handoff.md for current state. Skim
docs/lumen_lite_design.md for architecture and the "Settled, do not
re-investigate" list. Don't re-derive either.

This session builds a Lumen stage: screen probes. I've spent recent sessions on
testing, refactoring and docs, and I want architecture now. Optimise for code
that lands, not for more measurement.

Step 0, time-boxed to the first hour: the interiors are ~3.8x too bright, and
the handoff traces it to the raster ambient term being unoccluded when SSAO is
off. If it's a small fix, make it. If it opens up, stop, write down what you
found, and move on to the probes anyway - don't let it eat the session.

Then screen probes. Before writing code, tell me in a few lines: where probes
live in the frame, how they're placed and reprojected, how the gather reads them,
and what you're reusing from the existing SSGI trace and temporal pass. Then
build it.

Acceptance, decided now so it isn't decided afterwards: probes reduce speckle at
a fixed ray budget. Measure noise as standard deviation over mean on a flat wall,
Cornell box and living room, cameras recorded. This is a relative measure, so the
brightness error doesn't invalidate it either way.

Build the stage, not a scene fix. No per-scene tuning.
```

---

## Block 2 — next stage, after probes land

```
Screen probes are in and measured. Next stage: HZB screen tracing.

Same shape as before - tell me the design in a few lines first, then build. The
acceptance measure, decided now: screen trace cost at equal or better hit rate,
in Sponza and the living room, cameras recorded. Compare against the current
fixed-step march in ssgi_body.glsl.

Reuse the existing depth pyramid if one exists; if not, say what building it
costs before you build it.
```

## Block 3 — the stage that needs the most design

```
Next stage: scene distance field clipmaps.

This is the one the field agreement measurements have been pointing at.
docs/lumen_lite_design.md records that bias tracks field voxel size directly:
-8.4 cm in Sponza at 9.7 cm voxels, -3.1 cm in the living room at 1.5 cm. One
field over the whole scene can't fix that; clipmaps raise resolution only where
the camera is.

Design first, in a few lines: how many cascades, what voxel sizes, how they
recentre as the camera moves, how a trace picks a cascade and crosses between
them, and what happens to the existing single-field path.

Acceptance: check_sdf_agreement.py bias within 5 cm in Sponza from camera
'0 10 0 -0.5 0'. That's the threshold the script already enforces and currently
fails.
```

## Block 4 — remaining stages

```
Next stage: <world radiance cache | reflections>.

Same discipline: short design first, acceptance measure decided before building,
architectural stage rather than scene tuning. Check
docs/lumen_lite_design.md for what the surface cache and gather already provide
so this builds on them rather than beside them.
```

---

## Side quests

Independent of the stages above. Use when one becomes annoying enough, or when
you want something small.

```
SSGI capture isn't run-to-run deterministic in the living room: the same binary
at the same frame count differs more per-pixel than two different builds do.
Details in docs/next_session_handoff.md. The Cornell box is bit-identical, so
it's scene-specific.

Find the cause. It blocks using capture diffs to validate changes in that scene.
```

```
Fix the ten validation warnings in the living room: mesh_shading.glsl writes four
MRT outputs but the portal view pass binds a single-attachment VkRenderingInfo.
They appear with SSGI fully disabled, so it's a raster/portal bug, not GI. Small
and self-contained.
```

```
The brightness error is closed. Re-grade every scene against a path-traced
reference and update the tables in docs/lumen_lite_design.md - the checkpoint 7
Cornell numbers predate both the sky entering the cache and the error being
known, so they aren't a usable baseline.

Run with environment policies matched (traceEnvironmentMap) and note in the doc
which policy each number used.
```

---

## Why the acceptance line is in every prompt

It's one sentence written before the code, not a testing phase. Checkpoints 1–7
each had one; checkpoint 8 doesn't, and the design doc's closing line asks for
it. That habit is also what caught the 3.8× error — which had been present,
unnoticed, across several sessions of work that looked fine.

It costs a line. It isn't the thing that's been slowing the architecture down.
