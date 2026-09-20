# Lumen-lite recovery handoff

Last updated: 2026-09-19

The authoritative specification is [lumen_lite_design.md](lumen_lite_design.md). Read it before
changing renderer code. This handoff is only a current-state index and does not override its
gates.

## Repository state

- Active branch: `main`
- Active commit: R2, on top of `bac9b45` (R1) and `6d994b4` (the clipmap revert)
- R1 complete: boolean flags, debug-view constants, Cornell emitter sidedness
- R2 complete: Balanced (preset 2) is the implicit startup default, applied through
  `apply_ssgi_quality_preset()`; the preset table is the only source of the derived settings
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

## Next task: R3 only

Do not start probes, HZB, clipmaps, or a brightness adjustment. R1 and R2 are complete.
Complete the specification's R3 task:

1. Regenerate the Cornell reference now the ceiling emitter faces the room.
2. Regenerate the living-room reference with matched environment settings.
3. Record every parity field in section 7.1 beside each capture.
4. Repeat reference seeds to quantify noise, and store masks and crops before comparing.

Do not change renderer behavior during R3. If a safe reference cannot be produced, mark the
affected gate blocked. Sponza reference generation stays forbidden on this laptop.

Before editing, create a branch and post the frozen experiment record from the specification.
After R3, stop and report every gate as PASS, FAIL, or BLOCKED. Do not proceed automatically to
R4.

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
- The Cornell reference must be regenerated after fixing its ceiling emitter.
- Surface-cache radiosity currently performs a blocking full-atlas copy each Lumen-lite frame.

## Required sequence

`R1 diagnostics → R2 coherent Balanced default → R3 matched references → R4 lighting ownership
→ R5 nonblocking radiosity → R6 complete rebaseline`

Stop at the end of each item for review. Preserve raw logs and failed results.
