# Lumen-lite recovery handoff

Last updated: 2026-09-19

The authoritative specification is [lumen_lite_design.md](lumen_lite_design.md). Read it before
changing renderer code. This handoff is only a current-state index and does not override its
gates.

## Repository state

- Active branch: `main`
- Active commit: `6d994b4`, the revert of distance-field clipmaps
- Active tree: identical to `adc87e3`
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
| Plain SSGI, preset 0 | 960×540 | 3.779 | 2.345 | 1.991 |
| Lumen preset 0, radiosity off | 960×540 | 29.063 | 27.617 | 27.257 |
| Lumen preset 0, radiosity on | 960×540 | 31.018 | 27.590 | 27.215 |
| Lumen preset 2, radiosity off | 480×270 | 5.154 | 3.844 | 3.643 |
| Lumen preset 2, radiosity on | 480×270 | 6.968 | 3.868 | 3.664 |

Preset 0 is a diagnostic mode. Preset 2 is the performance candidate. It is not yet a
correctness-accepted default.

## Next task: R1 only

Do not start probes, HZB, clipmaps, or a brightness adjustment. Complete the specification's
R1 task:

1. Fix boolean environment parsing so `=0` is disabled.
2. Align material-debug constants with the C++ enum.
3. Correct the Cornell emitter orientation/sidedness at the source.
4. Add focused tests and verify Debug and Release builds.

Before editing, create a branch and post the frozen experiment record from the specification.
After R1, stop and report every gate as PASS, FAIL, or BLOCKED. Do not proceed automatically to
R2.

## Known hazards

- Never run the Sponza path tracer on this laptop.
- Never start a GPU workload in the background or without stating it first.
- Removing a boolean environment variable disables it; setting the current presence-based flags
  to `0` does not.
- A successful build does not validate runtime behavior.
- No valid Sponza path-traced reference exists.
- The Cornell reference must be regenerated after fixing its ceiling emitter.
- Surface-cache radiosity currently performs a blocking full-atlas copy each Lumen-lite frame.

## Required sequence

`R1 diagnostics → R2 coherent Balanced default → R3 matched references → R4 lighting ownership
→ R5 nonblocking radiosity → R6 complete rebaseline`

Stop at the end of each item for review. Preserve raw logs and failed results.
