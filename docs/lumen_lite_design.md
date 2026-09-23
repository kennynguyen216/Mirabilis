# Mirabilis Lumen-lite GI: authoritative design and validation contract

| Field | Value |
|---|---|
| Status | Experimental; not accepted as the engine default |
| Specification version | 2.0 |
| Last evidence review | 2026-09-19 |
| Reviewed code baseline | `main` at `6d994b4`, tree-identical to `adc87e3` |
| Test GPU | NVIDIA RTX 3060 Laptop GPU |
| Preserved rejected work | `recovery/lumen-experiments` at `4241d01` |

This is the canonical design and acceptance document for Lumen-lite. Git history preserves
the earlier document as historical evidence; it is not an implementation contract. If a card,
prompt, comment, or old benchmark conflicts with this document, stop and resolve the conflict
before changing code.

The words **must**, **must not**, **accepted**, **failed**, and **blocked** are intentional.
An implementer may not silently weaken them.

## 1. Operating contract

### 1.1 Current decision

Do not add another GI subsystem yet. First complete the remediation sequence in section 10.
Screen probes, HZB tracing, and distance-field clipmaps remain experiments behind disabled
flags. Clipmaps are reverted. None may be described as shipped, complete, or accepted.

### 1.2 Non-negotiable rules

1. Before implementation, write down the hypothesis, comparison baseline, scenes, cameras,
   build type, metrics, thresholds, expected trade-offs, and rollback command.
2. Freeze that acceptance block before gathering results. A changed metric or threshold is a
   new experiment and must retain the failed result.
3. Compare against the configuration Mirabilis actually ships. A deliberately slow reference
   may be useful diagnostically but cannot establish a performance win.
4. Performance claims use Release builds. Debug timings may diagnose behavior but never support
   an acceptance claim.
5. Measure whole-frame time, affected GPU passes, tail latency or movement hitches, and memory.
   An average for one pass is insufficient.
6. Image comparisons use matched camera, geometry, materials, lights, exposure, resolution,
   crop, and linear-HDR data. Never choose a crop or detrending model after seeing the result.
7. Report every material regression in the result record before committing. A passing narrow
   metric does not cancel a frame-time, brightness, stability, or memory regression.
8. Never start a hidden or background GPU workload. State the exact workload first. Do not run
   the Sponza path tracer on the current laptop; it has repeatedly caused device hangs.
9. Missing evidence means **blocked**, not passed. Do not invent a reference, result, or test.
10. With an experimental feature disabled, output and performance must remain equivalent to the
    accepted baseline within the tolerances declared before the test.
11. Stop when a gate fails. Diagnose and record the failure before attempting a redesign.
12. A commit is not evidence. Build success and unit tests do not establish image correctness,
    performance, stability, or design completion.

### 1.3 Status vocabulary

| Status | Meaning |
|---|---|
| Proposed | A written hypothesis and frozen gate exist; implementation has not started. |
| Experimental | Code exists, but all acceptance evidence is not present. |
| Blocked | Required evidence or a safe test environment is unavailable. |
| Accepted | Every frozen gate passed and the result record is complete. |
| Default candidate | Accepted and explicitly approved for default-on soak testing. |
| Rejected | A gate failed; retain evidence and do not build dependent work on it. |
| Reverted | Rejected code was removed from the active tree and preserved in history. |

Only the project owner may relax a gate, and only before the next attempt begins.

## 2. Evidence-backed current state

| Component | State | Evidence-based conclusion |
|---|---|---|
| Existing Lumen-lite foundations | Experimental | Functional, but radiometric ownership and reference parity are unresolved. |
| Preset 0 | Diagnostic only | At 960×540 it is about 7.7× the measured plain-SSGI frame time in the living-room view. |
| Preset 2 (Balanced) | Performance candidate | Viable living-room cost, but not yet a correctness-accepted default. |
| Surface-cache radiosity | Experimental | Determinism fix is credible, but a full-atlas copy plus GPU wait occurs each Lumen-lite frame. |
| Screen probes | Inconclusive; off | Original quality metric was selected after results; Debug timing and brightness regression invalidate acceptance. |
| HZB screen trace | Failed; off | It loses against the shipped 32-step baseline and has a bounds-safety concern. |
| Distance-field clipmaps | Reverted | Severe Sponza time, brightness, memory, and movement-hitch regressions. |
| Sponza path-traced reference | Blocked | No valid reference exists; do not claim one does. |

### 2.1 Reproducible living-room performance evidence

Release build, fixed camera `4.740 1.700 -2.947 -0.133 -14.670`, 300-frame run,
295 measured samples:

| Configuration | Internal extent | Frame ms | SSGI GPU ms | Trace GPU ms |
|---|---:|---:|---:|---:|
| Plain SSGI, explicit preset 0 (pre-R2 implicit default) | 960×540 | 3.779 | 2.345 | 1.991 |
| Plain SSGI, explicit preset 2 (R2 implicit default) | 480×270 | 1.894 | 0.503 | 0.295 |
| Lumen-lite, preset 0, radiosity off | 960×540 | 29.063 | 27.617 | 27.257 |
| Lumen-lite, preset 0, radiosity on | 960×540 | 31.018 | 27.590 | 27.215 |
| Lumen-lite, preset 2, radiosity off | 480×270 | 5.154 | 3.844 | 3.643 |
| Lumen-lite, preset 2, radiosity on | 480×270 | 6.968 | 3.868 | 3.664 |

Interpretation:

- Preset 0 is a validation mode, not a reasonable default on this GPU. The 3.779 ms row is
  retained as the pre-R2 explicit-preset-0 baseline; it is not the post-R2 shipped default.
- The preset-2 row was measured at R2 with Lumen-lite, probes and HZB explicitly disabled.
  Re-measuring explicit preset 0 at the same build gave 4.089 ms and 3.750 ms in two runs
  against the recorded 3.779 ms, so preset 0 reproduces and neither R1 nor R2 regressed it.
  Both samples are recorded because the spread is the finding: run-to-run variance is about
  0.34 ms peak-to-peak at this frame time, which consumes most of the 0.5 ms pass budget in
  section 8.1. Treat a sub-0.5 ms difference from a single run as unmeasured, not as a pass.
- Balanced preset 2 is the only currently measured default candidate.
- At preset 2, enabling radiosity adds about 1.81 ms of whole-frame cost while the reported
  SSGI and trace timings barely move. The known blocking atlas transfer is the leading cause
  to verify, not a settled attribution.
- These numbers are one scene and one camera. They do not establish cross-scene acceptance.

### 2.2 Confirmed correctness and engineering debts

Resolved by R1 at `bac9b45`, retained so the evidence trail stays readable:

- `MIRABILIS_LUMEN_LITE`, `MIRABILIS_SSGI_PROBES`, and `MIRABILIS_SSGI_HZB` tested variable
  presence, so `=0` enabled them. They now parse `0`/`false`/`off`/`no` as disabled through
  `parse_env_bool` in `src/env_flags.h`, and an invalid value warns and stays disabled.
  Every other `MIRABILIS_*` variable is still presence-based.
- The material-debug shader constants were two positions out of sync with the C++ enum, so
  nine debug views showed the wrong buffer. `tests/test_debug_view_modes.py` now fails if they
  drift again.
- The Cornell ceiling emitter faced upward into the ceiling, and the path tracer's absolute
  cosine hid it. Geometry, winding and declared sidedness were corrected at the source.

Still open:

- HZB code performs an initial texel fetch before proving that the starting location is within
  the valid screen/segment bounds.
- Surface-cache radiosity copies the full RGBA16F indirect atlas and waits for completion on
  every Lumen-lite frame.
- The older “3.8×” living-room brightness claim concerns a deep-interior subset, not the whole
  image. Whole-interior evidence was roughly 1.414×. Neither comparison is accepted until
  reference parity is re-established.
- The current one-cone `sky_visibility` estimate cannot faithfully represent partial
  hemispherical visibility.

## 3. Product scope

Lumen-lite targets stable, diffuse dynamic global illumination for indoor and mixed
indoor/outdoor scenes on mid-range Vulkan hardware. It should reuse screen-space information
when reliable, fall back to a bounded world-space representation, and amortize expensive
lighting work through a surface cache.

The first accepted version must provide:

- stable diffuse indirect lighting during camera motion;
- screen trace plus explicit world fallback behavior;
- a bounded, updateable surface cache;
- a Balanced-quality mode suitable for the RTX 3060 Laptop GPU;
- deterministic automated captures; and
- debug views and measurements that agree with the implementation.

The first version does not promise:

- full Unreal Engine Lumen feature parity;
- production screen probes;
- specular GI or reflection replacement;
- arbitrary dynamic geometry in distance fields;
- path-traced reference generation for Sponza on this laptop; or
- zero bias. Bias must instead be measured, bounded, and documented.

## 4. Radiometric and ownership contract

### 4.1 Stored quantities

Use one convention throughout code, shader comments, debug views, and tests:

- `Li`: incident radiance arriving at a surface.
- Direct diffuse cache value: irradiance divided by π, before receiver albedo.
- Indirect diffuse cache value: irradiance divided by π, before receiver albedo.
- Emissive: emitted radiance, stored and added separately.
- Final diffuse outgoing radiance:

  `Lo_diffuse = receiver_albedo * (direct_E_over_pi + indirect_E_over_pi)`

- Final material output:

  `Lo = Lo_diffuse + emissive + separately_owned_specular`

A producer must not pre-apply receiver albedo if the consumer applies it. A sampled screen
pixel must be converted to the documented incident-light representation before reuse; do not
reapply the source material's albedo as though it were transport.

### 4.2 Lighting ownership

| Contribution | Sole owner |
|---|---|
| Direct sun diffuse | Direct-lighting path or direct surface-cache channel |
| Environment diffuse | One diffuse-environment path, selected explicitly |
| Indirect diffuse bounce | Lumen-lite/SSGI result |
| Specular environment/reflections | Existing specular/IBL path |
| Material emission | Emissive material path and explicitly sampled transport |

No contribution may be owned by two paths. `ambientRetention` or an arbitrary multiplier is
not a correctness mechanism. If the baseline ambient term is retained for graceful fallback,
its ownership, units, and blend equation must be documented and tested.

### 4.3 Ray-result contract

Every ray must end with an explicit classification:

| Result | Meaning and allowed output |
|---|---|
| Screen hit | Valid visible-surface sample converted to the incident-light convention |
| Covered world-field hit | Sample the documented surface-cache/world-field representation |
| Covered world-field miss/exit | Environment radiance, if the ray demonstrably exits geometry |
| Uncovered world-field region | Explicit “unknown/uncovered” policy; never silently sky |
| Step-budget exhaustion | No estimate/low confidence; never silently sky |
| Portal/window exit | Environment only when the exit is demonstrated |

Returning sky for “no card,” “outside coverage,” or “ran out of steps” creates light leaks and
must not be used as a convenience fallback.

### 4.4 Surface sidedness

Emitters and cards must have a declared sidedness. One-sided emitters use a physically
consistent outward normal and a clamped cosine. Tests must not use `abs(dot(n, wi))` to make
back-facing emitters appear valid unless a material is explicitly two-sided.

## 5. Pipeline and architecture

```text
G-buffer + depth
       |
       v
bounded screen trace ---- valid hit ----> incident-light sample
       |
       +---- miss/uncertain ----> bounded world fallback
                                      |
                                      v
                               surface-cache lookup
                                      |
                                      v
                         classified ray result + confidence
                                      |
                                      v
                         spatial/temporal reconstruction
                                      |
                                      v
                   receiver albedo applied exactly once
```

### 5.1 Current world representation

The accepted baseline currently contains the pre-clipmap scene distance-field path. Its
monolithic volume and current resolution are constraints to measure, not permission to replace
it without a migration gate. The reverted clipmap implementation is evidence that a nominally
more scalable structure can still lose on tracing, lighting, memory, and update hitches.

A future world representation must define:

- coverage and resolution as world-space functions;
- how uncovered space is classified;
- update scheduling and maximum work per frame;
- static versus dynamic geometry behavior;
- memory ownership and hard budget;
- synchronization and resource lifetime;
- camera movement, teleport, and scene-change behavior; and
- an accuracy test against analytic shapes before scene tests.

### 5.2 Surface cache

Surface cards cache lighting; they are not geometry truth. Missing card coverage must remain
distinguishable from a geometric miss. Card allocation, atlas capacity, and eviction must have
observable counters. Historical Sponza card-memory pressure, including an approximately 1 GB
configuration, is a warning that capacity cannot be chosen from Cornell alone.

### 5.3 Direct cache and sky visibility

A single normal-directed visibility cone is not hemispherical diffuse visibility. Until a
better estimator is accepted, label this term as an approximation and measure its bias in
windowed, corner, open-sky, and deep-interior regions. Do not compensate globally with a
brightness multiplier.

### 5.4 Radiosity update

The deterministic copy introduced at `7e80627` fixed two suspected GPU races, but its
per-frame full-atlas transfer and synchronous wait are not acceptable as the final design.
A replacement must preserve ordering with explicit GPU dependencies and avoid a CPU-visible
queue drain in the steady-state frame.

### 5.5 Quality presets

Current preset behavior:

| Preset | Name/use | Internal resolution | Rays | Steps |
|---:|---|---|---:|---:|
| 0 | Full-resolution validation | Full | 4 | 32 |
| 1 | High | Half | 4 | 48 |
| 2 | Balanced candidate | Half | 2 | 32 |
| 3 | Low | Half | 1 | 16 |
| 4 | Reference stress mode | Full | 8 | 96 |

Preset 0 is not the performance default. If changing the default, call the same preset
application function used by runtime selection; changing only the integer leaves derived
settings inconsistent.

## 6. Experimental feature decisions

### 6.1 Screen probes

Status: **inconclusive, disabled**.

The existing implementation is a skeleton, not a Lumen-style screen-probe system. It lacks
importance sampling, probe-space filtering, adaptive placement, and per-probe temporal
accumulation. The original speckle metric was selected after a quadratic detrend produced a
passing result; a planar detrend changed the Cornell region only from 11.79% to 11.67%.
Timings came from Debug, and indirect illumination rose by 37–42% against the shipped path.

Do not extend this implementation until the base radiometric contract passes. A future proposal
must specify probe placement, sampling PDF and weights, reconstruction, disocclusion, temporal
state, failure modes, and an immutable quality gate before code.

### 6.2 HZB screen tracing

Status: **failed, disabled**.

The original performance comparison used 128/256-step linear marches rather than the shipped
32-step march. Against the shipped baseline, HZB approximately doubled screen-trace cost; the
whole Lumen-lite trace was slightly slower in Sponza and only about 1 ms faster in the living
room. It also requires a bounds-safe first sample and a real fix for the Cornell emitter.

A retry must:

- compare against the shipped screen tracer at matched scene hit rate and image error;
- prove every texture fetch is in bounds;
- report hierarchy build cost and whole-frame cost;
- include thin geometry, screen-edge starts, behind-camera starts, and disocclusion; and
- beat the baseline on the predeclared scene matrix without a correctness loss.

### 6.3 Distance-field clipmaps

Status: **rejected and reverted**.

Observed Sponza regressions:

- GI trace: about 34 ms to 49.5 ms;
- indirect mean: 0.0549 to 0.0302, approximately 45% darker;
- field memory: 83 MB to 222 MB; and
- camera movement: approximately 25 ms hitch every 2 m.

An attempted repair reached roughly 53 ms and was reverted. No Sponza reference exists, so the
darker result cannot be called more correct.

Do not resurrect the patch incrementally. Any retry requires a new design with analytic
distance tests, explicit uncovered-space behavior, bounded incremental updates, a memory
budget, and static/moving/teleport gates.

## 7. Correctness validation protocol

### 7.1 Reference metadata

Every comparison directory must contain or record:

- commit hash and dirty-tree status;
- executable configuration and shader hash/time;
- GPU and driver;
- scene asset hash;
- camera transform;
- resolution, internal extent, sample count, and random seed;
- all relevant environment variables;
- light, material, exposure, tone-map, and sky settings;
- capture format and row orientation; and
- exact command used.

If any parity field is unknown, the comparison is diagnostic only.

### 7.2 Reference meaning

A finite-sample path trace is a noisy reference, not exact truth. Record sample count and, when
possible, repeat seeds to estimate reference variance. Never call a file a reference unless it
exists and its provenance is recorded.

### 7.3 Required image metrics

Evaluate linear HDR before tone mapping:

- mean luminance ratio and signed bias;
- MAE and RMSE;
- median and p90 relative error with a declared dark-pixel floor;
- non-finite and negative-value counts;
- temporal variance for static and moving cameras; and
- valid-hit, fallback, uncovered, and exhaustion coverage.

Report the whole image and predeclared semantic regions. A deep-interior crop may reveal a
problem but cannot be presented as whole-scene behavior. Detrending, masks, and crop coordinates
must be frozen before viewing results. Verify PFM/image row orientation with a known marker.

### 7.4 Required scenes and fixed cameras

| Scene | Camera | Purpose |
|---|---|---|
| Living-room interior | `0 1.6 -3 0 3.14` | Matched-reference whole room and deep interior |
| Living-room window view | `4.740 1.700 -2.947 -0.133 -14.670` | Performance and high-contrast leakage |
| Cornell box | `0 2 3.5 0 0` | Controlled energy/orientation test; regenerate after emitter fix |
| Sponza | `0 10 0 -0.5 0` | Raster/Lumen performance, coverage, memory, and movement only |

Camera values must be verified against the parser before capture. Sponza path-traced reference
generation is forbidden on the current laptop.

## 8. Performance and stability protocol

Use Release, a fixed executable and shader set, fixed camera, fixed resolution, and an idle
machine. Warm up before measuring. A nominal run uses 300 frames with at least 295 measured
samples. Record average, median, p95, p99, and maximum frame time, affected GPU pass times,
CPU frame time where available, allocated/used VRAM, and device errors.

Run both static-camera and scripted-motion tests. Averages cannot detect the prior recurring
25 ms clipmap hitch.

Command pattern:

```bat
set MIRABILIS_TEST_CAMERA=<verified camera>
set MIRABILIS_TEST_FRAMES=300
set MIRABILIS_SSGI_BENCHMARK=1
set MIRABILIS_SSGI_PRESET=<preset>
cd bin\Release
engine.exe > ..\..\tmp\benchmark-<descriptive-name>.txt 2>&1
```

Do not set boolean presence flags to `0` until section 10 fixes parsing; remove them from the
environment to disable them.

### 8.1 Provisional regression gates

These protect the current recovered baseline until a fuller baseline is recorded:

- Balanced living-room frame time must not exceed 6.968 ms by more than 0.5 ms.
- A changed GPU pass must not regress by more than 0.5 ms without a pre-approved quantified
  correctness gain.
- A recurring movement hitch must not exceed baseline p99 by more than 2 ms.
- GPU memory must not rise by more than 5% without prior approval and a documented budget.
- Mean indirect luminance must not fall by more than 2% unless matched-reference error improves
  by a predeclared amount and the direction was predicted.
- Feature-off captures must remain bit-identical where determinism permits, otherwise within a
  predeclared numerical tolerance.
- Validation errors, device loss, non-finite output, and corrupted captures are automatic fails.

Record fresh post-revert Sponza raster/Lumen baselines before using numeric Sponza gates.

## 9. Experiment record template

Copy this block into the issue/card before implementation:

```markdown
Hypothesis:
Baseline commit/configuration:
Proposed change:
Expected correctness effect:
Expected performance/memory effect:
Scenes and exact cameras:
Build and hardware:
Reference provenance:
Frozen metrics and thresholds:
Feature-off equivalence test:
Static/movement/teleport tests:
Safety constraints:
Rollback command:
```

After testing, append:

```markdown
Result commit and dirty-tree status:
Exact commands/environment:
Raw artifact paths:
Complete results, including regressions:
Gate-by-gate PASS / FAIL / BLOCKED:
Unexpected observations:
Decision: accepted / rejected / blocked
Rollback performed:
```

No results comment means no acceptance. “Looks better,” build success, or one passing metric is
not a result record.

## 10. Recovery sequence

Complete in order. Each item gets its own small commit only after its gates pass.

### R1 — Repair trustworthy controls and diagnostics

Implement:

1. Parse boolean environment values consistently: absent/empty/0/false/off disable; 1/true/on
   enable; invalid values produce a warning and remain disabled.
2. Align material-debug shader constants with the C++ enum from one shared definition or a test.
3. Correct the Cornell emitter geometry/winding/normal and enforce declared sidedness.
4. Add CPU/unit tests for boolean parsing and debug-mode mapping where practical.

Acceptance:

- `=0` demonstrably disables Lumen-lite, probes, and HZB;
- all C++ modes select their matching shader view;
- Cornell emitter normal points into the room without a start-behind workaround;
- Debug and Release compile, existing unit tests pass; and
- no unrelated renderer behavior changes.

### R2 — Make Balanced the coherent candidate default

Use the preset application function to initialize preset 2 and all derived fields. Do not merely
change `qualityPreset`.

Acceptance:

- startup log states mode, selected preset, output extent, and internal SSGI extent;
- plain SSGI shader behavior and explicit presets 0–4 remain unchanged;
- the implicit startup preset intentionally changes from preset 0 to preset 2, so the
  pre-R2 plain-SSGI figure in section 2.1 no longer describes the shipped default;
- feature-off equivalence must compare identical explicit presets, not the old and new
  implicit defaults;
- explicit preset overrides still reproduce their table and win over the startup default;
- Lumen-lite remains opt-in until correctness gates pass; and
- living-room performance stays within section 8.1.

### R3 — Establish matched references — COMPLETE

After the emitter fix, regenerate safe Cornell and living-room references with complete metadata.
Do not change renderer behavior during this task. If a safe reference cannot be produced, mark
the affected gate blocked.

Acceptance:

- files exist and can be decoded;
- parity fields are complete;
- repeated reference seeds quantify noise;
- masks/crops are stored before candidate comparison; and
- orientation is verified.

#### R3.1 Frozen environment-parity contract for later candidates

Frozen now, before any candidate exists, so the matching rule cannot be chosen after
seeing a comparison result. This section describes how a candidate must be configured to
be comparable with these references. It changes no lighting equation and no default.

The two paths do not light a ray miss the same way:

- the software path tracer fills misses from the **analytic gradient**;
- SSGI fills misses from the **selected skybox**, because `traceEnvironmentMap` defaults
  to true (`SSGIState`, `src/vk_engine.h`).

A candidate compared against an R3 reference must therefore run with `traceEnvironmentMap`
false, so both paths light misses from the analytic gradient. No other environment
reconciliation is permitted: the reference must not be re-rendered against the skybox, the
candidate must not be scaled to match, and `ambientRetention` must not be used to close the
gap. If the two still disagree with `traceEnvironmentMap` false, that disagreement is a
finding for R4, not a parameter to tune.

Known gap, recorded rather than worked around: **no environment variable sets
`traceEnvironmentMap`**, so an unattended candidate run cannot currently select it. Until
that control exists, every candidate-versus-reference comparison is diagnostic only and
may not discharge a gate. Adding it is the first task of R4, not of R3, because R3 must not
change renderer behavior.

Parity is auditable after the fact: each capture's sidecar records sun radiance,
environment intensity and the black-environment flag, so a comparison whose two sides
disagree on those fields is invalid regardless of what its metrics say.

#### R3.2 Result — accepted, 0 failed, 0 blocked

Every R3 acceptance item above passes. Validation reports **0 failed, 0 blocked**.

| | |
|---|---|
| Source commit | `89b5e7794d48592d901b69dd7e8290bbb1a50d8a`, clean worktree |
| Seeds | 1337, 2026, 90210 |
| Samples per seed | 512 accumulated path-traced samples, identical for every scene and seed |
| Scenes | Cornell (`gi_cornell_box.json`, camera `0 2 3.5 0 0`) and living room (`living_room_showcase.json`, camera `0 1.6 -3 0 3.14`) |
| Executable build | Release, engine SHA-256 `EA9255A3401758ABEB1AAE917D4F2C0C8910B52CD3C3D53CCED76170718F13C7` |
| Artifact directory | `tmp/r3-references/20260922-212524` (local only — see below) |

Canonical references, SHA-256 of the linear-HDR PFM. All six are recorded because R4 compares
against the indirect files directly, not only the combined ones:

```
E38AB084D90B9B23D5B667189099B5F480B3F303EB26E59F19FEDE9DB6025663  cornell-reference.direct.pfm
579E3A3D7867D8CB6F51D6950294A7FEF9319B1FEAC9E5469D23418485BA9A26  cornell-reference.indirect.pfm
DCF02720C1328019904539A1AD6DA536D251D10E5FDE6634B999443BE3E7ACA1  cornell-reference.pfm
B70CDE1D4DC159DA8D6FC9DFD2268B6AE661C24D5C3069843F8FAE530CFB15FB  living-room-reference.direct.pfm
9E062708263D99BE5FD65A6875851CAF555A1299DEC02372446429CDD15B86B5  living-room-reference.indirect.pfm
64FFE8792C23B48E7C0243AC3D27DC8AB50FE6145F7817C37CF7EDE842BAA2E6  living-room-reference.pfm
```

A candidate that cites an R3 reference must cite the hash of the exact file it compared against.
The per-seed captures these were formed from are retained unmodified.

**These references are not yet usable to discharge a gate.** R3.1 still holds: no environment
variable sets `traceEnvironmentMap`, so an unattended candidate cannot be put in environment
parity with them. That control is the first R4 task.

##### Measured reference noise

Two different quantities are measured here and they must not be confused, because they differ by
two to three orders of magnitude on the same data. **Neither is a statistical bound**, and
neither may be used as a tolerance on its own:

- **Pixel-level noise** — the mean of the per-pixel sample standard deviations across the three
  seeds, divided by the region mean. It is an empirical noise estimate relevant to *pixelwise*
  metrics: MAE, RMSE, per-pixel ratio, image difference. It informs their uncertainty; it does
  not directly bound either metric, because the relationship between per-pixel scatter and an
  aggregated error statistic depends on the metric's own form.
- **Region-mean spread** — the sample standard deviation of the three per-seed *region means*,
  divided by the region mean. It is an empirical repeatability estimate relevant to
  *region-level* metrics: mean ratio, signed bias, or any other aggregate over a region.
  Averaging over a region cancels most of the per-pixel scatter, which is why it is so much
  smaller. It is computed from **three samples**, so it is a repeatability measurement, not a
  confidence interval and not a hard limit.

Full per-region data is in `noise.json` and `validation.txt` beside the captures.

| Scene | Component | Region | Pixel-level | Region-mean spread |
|---|---|---|---:|---:|
| Cornell | combined | whole | 0.9018% | 0.0009% |
| Cornell | direct | whole | 0.2946% | 0.0013% |
| Cornell | indirect | whole | 6.9295% | 0.0038% |
| Cornell | indirect | floor_centre | 6.0289% | 0.0429% |
| Cornell | indirect | ceiling_emitter | 6.2684% | 0.0756% |
| Cornell | indirect | right_wall_green | 6.5476% | 0.0199% |
| Cornell | indirect | back_wall_centre | 7.3919% | 0.0286% |
| Cornell | indirect | deep_interior | 8.0740% | 0.0334% |
| Cornell | indirect | left_wall_red | 8.8786% | 0.0345% |
| living room | combined | whole | 2.2008% | 0.0694% |
| living room | direct | whole | 1.6849% | 0.0058% |
| living room | indirect | whole | 19.8838% | 0.9441% |
| living room | indirect | deep_interior | 17.0737% | 0.0378% |

The direct component is quiet by both measures. The indirect component is noisy **per pixel** —
Cornell indirect regions sit at roughly 6–9%, and the living room at 19.8838% whole and 17.0737%
deep interior, because its indirect means are small (0.0094 and 0.0158 linear) so the per-pixel
scatter is a large fraction of them. Its **region means** are nevertheless repeatable across the
three seeds: 0.9441% and 0.0378% respectively, and every Cornell indirect region is below
0.076%.

**Consequence for R4.** No pass/fail exclusion zone is defined here, and neither column may be
turned into one. In particular the 17–20% figures describe average pixel-level scatter; they do
**not** express an uncertainty in a region-level mean ratio, and using them that way would
discard real R4 findings as noise. What this table provides is prior evidence of where the
reference is noisy and where it is repeatable — an input to choosing an uncertainty method, not
the method itself.

R4 must **derive an uncertainty for each metric it reports, and freeze that derivation in
writing before viewing any candidate result.** Deriving it means computing the metric itself
under the reference's own variation — for example evaluating the metric against each seed
individually and against the ensemble, or defining a bootstrap or confidence procedure over the
per-seed captures — and stating the pass/fail rule in terms of that derived quantity. Choosing
the method after seeing the numbers is the failure mode section 9 exists to prevent.

What follows from a result that is small relative to its derived uncertainty is likewise for the
frozen method to state. More samples per seed, with a reference set regenerated at that higher
sample count, is one available response; it is not an automatic consequence of any figure in
this table.

##### Cross-run determinism observation — not blocking, deferred to R5

Two capture runs produced output that was not byte-identical: **15 of 18 raw PFM files differed**
between `tmp/r3-references/20260922-204519` and `tmp/r3-references/20260922-212524`.

What was identical between the two runs: the **engine binary**
(`EA9255A3401758ABEB1AAE917D4F2C0C8910B52CD3C3D53CCED76170718F13C7`), the **shader manifest**,
the **scene manifest**, and the **seeds** (1337, 2026, 90210) — verified by comparing
`binary-sha256.txt`, `shader-sha256.txt` and `scene-sha256.txt` across the two directories.

The two runs were **not** from the same Git commit: the first recorded `742c0a6` and the
accepted run `89b5e77`. Both trees were clean. The intervening commits changed capture tooling,
not renderer code — which is consistent with the identical binary and shader hashes — but the
commit is not the thing that was held constant, and the observation must not be cited as if it
were. The controlled inputs are the four listed above.

The differences are small: mean absolute difference approximately **1e-9 to 3e-8**, maximum
absolute difference **0.0017**. **The cause is unexplained.** Non-deterministic accumulation
order is one hypothesis and has not been tested; so are driver or scheduling variation and
uninitialised state. Nothing here establishes which, and the record should not imply otherwise
until R5 measures it.

Every R3 acceptance item is stated over decoded values and measured noise, and this spread is
far below both noise measures above, so it does not block R3 and the capture set is accepted.

It is recorded because it directly contradicts a later requirement: **R5's acceptance demands
deterministic capture hashes at frames 1, 32, 128 and 256 across at least three repeated runs.**
That gate cannot pass while identical inputs yield different bytes. R5 must either identify and
remove the source of the variance, or replace its deterministic-hash acceptance with an explicit
tolerance derived from a measurement — a decision to make in R5, with evidence, not by quietly
loosening the gate.

##### Artifact retention

The accepted artifact directory is **151 MiB** and is deliberately **not** added to ordinary Git
history; `tmp/` is ignored. Nothing in the repository reproduces those bytes — the PFMs are the
reference. **`tmp/r3-references/20260922-212524` must be preserved and backed up separately**
(external drive or archive), and the six canonical hashes above are the only in-repo record that
can prove a restored copy is the accepted set. If that directory is lost, R3 must be re-run and
every comparison made against it is void.

### R4 — Resolve lighting ownership and brightness

Instrument, do not guess. Separately capture direct sun diffuse, diffuse environment, indirect
diffuse, specular environment, emissive, and fallback classifications. Determine whether the
known brightness mismatch is duplicate ownership, lost environment, albedo misuse, fallback
misclassification, temporal darkening, or a combination.

Acceptance:

- each contribution has one owner and documented units;
- removing any single owner produces the predicted delta;
- whole-image and deep-interior metrics are both reported;
- for matched references, mean ratio is initially within 0.8–1.2 and signed bias/RMSE improve
  over the recovered baseline; and
- no scalar fudge is introduced without a physical derivation and cross-scene validation.

### R5 — Replace blocking radiosity synchronization

Design explicit GPU-to-GPU ordering and stable ping-pong resource ownership. Avoid a full-atlas
per-frame CPU-blocking submission. Test captures at frames 1, 32, 128, and 256 across at least
three repeated runs.

Acceptance:

- deterministic capture hashes at each checkpoint;
- no race, validation message, or stale read;
- radiosity-on overhead is no more than 0.5 ms over radiosity-off in the living-room Balanced
  test, unless the owner approves a revised hardware-backed budget;
- no recurring Sponza movement hitch above section 8.1; and
- output remains within the accepted correctness tolerance.

### R6 — Rebaseline and decide

Record the full scene matrix, feature-off equivalence, static and movement tails, and memory.
Only then decide whether Balanced Lumen-lite becomes a default candidate. Any failed required
gate leaves it experimental and opt-in.

## 11. Gate for future features

After R1–R6, a future feature must receive a fresh frozen experiment record. It must include:

- a causal reason the accepted baseline cannot meet a stated goal;
- the smallest testable architectural change;
- comparison to the shipped baseline;
- a quality metric resistant to gaming;
- whole-frame and pass timing;
- memory and movement-tail budgets;
- feature-off equivalence;
- cross-scene validation; and
- a one-command rollback.

Do not stack experimental systems. Probes may not depend on unaccepted HZB behavior; a new world
field may not land while its brightness semantics are unknown.

## 12. Change-control and safety checklist

Before code:

- clean or explicitly account for the worktree;
- create a named branch;
- save baseline hashes and raw outputs;
- confirm Release shaders match the executable; and
- post the frozen experiment record.

During work:

- run the smallest safe test first;
- keep GPU workloads visible and foreground;
- stop after device instability;
- preserve failed results;
- do not modify unrelated assets or `tmp/card5`; and
- do not push, fetch, merge, or rewrite history unless explicitly requested.

Before commit:

- inspect the full diff;
- run Debug and Release builds plus unit tests;
- run all frozen correctness/performance gates;
- post complete results;
- verify `git status --short`; and
- use no contributor/co-author attribution unless requested.

Rollback is a valid result, not a failure to deliver.

## 13. Implementation map

| Area | Primary location |
|---|---|
| Feature flags, frame scheduling, preset startup | `src/vk_engine.cpp` |
| SSGI/Lumen trace orchestration | SSGI renderer source and associated shaders |
| Surface cache and radiosity synchronization | `src/vk_engine_surface_cache.cpp` |
| Distance fields | `src/vk_engine_sdf.cpp` and distance-field shaders |
| Material debug modes | C++ render-mode enum and material/debug shaders |
| Test scenes/cameras | `assets/scenes`, benchmark scripts, and capture metadata |

Search current symbols before relying on line numbers; line numbers in historical reviews may
move.

## 14. Historical evidence that must not be reused as acceptance

- the post-hoc quadratic speckle metric;
- Debug-build probe timings;
- HZB comparisons against 128/256-step marches as proof of a shipped-path win;
- the claim that probes/HZB “cannot affect you” merely because their defaults are off;
- the nonexistent Sponza reference;
- the 3.8× deep-interior brightness ratio as a whole-scene result;
- distance-field bias alone as a clipmap acceptance criterion; or
- successful builds/tests as proof that the renderer runs correctly.

## 15. Definition of an accepted baseline

Lumen-lite is accepted only when all of the following are true:

1. R1–R6 are complete with linked raw artifacts.
2. All required scenes pass correctness, stability, performance, and memory gates.
3. Feature-off behavior matches the recovered baseline.
4. No unresolved validation errors, device hangs, non-finite pixels, or hidden fallbacks remain.
5. The implementation and this document describe the same equations, presets, and ownership.
6. A reviewer can reproduce each result from recorded commands without relying on chat history.

Until then, describe Lumen-lite accurately: a promising experimental GI path with a viable
Balanced performance point, unresolved radiometric correctness, and several isolated prototype
features that are not part of the accepted architecture.
