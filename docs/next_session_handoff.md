# Lumen-lite recovery handoff

Last updated: 2026-09-23

The authoritative specification is [lumen_lite_design.md](lumen_lite_design.md). Read it before
changing renderer code. This handoff is only a current-state index and does not override its
gates.

## Repository state

- R3 implementation branch: `recovery/r3-matched-references`
- R1 complete: boolean flags, debug-view constants, Cornell emitter sidedness
- R2 complete: Balanced (preset 2) is the implicit startup default, applied through
  `apply_ssgi_quality_preset()`; the preset table is the only source of the derived settings
- R3 complete: matched Cornell and living-room references captured at
  `89b5e7794d48592d901b69dd7e8290bbb1a50d8a` (clean tree), seeds 1337/2026/90210, 512 samples
  per seed, 0 failed and 0 blocked. Record in
  [lumen_lite_design.md](lumen_lite_design.md) section R3.2.
- Preserved rejected tip: `recovery/lumen-experiments` at `4241d01`
- Clipmaps: reverted
- Screen probes: experimental and disabled
- HZB screen tracing: experimental and disabled
- Existing unrelated local asset state, including `assets/scenes/.last_scene`, belongs to the
  user and must not be changed or committed.

Do not recreate the old clipmap patch on `main`. Do not use the earlier handoff or prompt
wording from Git history; it encouraged architecture work before the baseline was trustworthy.

## Verified living-room measurements

Release build, RTX 3060 Laptop GPU, camera
`4.740 1.700 -2.947 -0.133 -14.670`, 295 measured samples:

| Configuration | Extent | Frame ms | SSGI GPU ms | Trace GPU ms |
|---|---:|---:|---:|---:|
| Plain SSGI, explicit preset 0 (pre-R2 implicit default) | 960×540 | 3.779 | 2.345 | 1.991 |
| Lumen preset 0, radiosity off | 960×540 | 29.063 | 27.617 | 27.257 |
| Lumen preset 0, radiosity on | 960×540 | 31.018 | 27.590 | 27.215 |
| Lumen preset 2, radiosity off | 480×270 | 5.154 | 3.844 | 3.643 |
| Lumen preset 2, radiosity on | 480×270 | 6.968 | 3.868 | 3.664 |

Preset 0 is a diagnostic mode. Preset 2 is the performance candidate. It is not yet a
correctness-accepted default.

## Next task: R4 only

R1, R2 and R3 are complete. Do not start probes, HZB, clipmaps, or R5's radiosity work.

**First R4 task: add the auditable unattended control for `traceEnvironmentMap=false`.** Nothing
else in R4 can begin before it. R3.1 froze the parity rule — a candidate compared against an R3
reference must run with `traceEnvironmentMap` false so both paths light ray misses from the
analytic gradient — but no environment variable sets it today. Until the control exists, every
candidate-versus-reference comparison is diagnostic only and cannot discharge a gate. Auditable
means the capture sidecar records the value actually used, so a comparison can be checked after
the fact rather than trusted.

Then, and only then, the rest of R4: instrument contributions separately — direct sun diffuse,
diffuse environment, indirect diffuse, specular environment, emissive, and fallback
classification — and determine from that instrumentation whether the known brightness mismatch
is duplicate ownership, lost environment, albedo misuse, fallback misclassification, temporal
darkening, or a combination.

**No brightness scaling and no renderer tuning before the contribution instrumentation exists.**
Not a scalar, not `ambientRetention`, not a re-rendered reference. Fitting a multiplier to close
a gap whose cause has not been measured destroys the evidence that would have identified it, and
is rejected by R4's acceptance regardless of the numbers it produces.

### R4 must derive and freeze its uncertainty method first

The references carry two empirical noise estimates, and **neither is a statistical bound**.
Pixel-level noise (living-room indirect 19.8838% whole, 17.0737% deep interior; Cornell indirect
6–9%) is the mean of per-pixel sample standard deviations — relevant to pixelwise metrics such
as MAE and RMSE, but not a limit on either. Region-mean spread (0.9441% and 0.0378% for those
same living-room regions; every Cornell indirect region below 0.076%) is a sample standard
deviation over **three** region means — a repeatability measurement, not a confidence interval.

Do not use either as a tolerance, and in particular do not read the 17–20% figures as an
uncertainty in a region-level mean ratio: they are not, and doing so would discard real findings
as noise. For each R4 metric, derive its uncertainty from the reference's own variation — for
example by evaluating that metric per seed and against the ensemble, or by a bootstrap or
confidence procedure over the per-seed captures — and freeze that derivation and the pass/fail
rule in writing before viewing any candidate result. Full table and reasoning in R3.2.

## Reference artifacts — back these up

The accepted capture set is `tmp/r3-references/20260922-212524`, 151 MiB, **not in Git**
(`tmp/` is ignored) and not reproducible from the repository. Preserve and back it up
separately. Canonical references — R4 uses the indirect files directly:

```
E38AB084D90B9B23D5B667189099B5F480B3F303EB26E59F19FEDE9DB6025663  cornell-reference.direct.pfm
579E3A3D7867D8CB6F51D6950294A7FEF9319B1FEAC9E5469D23418485BA9A26  cornell-reference.indirect.pfm
DCF02720C1328019904539A1AD6DA536D251D10E5FDE6634B999443BE3E7ACA1  cornell-reference.pfm
B70CDE1D4DC159DA8D6FC9DFD2268B6AE661C24D5C3069843F8FAE530CFB15FB  living-room-reference.direct.pfm
9E062708263D99BE5FD65A6875851CAF555A1299DEC02372446429CDD15B86B5  living-room-reference.indirect.pfm
64FFE8792C23B48E7C0243AC3D27DC8AB50FE6145F7817C37CF7EDE842BAA2E6  living-room-reference.pfm
```

## Known hazards

- Never run the Sponza path tracer on this laptop.
- Never start a GPU workload in the background or without stating it first.
- `MIRABILIS_LUMEN_LITE`, `MIRABILIS_SSGI_PROBES` and `MIRABILIS_SSGI_HZB` parse `0`/`false`/
  `off`/`no` as disabled since R1. Every other `MIRABILIS_*` variable is still presence-based,
  so writing `=0` for one of those enables it.
- Run-to-run frame-time spread is about 0.34 ms at 3.8 ms. A single run cannot resolve a
  0.5 ms gate.
- A successful build does not validate runtime behavior.
- No valid Sponza path-traced reference exists.
- Two runs with the same engine binary, shader manifest, scene manifest and seeds produced
  15 of 18 raw PFMs differing: mean absolute difference about 1e-9 to 3e-8, maximum 0.0017.
  (The two runs were at different commits, `742c0a6` and `89b5e77`; the intervening changes were
  capture tooling, not renderer code.) The cause is unexplained. Below the measured noise, so it
  did not block R3 — but R5's acceptance requires deterministic capture hashes and cannot pass
  until this is explained or that gate is deliberately revised. See R3.2.
- Surface-cache radiosity currently performs a blocking full-atlas copy each Lumen-lite frame.

## Required sequence

`R1 diagnostics ✓ → R2 coherent Balanced default ✓ → R3 matched references ✓ → R4 lighting
ownership (next) → R5 nonblocking radiosity → R6 complete rebaseline`

Stop at the end of each item for review. Preserve raw logs and failed results.
