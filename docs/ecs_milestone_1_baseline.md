# Milestone 1 baseline

Pinned base: `5486a56d19eea7d37fb248b10936a8ba77302a5b`.
Initial checkout: `recovery/r6-validation` at `625d32b`; clean.
Local main: `848dd37`; origin/main: pinned `5486a56`. Main is untouched.
Isolated branch/worktree: `feature/ecs`, `C:/dev/mirabilis-work/ecs`.
Build: `C:/dev/mirabilis-work/build-baseline`.

Behavior-neutral instrumentation commits: `c7863a7`, `5da889d`.
Raw evidence and baseline binaries: `C:/dev/mirabilis-work/baseline`.
Build logs: `C:/dev/mirabilis-work/baseline-build-{debug,release}.log`.

Debug and Release engine/shaders built with Visual Studio 18 2026, MSVC
14.51.36231, CMake 4.3.1 and Vulkan SDK 1.4.357.0. All 170 Python tests pass
with inherited `PSModulePath` cleared so Windows PowerShell loads its own modules.
The initial uncorrected shell run had 159 tests, one failure and one setup error
because its metadata writer could not find Get-FileHash. Both results are retained.

All 23 existing scene collision sets were dumped by the baseline engine before
test portal placement, with authored pairs loaded. Fixtures contain ordered wall
AABBs, ground planes and ramps. Float comparison: absolute 1e-5 plus relative 1e-6.
Authored JSON and `.last_scene` stayed unchanged.

Debug cycle/minimize/restore (20 frames), scene switch from lighting calibration
to Sponza (100), portal traversal (200), and portal placement (180) completed with
exit 0 and zero Vulkan validation errors, VUIDs or harness failures. The existing
NVIDIA loader warning about the layer manifest version remains recorded.
Ray-query shadows were available and enabled. Sponza path tracing was not run.

## Frozen timing protocol and results

Release, validation off, 1600x900, scene settings without renderer/GI overrides,
hidden panels, frame cap disabled; MAILBOX requested with FIFO fallback. Timing
is steady-clock loop start to start, including the preceding present/wait.
120 warm-up frames then exactly 600 focused samples, three runs per scene.
GPU: RTX 3060 Laptop, Windows driver 32.0.15.5597 (NVIDIA 555.97); Balanced power.

| Scene | Camera (xyz pitch yaw) | Three mean frame times (ms) | Median (ms) | Spread | Allowed slowdown |
| --- | --- | --- | --- | --- | --- |
| sandbox.json | 0 2.5 12 -0.08 0 | 1.954, 1.925, 1.869 | 1.925 | 4.416% | 5% |
| sponza_showcase.json | 0 2.5 12 0 0 | 4.439, 3.720, 4.258 | 4.258 | 16.886% | 33.772% |

Sponza's allowed slowdown is twice its baseline spread, as required by the
implementation specification. Its high variance limits precision of the comparison.
