# Mirabilis Milestone 2 report

Milestone 2 implements Jolt rigid bodies, editor authoring and v2 persistence,
Play lifecycle and snapshots, transform ownership, interpolation and motion
history, dynamic descendant handling, and shared static-cache eligibility.
The falling-box demo is `assets/scenes/physics_drop_demo.json`. C# and Milestone 3
are outside this change. Final tested source is `0bc66b0`; every required
post-review gate passes and the independent reviewer approved it. All three
blocking findings are resolved. Initial pre-review measurements are explicitly
retained separately below. No required check remains blocked or unrun.

## Repository, scope and contracts

The user selected CURRENT MILESTONE 2, overriding the specification's stale 1.
The complete specification was read before edits. No applicable `AGENTS.md` was
found in the repository or checked ancestors. The reviewed M1 commit is
`6497ec16d60989401a8c20b4a4839aec9d3eca70`. At initial inspection, `main`,
`origin/main` and `feature/ecs` all pointed to that commit; the original checkout
was clean. The original M1 report's historical main/push statements were not used
as current state. Original baseline `5486a56d19eea7d37fb248b10936a8ba77302a5b`
and its evidence remain intact.

Implementation uses `feature/physics` in `C:/dev/mirabilis-work/physics`.
Workers have separate `physics-editor`, `physics-gameplay` and
`physics-validation` worktrees. Builds are in `build-physics` and
`build-physics-core-only`, all under `C:/dev/mirabilis-work`, outside OneDrive.
The ECS worktree was used for read-only baseline runs, not implementation. Its
preexisting `.last_scene` change was preserved. Local integration merges were
made on `integration/physics` before the combined, building group was merged
onto `feature/physics`. There was no push or merge into main.

Commit `8079e04` is the mechanical player-controller naming change, before Jolt:
`update_player_movement`, `PlayerStepState`, `PlayerMovementDt` and
`MaxPlayerMovementSteps`; the original 1/120 second, eight-step clock remains.
Commit `40232f5` freezes shared components, APIs, body/lifecycle, transform,
interpolation and eligibility contracts before workers start. Later shared
declarations and contract refinements were lead-owned and communicated to their
owners. See `docs/physics_contracts.md`.

## Actual team and dependencies

The inherited lead handled Jolt, components, Scene, engine header/main, CMake,
demo, integration and this report. Its model label is not exposed by the tools;
the requested lead model could not be independently selected or verified.
The three implementation roles were explicitly selected as **GPT-6.1 Sol,
High reasoning**, with separate worktrees and one owner per file:

- Gameplay/render: player/portal filters, emission, interpolation and cache consumers.
- Editor/scene: inspector, engine/core serialization and scene-format documentation.
- Validation: tests, fixtures, runners and evidence, written from the specification
  and contracts without reading the physics implementation.

Fresh independent reviewer: **GPT-6 Astra, High**, selected without implementation
conversation history after workers retired; final verdict is **Ship** at `0bc66b0`.
Four-agent
concurrency was respected. The selected worker models were supported; only the
inherited lead label uses the documented reporting fallback.

New dependency: **Jolt Physics v5.6.0**, MIT, tag commit
`e77f175595e64cb44218cc9d9d56fc365ad0e36a`. Upstream `Jolt/`, `Build/` and license
are vendored, statically linked, with no configure-time download or runtime
DLL. Download archive SHA-256:
`0af9beea51637ef805e624fe838ea2870f7b68cd48cbe1615c853bd9bcf4f1d7`.
Jolt profiler, debug renderer, compute backends, install and LTO are disabled;
MSVC runtime matches the engine. Upstream AVX2/SSE/FMA defaults are retained.
The wrapper uses a single-thread job system and 16 MiB temporary allocator.
Existing EnTT v3.15.0 (MIT), doctest v2.4.12 (MIT), simdjson v3.3.0
(Apache-2.0 or MIT), and renderer dependencies are unchanged. Licenses and source
provenance are in `third_party/README.md` and the vendored directories.

Validation-only SDF tools were checked from installed distribution metadata and
license files: NumPy 2.5.3 (BSD-3-Clause AND 0BSD AND MIT AND Zlib AND CC0-1.0),
trimesh 5.1.0 (MIT), mesh-to-sdf 0.0.15 (MIT), PyOpenGL 3.1.0 (BSD),
pyrender 0.1.45 (MIT), and SciPy 1.17.1 (BSD-3-Clause with bundled notices).
They are not new engine runtime requirements. The existing bake script and
license/source provenance are recorded in the initial evidence directory's
`bake-license-provenance.json`; the post-review bake inventory has the same versions.

## Build, demo and repeatable checks

Test host: Windows, Visual Studio 18 2026, MSVC 19.51.36252, CMake 4.3.1,
Vulkan SDK 1.4.357.0, Python 3.12, NVIDIA RTX 3060 Laptop,
driver 32.0.15.5597 (555.97), Balanced power. Run from the physics worktree:

```powershell
cmake -S . -B C:/dev/mirabilis-work/build-physics -G "Visual Studio 18 2026" -A x64
cmake --build C:/dev/mirabilis-work/build-physics --config Debug --parallel 4
cmake --build C:/dev/mirabilis-work/build-physics --config Release --parallel 4
ctest --test-dir C:/dev/mirabilis-work/build-physics -C Debug --output-on-failure
ctest --test-dir C:/dev/mirabilis-work/build-physics -C Release --output-on-failure
cmake -S . -B C:/dev/mirabilis-work/build-physics-core-only -G "Visual Studio 18 2026" -A x64 -DMIRABILIS_BUILD_ENGINE=OFF
cmake --build C:/dev/mirabilis-work/build-physics-core-only --config Debug --parallel 4
ctest --test-dir C:/dev/mirabilis-work/build-physics-core-only -C Debug --output-on-failure
Push-Location ./bin/Release
$env:MIRABILIS_TEST_SCENE = "physics_drop_demo.json"
./engine.exe
Remove-Item Env:MIRABILIS_TEST_SCENE
Pop-Location
```

The demo starts in Play; Tab enters Edit to inspect all five RigidBody fields. The
orange box is a root Dynamic with mass 2, above a static GroundPlane floor and
static box obstacle. Its native body count is 3. Reset Play and leaving Play
restore authored state. Sandbox has 7 static bodies. Save only to a scratch path
when experimenting with authored fields. Builds, shaders and native dependencies
stay outside OneDrive with this worktree.

Python subprocesses need case-insensitive removal of this host's inherited
PowerShell 7 `PSModulePath` so Windows PowerShell finds `Get-FileHash`:

```powershell
python -c "import os,subprocess,sys; environment={k:v for k,v in os.environ.items() if k.upper()!='PSMODULEPATH'}; sys.exit(subprocess.call([sys.executable,'-m','unittest','discover','-s','tests'],env=environment))"
python tests/physics_gate.py --repo C:/dev/mirabilis-work/physics --output C:/dev/mirabilis-work/recheck-physics
python tests/ecs_gate.py gate-validation --repo C:/dev/mirabilis-work/physics --output C:/dev/mirabilis-work/recheck-ecs
python tests/ecs_gate.py collisions --repo C:/dev/mirabilis-work/physics --output C:/dev/mirabilis-work/recheck-collisions
python tests/ecs_gate.py timings --repo C:/dev/mirabilis-work/physics --output C:/dev/mirabilis-work/recheck-timings --baseline C:/dev/mirabilis-work/baseline
```

Use fresh output directories. GPU runs must be sequential. The physics runner
scratch-copies scenes, adds a glTF child with dormant Static collider/body data,
runs an Edit preflight and uses the existing real mesh SDF bake pipeline:
`python scripts/bake_sdf.py --queue assets/sdf/queue assets/sdf/cache`.
The ignored generated volumes are runtime assets, not synthetic test fixtures.
Bake dependencies, source hashes and cache hashes are recorded by the runner.

## Implementation behavior

Every enabled collider outside a Dynamic subtree creates a default static body.
Box geometry is oriented and honors signed scale and collider-center offsets;
GroundPlane is a thin box with its top at authored height; SurfRamp is the
existing wedge as a convex hull. Dynamic requires an enabled root Box. Static
settings with collision disabled persist without a body. Known values and
combined edits are validated before mutation or replacement. Parented Dynamic
files detach in staging with exact representable world TRS and ordered-root
append; shear rejects transactionally. Static shear uses the existing best-fit
decomposition with inspector/log warning.

Bodies reconcile in Edit without stepping. During Play, Dynamic poses are owned
by physics; public transform writes queue teleports, preserving velocity and
waking the body. Queued commands can target a Play-created component before its
first body flush. Scale and settings rebuild native shapes; every representable
static translation and scale edit is honored. Teardown callbacks retain readable
native bodies until body removal immediately before ECS destruction. Epoch
replacement/reset discards old bodies and queued commands and rebinds new handles.
Snapshots restore optional body settings and Play-created/deleted entities.

Rendering interpolates position/quaternion, including all descendant local
transforms. Creation, teleport, restore and shape rebuild reset interpolation
and primitive/glTF motion history for the subtree. Ordinary movement keeps
history; sleep collapses endpoints to the authoritative pose. Descendant saved
collider/Static settings remain dormant: no compound collider, independent native
body or player collision. One emitted eligibility field, including every glTF
draw, feeds SDF, surface cache and ray-query sun shadows. Authored dynamics are
eligible in Edit, excluded awake in Play and included asleep. Raster's auxiliary
trace/cache lighting uses that same static subset. SoftwarePathTrace includes
all renderables and resets accumulation/rebuilds on motion; its cost is measured.

The original player AABB controller, surf/bhop and portal traversal remain.
Dynamic subtrees are excluded from player collision and portal placement.
Authored portal-host ancestry is protected against becoming Dynamic/reparenting
under Dynamic; invalid authored portal files fail before replacement.

## Initial gate and comparisons (retained before reviewer fixes)

Initial pre-review source: `6d87a3bbc98d405fb489d14d1d97cb0cdb103ab0`.
Debug engine SHA-256:
`b04ef31dc708f9126955558f512baa99eb5cb752be8487e1a89eed7af6e7198a`.
Release engine SHA-256:
`f805fe3462baf20c6b5b65cf25a51f51a8d29bdc1c0bafeaeb59274370ded4e9`.

| Required check | Baseline | Initial candidate result |
| --- | --- | --- |
| Debug and Release builds | Reviewed M1 builds retained | PASS, both initial candidate builds |
| Headless core | M1 26 cases / 158,497 assertions | PASS, 43 cases / 162,243 assertions in Debug and Release; core-only Debug also passes without Vulkan discovery |
| Python | Fresh reviewed M1 173 | PASS, 177 with normalized environment |
| Original v1 scenes / v2 scratch round trips | 23 frozen collision goldens | PASS in core and initial 23 engine dumps; counts/order identical and maximum numeric delta 0 |
| All prior cycle/scene/portal/ECS GPU checks | Fresh M1 five cases PASS, zero errors | PASS, all five initial cases exit 0, no fatal/Vulkan/VUID lines |
| Physics counts, creation/deletion, deferred step removal, replacement | New M2 requirements | PASS headless tests |
| Reset/leaving-Play snapshots incl. created/deleted entities | New M2 requirements | PASS headless tests and initial raster/reference lifecycle runs |
| Shapes, offset sync, floor contact, static shear warning | New M2 requirements | PASS headless tests |
| Root validation, staged detachment/save/reload, transactional shear rejection | New M2 requirements | PASS headless tests |
| Portal-host guards, valid protected save/reload, player/static parity | Existing player baseline plus M2 restrictions | PASS headless and initial engine portal repeat |
| Dirty parent/Edit flush, tiny translation/scale, signed scale | New M2 requirements | PASS headless tests |
| Descendant interpolation/eligibility and no independent collision | New M2 requirements | PASS headless and initial glTF GPU cases |
| Teleport/history reset, sleep/wake, epoch/rebinding | New M2 requirements | PASS headless and initial GPU lifecycle/counter checks |
| Demo + glTF raster cache rebuilds / visibility / validation | New M2 requirements | PASS, both initial 1080-frame raster cases, no fatal/Vulkan lines |
| Short + full SoftwarePathTrace lifecycle and cost | New M2 requirements | PASS initial 200- and 1080-frame reference cases |
| Scratch edit/save/reopen and sandbox body count | New M2 requirements | PASS headless and initial engine lifecycle runs |
| Unchanged restoration does not rebuild caches | New M2 requirements | PASS in initial raster and full reference cases |
| Matched performance | Original frozen protocol and thresholds | PASS, six valid initial runs, 600/600 focused samples each |
| Fresh independent reviewer | Required initial gate | Three reproduced P2 blockers found; resolved and revalidated below |

The 17 initial physics cases also exercise duplication, dormant serialization,
invalid/stale/pending command targets, finite trust-boundary inputs, negative
gravity, invalid known blocks and immediate commands for a new Play body.
No original tests were removed. Core-only configuration has no Vulkan find/link.

Before behavior changes, the reviewed M1 binary was rerun independently. Its
fresh CTest Debug/Release, Python 173, five Debug GPU cases and all 23 collision
dumps passed; maximum collision numeric difference was 0. Baseline source and
binary hashes are retained under `physics-baseline`. The original specification
baseline under `baseline` remains the primary performance reference.

Timing protocol: Release, validation off, 1600x900 window, saved renderer/GI,
hidden panels, fixed cameras, uncapped MAILBOX request with FIFO fallback,
120 warm-up then 600 focused measured frames, three runs per scene. Interval is
steady-clock frame-loop start to start including preceding present/wait.
Sandbox camera `0 2.5 12 -0.08 0`; Sponza `0 2.5 12 0 0`. Spread is
(largest mean - smallest mean) / median. No rendering preference change was
introduced; the user's Auto Exposure off / exposure 4.0 session preference was
not converted into an unrelated source or authored-scene change.
Original and fresh M1 performance logs use plain SSGI preset 2 at 600x338,
with average radiosity GPU cost 0.000 ms. The five real SDF bake volumes prepared
for the separate Lumen physics gate do not participate in this saved timing mode.

| Scene | Original means/median ms | Original spread / allowed regression | Fresh reviewed M1 means/median ms | Initial means/median/spread/change |
| --- | --- | --- | --- | --- |
| sandbox | 1.954 / 1.925 / 1.869; median 1.925 | 4.416%; 5%, maximum 2.02125 ms | 1.765 / 1.881 / 1.783; median 1.783, spread 6.506% | 1.997 / 1.841 / 1.847; median 1.847, spread 8.446%; 4.05% faster than original, 3.59% slower than fresh M1 |
| Sponza | 4.439 / 3.720 / 4.258; median 4.258 | 16.886%; twice spread 33.772%, maximum 5.696 ms | 4.193 / 3.671 / 4.233; median 4.193, spread 13.403% | 4.010 / 4.112 / 3.846; median 4.010, spread 6.633%; 5.82% faster than original, 4.36% faster than fresh M1 |

Both primary thresholds pass. Supplemental comparison also passes the same
spread rule using fresh M1: Sandbox's 6.506% baseline spread allows 13.012%
regression; Sponza's 13.403% allows 26.807%. These noisy host measurements do
not isolate Jolt's causal cost. All initial samples are focused, saved settings
match (plain SSGI preset 2, radiosity/exposure GPU cost 0.000), and no other
engine/build/test process ran concurrently. A delayed external launcher allowed
the Computer Use API to activate each returned engine window before warm-up;
six activations are recorded. The delay precedes engine launch, so it adds no
benchmark frame time or extra engine warm-up frames.

## Raster observations and reference cost

Initial normal raster, reference and SDF/surface diagnostic captures are complete.
All four main cases exited 0, retained the
selection marker, and had no fatal/Vulkan errors. Base elapsed time was 46.406 s;
the child case was 51.625 s. Body count was 3 in both, while emitted Dynamic
draws increased from 1 to 4 with the child. In cache-counter order
SDF/surface/Raster auxiliary trace/ray-query shadow, the values were:

| Phase | Base and child counters | Base eligible/SDF placed | Child eligible/SDF placed |
| --- | --- | --- | --- |
| Awake frame 30 | 1 / 1 / 2 / 1 | 2 | 2 |
| Sleep frame 176 | 2 / 2 / 3 / 2 | 3 | 6 |
| Wake frame 901 | 3 / 3 / 4 / 3 | 2 | 2 |
| Teleport frame 931 | 3 / 3 / 4 / 3 | 2 | 2 |
| Reset frame 961 | 3 / 3 / 4 / 3 | 2 | 2 |
| Edit restore frame 981 | 4 / 4 / 5 / 4 | 3 | 6 |
| Unchanged restore frame 991 | 4 / 4 / 5 / 4 | 3 | 6 |

Real SDF placed counts equal instance counts, with no missing requested bake.
Native and ECS sleep states agree. Teleport uses zero gravity/velocity, checks
the exact requested pose, and checks previous/current transforms for every
affected draw rather than accepting a visual old-pose streak. Ordinary falling
draws retain motion history. Scratch mass edit/save/reopen returns 3 bodies;
replacement with sandbox returns 7, with zero leftover Dynamic draws.

Normal awake/sleep captures use camera `0,2.5,12`, pitch -0.08, yaw 0, full draw
1200x675, render scale .75, Lumen-lite and ray-query sun shadows enabled. The
orange box is visible suspended and resting. It has no floor shadow while
awake; a small shadow to its right appears asleep. The blue static obstacle's
shadow remains in both. The floor's normal-image color is nearly uniform; this
capture alone does not isolate a colored bounce-light contribution.
The box's own indirect-radiance pixel region is nonzero in both states:
mean RGB awake `(0.187671, 0.243812, 0.338310)`, asleep
`(0.191362, 0.233649, 0.316017)`, 100% nonzero pixels and 0 nonfinite pixels.
Both whole-image captures also report 0 nonfinite pixels. These measurements
show received indirect light despite the absent awake cache representation.

The SDFTrace view (debug 28, real mesh source 1) shows only the floor and static
obstacle awake; asleep it adds the box at its resting pose. In SurfaceCache view
29, the awake atlas lacks orange box cards; asleep six orange cells appear in
the bottom row. Logs confirm 2 instances/12 cards awake, 3/18 asleep and 2/12
after wake. Both diagnostics use the same camera, exit 0, have no fatal/Vulkan
lines or nonfinite pixels, and preserve `.last_scene`. These views directly
verify the missing awake SDF and cache representation. The absence of a box card
prevents cache bounce from that box; a separately isolated colored bounce on
the demo floor was not discernible in the normal captures.
SDF diagnostic elapsed 46.906 s; surface diagnostic elapsed 42.062 s.

Capture pairs and measurements are retained in the initial evidence directory:
`physics/raster-demo/captures`, `physics/raster-child/captures`,
`physics/trace-child/captures`, `physics/trace-child-lifecycle/captures`,
`diagnostic-sdf/captures`, `diagnostic-surface/captures`,
`box-indirect-observations.json` and `raster-phase-counters.json`.

The implemented tradeoff is specific: awake Dynamic subtrees do not occlude
SDF-traced GI, have no surface-cache bounce cards, and cast no ray-query sun
shadow. They still render in raster, retain normal shadow-map draws and receive
screen-space indirect illumination at their own visible pixels. Actual captures
and enabled capabilities will be reported, rather than claiming blanket no GI.
AO was not enabled in the normal diagnostic protocol, so an AO visual result
must not be claimed. SoftwarePathTrace includes moving objects and its motion
rebuild/accumulation-reset cost is separate from static Release measurements:
the short 200-frame case took 264.813 s, ending with 59 accumulated samples,
maximum recorded GPU dispatch 12.2895 ms, zero nonfinite pixels and zero
direct+indirect sum residual. Its 142 nonempty scene rebuilds averaged
1592.64 ms (1541.1-1707.2 ms), wall clock including device wait and the existing
4096-ray Debug CPU BVH/brute-force oracle. The full 1080-frame lifecycle case
took 374.360 s, ended with 30 samples, maximum recorded GPU dispatch 16.8275 ms,
zero nonfinite pixels and zero component residual. These Debug costs are not
Release shipping-frame-time estimates. Same-camera awake reference images show
the orange box and gray glTF child together; motion repeatedly resets
accumulation, so the one-sample moving capture is visibly noisy. The full run
passes teleport pose/history, Reset, Edit/unchanged restore, save/reopen and
sandbox replacement checks with reference rendering active.

## Independent review

The fresh GPT-6 Astra High reviewer independently reproduced three blocking P2
findings against initial candidate `6d87a3b`:

1. A sleeping box remained suspended after its static support's parent moved
   away. Static changes now activate Dynamic neighbors within old/new contact
   bounds, expanded by Jolt's speculative contact distance. Distant unrelated
   sleepers stay asleep. Sleep metadata is refreshed after all static updates,
   including Dynamics earlier in hierarchy order, before rendering.
2. Reparenting accepted an enabled collider under a zero-scale parent, leaving
   native and ECS geometry mismatched and the scene unsavable. Shared collider
   subtree validation now checks proposed ancestry/world matrices before any
   hierarchy mutation, and also covers transform edits. Disabled-only collider
   subtrees remain valid where allowed.
3. Float asin decomposition rejected valid near-90-degree pitch TRS, preventing
   native body creation and staged Dynamic detachment. Exact TRS and physics
   writeback now use GLM's stable Y-X-Z Euler extraction in double precision.
   The existing best-fit gizmo decomposition remains unchanged.

The independent validator added five regression cases from these reported
contracts: five legal support-loss paths, immediate targeted native/ECS wake
metadata, unrelated sleepers, transactional collider-subtree reparent rejection,
dormant collider allowance, and root/native/staged/reloaded poses at both signs
of 1.5700, 1.5705, 1.5707 and pi/2 pitch. Old-binary regressions fail four cases;
the fixed Debug suite passes 48 cases and 170,520 assertions. One initially
requested test tried detaching the required Collider component; it was corrected
to legal disable/native/entity removal paths, with both logs retained.

The reviewer independently passed the original 43-case suite and audited all
411 initial evidence hashes, both binaries, 24 scenes, all 41 valid launches,
timing statistics and raster/reference lifecycle telemetry with no discrepancies.
Initial review/repros are in `C:/dev/mirabilis-work/physics-review`.
The original isolated reproductions and full independent 48-case Debug suite
pass against `6c72f26`; see `review-fixes-6c72f26.txt`. Complete post-review
validation below also passes on the new frozen binaries. Final independent gate
confirmation is **Ship** at `0bc66b0`, with no unresolved findings.
The reviewer independently recomputed all 400 final evidence hashes, both actual
engine hashes, all 24 scene hashes and the selection marker; audited all 41 raw
launch logs/results; rechecked lifecycle/cache telemetry; directly compared the
23 collision JSON payloads with both baselines; recomputed all six raw timings
and focused sample counts; and inspected the new raster-child, reference, SDF
and surface captures. It checked the report's corrected initial/final labels.
Approval and repeatable audit script/log are under
`C:/dev/mirabilis-work/physics-review/final-review-0bc66b0.txt`.
The reviewer ran its independent headless suite and repros, then audited the
validator's GPU evidence; it did not launch a second set of GPU runs itself.

## Post-review gate

The fixed group was built and tested on `integration/physics-review`, then merged
onto `feature/physics` at `0bc66b0ce0180db7d7088425aa39db108726c578`. Final frozen
Debug engine SHA-256:
`960ae76f9f2ebf492efc27e49db595550e6049bf5de9c3c79d5be00358a27a6a`.
Release SHA-256:
`43812cd88f0cda07ae5b2602974e561b36eaa7a464679b407f7775dc1f528da7`.

| Post-review check | Result |
| --- | --- |
| Debug/Release builds and CTest | PASS, 48 cases / 170,520 assertions |
| Headless Debug build/CTest | PASS, no Vulkan discovery/link |
| Python | PASS, all 177 with the documented environment correction |
| Reviewer original isolated repros + independent Debug suite | PASS against fixed code `6c72f26`, same code integrated at `0bc66b0` |
| Four physics GPU cases | PASS, final raster 1080/1080 and reference 200/1080 frames on final hashes |
| Two SDF/surface diagnostic captures | PASS, real mesh SDF and atlas awake/sleep/wake views |
| Five earlier GPU cases and 23 engine collisions | PASS, both original and fresh ECS collision comparison maximum delta 0 |
| Six matched timing runs and both baseline comparisons | PASS, 600/600 focused samples in all six runs |
| Final preservation manifest/evidence hashes | PASS, source, both binaries, all 24 scenes and selection marker unchanged |
| Independent final gate approval | PASS, fresh GPT-6 Astra High verdict Ship; no unresolved findings |

Post-review evidence is `C:/dev/mirabilis-work/physics-gate-m2-reviewed`;
build/core/headless/Python logs and review/repro evidence are under
`C:/dev/mirabilis-work/physics-review`. Initial evidence is retained separately.

All 41 post-review launches are valid passes with zero fatal/Vulkan errors and
zero invalid-focus attempts. The same renderer/camera/warm-up protocol and
prepared external focus workflow were used. Both diagnostics show the same
missing awake SDF box and atlas 12 -> 18 -> 12 cards, and the own-box indirect
measurement is reproduced exactly. Final runtime results:

| Case | Frames | Elapsed seconds | Result |
| --- | --- | --- | --- |
| Raster demo | 1080 | 45.375 | PASS |
| Raster glTF child | 1080 | 49.813 | PASS |
| Short reference child | 200 | 267.078 | PASS |
| Full reference lifecycle | 1080 | 365.718 | PASS |
| SDFTrace diagnostic | 1080 | 40.625 | PASS |
| SurfaceCache diagnostic | 1080 | 41.157 | PASS |

Static-cache counters and eligibility/body/draw counts reproduce the initial
phase table exactly, including unchanged restore. The final short reference
has 59 samples, maximum GPU dispatch 16.6089 ms and zero nonfinite pixels or
component residual; 142 nonempty rebuilds average 1595.97 ms
(1531.9-2082.5 ms) including the Debug oracle/device wait. Full reference ends
with 30 samples and maximum recorded GPU dispatch 14.4389 ms, with zero
nonfinite pixels/residual; its sleep capture has 709 samples and visibly retains
the orange box and gray child at rest. These are recorded costs, not causal
performance claims.

| Scene | Final means ms | Median ms | Spread | Original comparison | Fresh reviewed M1 comparison |
| --- | --- | --- | --- | --- | --- |
| sandbox | 1.827 / 1.878 / 1.863 | 1.863 | 2.738% | 3.22% faster, below 2.02125 ms: PASS | 4.49% slower, within fresh spread rule: PASS |
| Sponza | 3.665 / 4.350 / 3.906 | 3.906 | 17.537% | 8.27% faster, below 5.696 ms: PASS | 6.84% faster: PASS |

All six final measurements retain plain SSGI preset 2, 600x338, and average
radiosity/exposure GPU cost 0.000. Final source/input, effective resources,
focus activations, performance comparisons and evidence hashes are retained
under the post-review directory. The fresh reviewer independently approved
these results and the fixed source. No required gate remains blocked or unrun.

## Evidence and retained failed attempts

Final post-review evidence: `C:/dev/mirabilis-work/physics-gate-m2-reviewed`,
with final build/core/headless/Python and independent review logs in
`C:/dev/mirabilis-work/physics-review`. These are the approval evidence.

Initial pre-review build/test logs: `C:/dev/mirabilis-work/physics-final-build-*.log`,
`physics-final-core-*.log`, `physics-final-core-count.log`,
`physics-final-headless-*.log` and `physics-final-python.log`.
Initial pre-review GPU/performance evidence: `C:/dev/mirabilis-work/physics-gate-m2-final`.
Its `validation-summary.txt` is the concise complete gate record and
`evidence-sha256.json` hashes the retained evidence. The finalizer accounts for
44 launches: 41 valid launches pass; three invalid-focus timings are excluded.
`input-before.json` and `input-after.json` verify both frozen binaries, source
candidate, all 24 scene hashes and `.last_scene` unchanged. All original 23
collision dumps match both the original and fresh reviewed ECS baselines exactly.
Fresh reviewed baseline: `C:/dev/mirabilis-work/physics-baseline`.
Original baseline: `C:/dev/mirabilis-work/baseline`; M1 gate: `gate`.
Committed tests/fixtures, runner README and exact runner source copies accompany
raw logs, environment/binary hashes, scratch JSON, counter summaries and captures.

Earlier incomplete/failed attempts are retained under `physics-gate-gpu-first`,
`physics-gate-gpu-ready`, `physics-gate-gpu-final` and
`physics-gate-final-frozen`. They are not substituted for final passes. They
exposed telemetry-before-draw, absent real baked SDFs, Raster auxiliary-trace
rebuilds, and capture-prefix metadata overwrite; each was corrected. An earlier
child run had a transient eligibility outlier without sufficient diagnostics to
establish its cause; later source-copied runners assert native/ECS agreement.
Full trace runs cancelled for superseded candidates are explicitly cancelled,
not passes. One incremental link failed after overlapping builds; sequential
clean candidate builds succeeded. The initial inherited-environment Python
failure is preserved as `physics-final-python-inherited-env-failed.log`; the
normalized environment runs all 177 tests successfully.
Three initial pre-review timing attempts had invalid focus and were correctly rejected.
They are retained as `physics-gate-m2-final/timings-invalid-focus`,
`timings-invalid-focus-2` and `timings-invalid-focus-3`, excluded from
performance comparisons. The complete focused six-run replacement passes in
`timings`. No invalid mean is counted as a pass.

## Limitations

- Player and Dynamic bodies do not interact; Dynamic bodies hit solid portal
  host walls and do not traverse portals. Rotated walls retain the known player
  enclosing-AABB versus Jolt oriented-box mismatch.
- Descendant colliders are dormant saved data, not compound colliders. Static
  shear is approximated with a warning; Dynamic root/file rules prevent shear.
- Native simulation is single-threaded, with capacity 65,536 bodies/body pairs
  and 10,240 contact constraints. Capacity exhaustion is diagnostic. AVX2-capable
  x86-64 is the tested default build target; portability to older CPUs was not tested.
- Awake raster subtrees omit the specified static-cache contributions, including
  Raster auxiliary trace geometry. SoftwarePathTrace rebuilds moving geometry;
  Debug CPU trace-oracle validation is expensive and is not a shipping-frame-time measure.
- One Windows/NVIDIA host was tested. Actual selected present mode is not logged;
  matching requested policy is verified. Sponza's original variance limits timing precision.
- Manual mouse/keyboard coverage of every inspector panel is not claimed;
  authoring paths are checked through builds, core tests, GPU hooks and source review.
  Existing nonfatal NVIDIA layer-manifest and upstream doctest C5285 warnings remain.
- No C#, scripting API, compound colliders, player physics migration or Milestone 3.

## Local commits

Local commits through the final implementation candidate follow, including all
reviewer fixes. The final report commit is additional and identified in the completion
response. No commits were pushed.

- 8079e04 refactor: name player controller and fixed-step state explicitly
- 40232f5 docs: freeze Jolt lifecycle transform and cache contracts
- 25ab1dd refactor: declare per-entity glTF motion history contract
- fdb53f8 test: expose native body pose and deferred in-step validation hook
- b8adebe feat: integrate vendored Jolt bodies and fixed-step lifecycle
- 6bec2d0 refactor: declare per-entity glTF motion history contract
- 31070a9 feat: inherit physics interpolation and eligibility across dynamic subtrees
- ed03fad feat: count completed renderer cache rebuilds and trace cost
- 949a185 fix: retire portal host protections when replacing authored links
- bc6e95f merge: integrate physics gameplay interpolation and cache counters
- e42bc55 feat: add physics drop demo with authored rigid-body settings
- 040843e Persist rigid-body settings and validate staged physics scenes
- 36c3e47 Expose rigid-body authoring and physics edit diagnostics
- 2407e67 merge: integrate rigid-body inspector and staged serialization
- 6348923 test: expose native body pose and deferred in-step validation hook
- 153d0cf test: cover frozen physics lifecycle transform and serialization contracts
- 48457f0 test: exercise atomic scratch saving of physics demo
- af961ba test: cover dormant serialization duplication commands and dynamic portal hosts
- 42aaab6 test: assert physics GPU lifecycle cache counters and glTF child evidence
- d811b95 test: require active physics cache capabilities and unchanged edit restore
- 4019629 merge: integrate independent physics lifecycle and persistence tests
- 0d1ccad test: add bounded physics renderer lifecycle telemetry and captures
- 3b111ea fix: refuse in-step body removal and verify trace restore phases
- 00c182e fix: use existing scene document reload entry point in physics gate
- d6d686a fix: reject singular collider ancestry before mutation and load
- b8afcab Reject colliders with singular inherited world scale
- bc9d554 merge: validate inherited collider world scale in staging
- 17b673e test: verify emitted glTF child draws and document physics gate
- 22b32aa test: cover active trace restore and isolate authored portal edit guards
- 16d3ec6 test: keep valid portal-placeable hosts in rejection fixture
- 036f236 test: verify dirty play support parity and transactional singular descendants
- a8f4587 merge: extend physics gate with active trace restores and support parity
- 020e1d7 fix: validate nonzero scales without an arbitrary size cutoff
- 6552832 Preserve small nonzero collider scales during validation
- 17d7d44 merge: retain finite nonzero inherited collider scales
- f9d4b53 fix: record physics gate telemetry after rendering at the fixed camera
- ef41019 test: report populated scene SDF instances in physics gate
- c232814 fix: preserve signed dynamic scale and rebuild every authored shape change
- 362f670 fix: retain native bodies through entity teardown callbacks
- 55d4897 fix: keep awake dynamics out of raster auxiliary trace caches
- 514d8ad merge: stabilize auxiliary raster trace geometry during motion
- ea6b3b6 docs: record validated trace inclusion and capture contracts
- d475ab9 test: diagnose sleep metadata and input during physics gate
- 4f6b899 test: bake real SDF mesh cache before physics GPU validation
- b2edeef test: require every SDF instance baked and placed
- d6075db test: cover stable raster trace input and preserve finite tiny collider scale
- ec4f638 test: preserve mirrored offset poses and exact small scale edits
- 5b9333d merge: validate signed scales populated caches and raster trace stability
- d5e89ac test: retain exact runner source and assert native ECS activation agreement
- e33c64f merge: retain gate provenance and native ECS sleep invariants
- 5cb98f4 merge: complete validated Jolt core editor and renderer integration
- 2152401 fix: keep raster and indirect capture metadata under distinct prefixes
- c858bdb fix: create live native bodies before applying queued physics commands
- 3050a7f test: preserve commands queued for newly created play bodies
- f4a1cb0 merge: regress commands on bodies created during play
- f140f2d fix: synchronize tiny edits and compare TRS at relative precision
- eae7a2d docs: clarify creation command and floating-point transform contracts
- 7d70058 fix: apply teleport scale changes before exposing the new pose
- 9a1bc1c test: compare teleport velocity to its actual pre-command state
- 6ec0630 test: retain tiny authored pose scale edits and large representable TRS
- 6d87a3b test: integrate physics precision regressions
- 5a67253 docs: refine reviewed support activation and ancestry contracts
- da14af8 test: cover review support loss singular reparent and near vertical physics poses
- 8cd1858 test: integrate independent gate reviewer regressions
- 0c625f6 test: respect required collider component ownership in support regressions
- dc4105d test: retain unrelated distant sleeping bodies during support invalidation
- 99f3f43 test: check contact wake metadata before render and unaffected sleep immediately
- 715c332 fix: validate collider subtrees before hierarchy mutation
- 6c72f26 fix: wake changed support neighbors and recover stable physics rotations
- 0bc66b0 merge: resolve independent physics gate findings
