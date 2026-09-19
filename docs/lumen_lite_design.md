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
| Plain SSGI, preset 0 | 960×540 | 3.779 | 2.345 | 1.991 |
| Lumen-lite, preset 0, radiosity off | 960×540 | 29.063 | 27.617 | 27.257 |
| Lumen-lite, preset 0, radiosity on | 960×540 | 31.018 | 27.590 | 27.215 |
| Lumen-lite, preset 2, radiosity off | 480×270 | 5.154 | 3.844 | 3.643 |
| Lumen-lite, preset 2, radiosity on | 480×270 | 6.968 | 3.868 | 3.664 |

Interpretation:

- Preset 0 is a validation mode, not a reasonable default on this GPU.
- Balanced preset 2 is the only currently measured default candidate.
- At preset 2, enabling radiosity adds about 1.81 ms of whole-frame cost while the reported
  SSGI and trace timings barely move. The known blocking atlas transfer is the leading cause
  to verify, not a settled attribution.
- These numbers are one scene and one camera. They do not establish cross-scene acceptance.

### 2.2 Confirmed correctness and engineering debts

- `MIRABILIS_LUMEN_LITE`, `MIRABILIS_SSGI_PROBES`, and `MIRABILIS_SSGI_HZB`
  currently test variable presence. Setting one to `0` still enables it.
- The material-debug shader constants are two positions out of sync with the C++ enum.
- The Cornell ceiling emitter geometry faces upward into the ceiling. A guard masks a symptom;
  the path tracer's absolute cosine can hide the orientation error.
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

- startup log states preset 2 and 480×270 at the measured output resolution;
- explicit preset overrides still reproduce their table;
- Lumen-lite remains opt-in until correctness gates pass;
- living-room performance stays within section 8.1; and
- plain SSGI behavior is unchanged.

### R3 — Establish matched references

After the emitter fix, regenerate safe Cornell and living-room references with complete metadata.
Do not change renderer behavior during this task. If a safe reference cannot be produced, mark
the affected gate blocked.

Acceptance:

- files exist and can be decoded;
- parity fields are complete;
- repeated reference seeds quantify noise;
- masks/crops are stored before candidate comparison; and
- orientation is verified.

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
