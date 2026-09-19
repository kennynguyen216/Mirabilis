# Lumen-lite recovery task checklists

These checklists implement the recovery sequence in
[lumen_lite_design.md](lumen_lite_design.md). Execute one recovery task at a time; do not combine
stages or optimize for landing code before its gate passes.

## Required preamble for every coding session

```text
Read docs/lumen_lite_design.md completely before acting. It is the authoritative contract.
Work only on the named recovery item. Do not begin a later item.

Before editing:
1. Show branch, HEAD, git status --short, and relevant environment variables.
2. Preserve all pre-existing user changes; do not edit or commit assets/scenes/.last_scene.
3. Create a dedicated branch.
4. Paste a frozen experiment record using section 9 of the design document.
5. State every command and GPU workload before running it.

Safety:
- Do not run the Sponza path tracer on this laptop.
- Do not launch hidden/background GPU work.
- Do not push, fetch, merge, rewrite history, or touch tmp/card5.
- Use Release for performance claims.
- Stop on a failed gate, device instability, or missing evidence.

At the end, report the diff, exact commands, raw artifact paths, all regressions, and every
acceptance line as PASS, FAIL, or BLOCKED. Do not commit unless all gates for this item pass.
Do not proceed to the next recovery item.
```

## R1 — controls and diagnostics

```text
Perform only R1 from docs/lumen_lite_design.md.

Fix boolean environment parsing for MIRABILIS_LUMEN_LITE, MIRABILIS_SSGI_PROBES, and
MIRABILIS_SSGI_HZB so absent/empty/0/false/off are disabled, 1/true/on are enabled, and invalid
values warn and remain disabled. Prefer one tested parser.

Make material debug-mode values agree between C++ and shaders, preferably from one shared source
or with a test that fails on divergence.

Fix the Cornell ceiling emitter's geometry/winding/normal so its emitting face points into the
room. Remove only guards proven to be obsolete by the source fix. Preserve explicit one- versus
two-sided behavior; do not hide the error with abs(dot()).

Add focused tests. Build Debug and Release and run unit tests. Use only a short, visible Cornell
raster smoke test if runtime verification is necessary. Do not change GI equations, presets,
probes, HZB, distance fields, or performance behavior.
```

## R2 — coherent Balanced candidate

```text
Perform only R2 from docs/lumen_lite_design.md after R1 is accepted.

Initialize quality preset 2 through the existing preset-application function so every derived
setting is coherent. Keep Lumen-lite opt-in. Verify the startup log and explicit presets 0–4,
plain-SSGI feature-off equivalence, and the fixed living-room Release benchmark.

The living-room frame-time gate is 6.968 ms + 0.5 ms at the documented camera and conditions.
Do not tune shaders or alter GI output in this task.
```

## R3 — reference provenance

```text
Perform only R3 from docs/lumen_lite_design.md after R2 is accepted.

Create no renderer changes. Generate safe Cornell and living-room reference/candidate artifacts
with complete metadata, fixed masks, verified image orientation, and repeated reference seeds.
Do not run the Sponza path tracer. If a required safe reference cannot be generated, mark the
gate BLOCKED and stop; never claim a missing file exists.
```

## R4 — lighting ownership diagnosis and correction

```text
Perform only R4 from docs/lumen_lite_design.md after R3 is accepted.

First add or use captures that isolate direct sun diffuse, diffuse environment, indirect
diffuse, specular environment, emissive, screen hits, world hits, uncovered rays, and exhausted
rays. Use those measurements to identify the brightness error causally.

Before changing equations, state the expected direction and magnitude of each affected metric.
Apply receiver albedo exactly once and give every lighting contribution one owner. Do not add an
ambient-retention or brightness scalar as a visual patch. Evaluate whole images and frozen
regions against matched linear-HDR references.
```

## R5 — radiosity synchronization

```text
Perform only R5 from docs/lumen_lite_design.md after R4 is accepted.

Replace the per-frame full-atlas immediate submission/CPU wait with explicit GPU ordering and
stable resource ownership. Write the proposed resource-state timeline before code. Preserve the
determinism fixes rather than deleting synchronization.

Verify repeated hashes at frames 1, 32, 128, and 256; validation cleanliness; radiosity-on versus
off whole-frame cost; and static plus moving Sponza raster/Lumen behavior. Do not run a Sponza
path trace. The provisional living-room radiosity overhead gate is 0.5 ms.
```

## R6 — rebaseline and decision

```text
Perform only R6 from docs/lumen_lite_design.md after R5 is accepted.

Make no architecture changes. Run the complete fixed matrix and record averages, percentiles,
maximums, GPU pass times, memory, coverage classifications, linear-HDR correctness metrics, and
feature-off equivalence. Preserve raw artifacts.

Conclude only one of: Experimental, Accepted, or Default candidate, using the definitions in the
design document. Any missing or failed required gate prevents advancement.
```

## Template for a future feature

```text
Do not implement yet. Write a proposed experiment record for <feature> using section 9 of
docs/lumen_lite_design.md. Explain the causal limitation in the accepted baseline, the smallest
testable change, shipped baseline, immutable metrics, cross-scene matrix, frame/pass/tail/memory
budgets, feature-off test, safety constraints, and rollback.

Stop after the proposal. Implementation requires owner approval of the frozen gate.
```
