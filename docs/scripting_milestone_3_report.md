# Mirabilis Milestone 3 report

Milestone 3 is complete. Independent reviewer verdict: **Ship**, with no
unresolved blocking findings. Tested implementation is
`824fdab17ae2daa98afbf6a473d07eb9e2ddd6a5`. Debug/Release, independent headless,
Python and the complete 42-launch gate pass, including the required original
performance comparisons. Prior evidence and supplemental failures are retained
below.

## Scope, repository and team

CURRENT MILESTONE 3 overrides the downloaded specification's stale 1. This
implements the complete scripting milestone, retaining the reviewed ECS and
physics behavior. No hot reload, scripting raycasts/spawn API, player/body
interaction, body portal traversal, new shapes or joints were added.

The original repository is
`C:/Users/kenne/OneDrive/Documents/GitHub/Mirabilis`. Initial fresh Git inspection
and fetch found `main`, `origin/main` and `feature/physics` at reviewed M2 report
tip `e46908d3e10cd17276a2410c6b597a8ae05123cb`; its implementation is `0bc66b0`.
The original checkout was clean. Reviewed M1 commit
`6497ec16d60989401a8c20b4a4839aec9d3eca70` remains in that history. Historical
push/main statements in prior reports were not used as current Git evidence.

Implementation uses `feature/scripting` in `C:/dev/mirabilis-work/scripting`,
based on `e46908d`, with builds in `C:/dev/mirabilis-work/build-scripting`.
No applicable `AGENTS.md` was found in the repository or checked ancestors.
Frozen shared contracts were committed as `06c6e4c` before worker code began;
see `docs/scripting_contracts.md`. Workers branched from that commit and owned
separate files. Dependent changes were combined and tested on
`integrate/scripting-runtime` before the integration merge `b18ef97`.

The inherited lead owned native hosting, components/Scene, engine header/main and
input, CMake, the demo and integration. Its model identifier is not exposed by
the tools, so the requested lead model could not be independently selected or
verified. This is the specification's reporting fallback.

| Role | Actual selected model | Work |
| --- | --- | --- |
| Scripting specialist `/root/managed` | GPT-6.1 Sol, High | `scripting/`, samples and `docs/scripting.md` |
| Editor/scene specialist `/root/scripting_editor` | GPT-6.1 Sol, High | Script inspector, core v2 serialization and format documentation |
| Validation specialist `/root/scripting_validation` | GPT-6.1 Sol, High | Independent tests, fixtures, baseline, GPU and timing evidence |
| Fresh gate reviewer `/root/scripting_gate_reviewer` | GPT-6 Astra, High | Read-only combined review after worker retirement; final verdict Ship at `824fdab` |

At most four agents were active. Validation wrote tests from the specification,
frozen contracts and public headers without reading implementation. Worker
checkouts were outside OneDrive; the clean, fully merged managed/editor
checkouts were removed after retirement to free disk space. Their commits remain
in this branch. The user's modified physics-worktree `.last_scene` was preserved,
SHA-256 `cc8088a7cd1690466ee1b1bc6154f25bc6ce5e9fc0bbab6bae796ee67f0f9067`.
The original main checkout remains untouched. No M3 push or main merge occurred.
After the managed specialist retired, author-guide ownership was explicitly
reassigned to the lead for the diagnostic/deployment clarification.

## Dependencies and deployment

New prerequisite: **.NET 10**, MIT licensed, framework-dependent hosting through
official `nethost`/`hostfxr`. Actual verified versions are SDK **10.0.203**, runtime
**10.0.7** and Windows x64 host pack **10.0.7**. No NuGet packages were added.
The SDK provides `nethost.h`, `hostfxr.h`, `coreclr_delegates.h`, `nethost.lib` and
`nethost.dll` under
`C:/Program Files/dotnet/packs/Microsoft.NETCore.App.Host.win-x64/10.0.7/runtimes/win-x64/native`.
The native host dynamically loads `nethost.dll`, locates installed `hostfxr`, and
uses its default-context assembly-load and function-pointer delegates. It closes
initialization contexts but leaves the runtime loaded until process exit.

CMake finds the installed SDK/10.0 host pack, runs `dotnet build -c <config>` with
artifacts outside OneDrive, and deploys ScriptCore/SampleScripts DLLs, PDBs,
dependencies, `.deps.json`, generated Core `.runtimeconfig.json`, and
`nethost.dll` beside native executables. BUILD_TESTING also builds independent
managed fixtures and an isolated deliberately incompatible bridge. That fake
bridge is kept in its own build directory and never deployed beside the engine.
Running needs the installed x64 .NET 10 runtime, not the SDK. Missing hosting
files, incompatible runtime or incompatible ABI disable scripting with diagnostics
and preserve Script data while native scenes and bodies remain usable.

Existing dependencies are unchanged: Jolt **v5.6.0** (MIT, tag commit
`e77f175595e64cb44218cc9d9d56fc365ad0e36a`), EnTT **v3.15.0** (MIT), doctest
**v2.4.12** (MIT), simdjson **v3.3.0** (Apache-2.0 or MIT), and renderer libraries.
Native libraries remain vendored; configure downloads nothing. CMake minimum is
3.21. See `third_party/README.md` and `docs/scripting.md` for provenance and
deployment. Existing validation-only SDF tools/licenses remain as recorded in the
M2 report and final gate's `physics/sdf-bake-inventory.json`.

## Build, run and author

Use the source checkout outside OneDrive. Verified tools were Visual Studio 18
2026 Insiders/MSVC 14.51, CMake 4.3.1, Vulkan SDK 1.4.357.0 and .NET versions above.

```powershell
cmake -S C:/dev/mirabilis-work/scripting -B C:/dev/mirabilis-work/build-scripting -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build C:/dev/mirabilis-work/build-scripting --config Debug --parallel 8
ctest --test-dir C:/dev/mirabilis-work/build-scripting -C Debug --output-on-failure
cmake --build C:/dev/mirabilis-work/build-scripting --config Release --parallel 8
ctest --test-dir C:/dev/mirabilis-work/build-scripting -C Release --output-on-failure
Set-Location C:/dev/mirabilis-work/scripting/bin/Release
./engine.exe
```

Press Tab for Edit mode, choose File > Open Scene >
`scripted_physics_demo.json`, and select Falling box. Its Script section identifies
`Mirabilis.SampleScripts` / `Mirabilis.SampleScripts.PushBox`. Edit Force or
Impulse in Edit mode, then press Tab to Play. Force is applied each fixed tick;
Space applies an impulse while gameplay captures input. The box collides with the
floor. Leaving Play restores authored values; Reset Play restores and recreates
instances. The original `physics_drop_demo.json` is unchanged.

Author a public concrete subclass of `Mirabilis.ScriptCore.Script` in a net10.0
class library referencing ScriptCore. Override `OnCreate`, `OnFixedUpdate(float)`,
`OnUpdate(float)` and/or `OnDestroy`. Public instance fields support float, int,
bool, string, Vec3 and Entity. Deploy the user assembly/dependencies beside the
engine and attach by assembly identity plus full namespace-qualified type name.
See `docs/scripting.md` for compiling, API examples, ownership and failure rules.
Metadata discovery does not construct user types: unsaved fields show schema
zero/null defaults; actual initializers run at Play construction. Restart the
engine after rebuilding assemblies; hot reload/unload is deliberately absent.

For a repeatable scratch-only engine scripting check:

```powershell
python C:/dev/mirabilis-work/scripting/tests/script_gate.py --repo C:/dev/mirabilis-work/scripting --output C:/dev/mirabilis-work/new-script-gate
```

Use a fresh output directory. Earlier gates remain documented in the M1/M2
reports and `tests/script_gate_README.md`. Python validation removes inherited
`PSModulePath` case-insensitively in child environments, matching the fresh
baseline protocol; raw commands and logs are retained.

## Milestone 3 gate

| Requirement | Result and authoritative evidence |
| --- | --- |
| Debug/Release native engine, shaders and managed assemblies | PASS, `scripting-review-fixes/build-{debug,release}-storage-final.log`; both configurations deployed and executed |
| GPU-free core builds | PASS independently in both configurations with engine disabled, no Vulkan discovery/linking; final core evidence |
| Every earlier check | PASS required original gates, detailed below; historical supplemental performance miss retained |
| Callback order and Play/reset/delete/replacement lifecycle | PASS real managed fixtures and engine lifecycle hook |
| Detach/replacement/constructor failure/shutdown release roots | PASS C++ fixtures plus managed live-handle assertions, including failed OnCreate and throwing OnDestroy |
| OnDestroy once, repeated/nested destruction, children before parent | PASS real callbacks; pending reads succeed, mutations fail; body remains until cleanup callback completes |
| Faults in every callback | PASS type/entity/stack diagnostics, suppressed updates, final cleanup attempted once |
| Dynamic transform/queued physics ownership | PASS teleport/force/impulse/velocity, pending-target command discard, and next-step cleanup local/world/body teleports |
| Parent/static collider dirty synchronization | PASS native physics, player collision and rendered/world transforms agree after script movement |
| Script snapshots/field restoration and subtree remapping | PASS authored fields restore, full epoch invalidation, internal references remap including ID 0 and whitespace; external/null remain distinct |
| Missing assembly/type and orphan/changed/unsupported fields | PASS saved data retained, diagnostics emitted; metadata runs no constructors |
| ABI/layout, statuses, UTF-8 and wrong thread/scope | PASS native/C# size/offset checks; empty/non-ASCII/NUL ownership; isolated bad version/size/null-function bridges disable scripting without data loss |
| Actual incompatible runtime | PASS isolated hostfxr request for absent 99.0.0 fails for runtime-initialization reason and retains usable native Scene/Script data |
| Stale references | PASS recycled/full native handles and managed references across reset/replacement, including matching persistent IDs |
| Input delivery | PASS two real scripts, 8 fixed ticks, zero-tick carry, all-observer edges, frame/fixed isolation, same-frame press/release, repeats, focus and Play/reset clears |
| SDL input integration | PASS actual engine event path: capture rejects presses, releases survive capture, focus/mouse loss releases held keys, modifier aliases and Escape quit behavior |
| Script v2 persistence and editor | PASS strict six-type fields/unknown data round trips and compiled Edit-only inspector; exhaustive manual panel interaction not claimed |
| Edit/save scratch demo, reopen, run, sandbox replacement | PASS 280-frame Debug hook in 15.156 seconds: Force 4 retained, floor contact x=0.92648625/y=0.47999978, reset/leave/replay correct, sandbox restores 7 bodies/0 scripts |
| MIRABILIS_TEST validation errors | PASS all 42 launches exit 0, no fatal/Vulkan/VUID lines |
| Author guide | PASS `docs/scripting.md` |
| Independent reviewer | PASS, GPT-6 Astra High: Ship at `824fdab`; initial diagnostic blocker resolved, no remaining blockers |

Both independent Debug/Release core runs pass **74 C++ cases / 174,882 assertions**
(48 retained cases plus 26 scripting cases). The isolated managed console passes
**105 assertions**. All six CTest entries pass in both configurations: core,
managed, three malformed returned-table probes and incompatible runtime.
Normalized Python passes **181 tests**, compared with 177 before scripting.
The final normalized run took 23.319 seconds. An earlier invocation could not
resolve Get-FileHash with its inherited PowerShell module path; its failure
log is retained. Removing PSModulePath case-insensitively in the child environment
uses the same normalization as the baseline and earlier successful gates.

Coverage lives in `tests/script_tests.cpp`, `tests/managed/`, retained ECS/physics
tests, `tests/script_abi_probe.cpp`, `tests/script_gate.py` and parser tests.
EngineStats exposes live script counts; test telemetry reads actual host handles.

## Retained M1/M2 gates and baseline comparisons

All 48 previous C++ cases remain, including identity/recycling/epoch/allocator,
queues/cascade/precedence, hierarchy/world/gizmo parity, unknown component and
transactional scene IO, deterministic order and runtime-only protections. All 23
original authored scenes round trip every saved component and match collision
goldens. The physics and scripted demos have additional round-trip tests. No
authored scene was overwritten; saves use scratch copies.

The five earlier Debug engine cases pass: renderer cycle/minimize/restore
(4.500 seconds), scene switch (36.218), portal traversal (14.468), portal
placement (11.562) and ECS lifecycle (5.750). All 23 Release
collision dumps match original `5486a56`, reviewed M2 and fresh M2 exactly, with
maximum numeric delta **0**, identical order/counts and the retained absolute
1e-5 plus relative 1e-6 comparator.

SDF preflight passes in 7.250 seconds. All four physics GPU cases pass with genuine
SDF/surface/ray-query support:

| Case | Frames | Seconds | Additional evidence |
| --- | ---: | ---: | --- |
| Raster physics demo | 1080 | 42.922 | Falling/sleep/wake, cache eligibility/counters, teleport/history/reset/unchanged restore, scratch save/reopen/sandbox |
| Raster visual/glTF child | 1080 | 47.078 | Inherited interpolation/eligibility, native/ECS sleep, real SDF 6/6, four moving draws |
| Short reference child | 200 | 251.204 | Both moving objects visible; 59 final samples, maximum recorded GPU dispatch 13.411744 ms |
| Full reference lifecycle | 1080 | 352.594 | Teleport/reset/Edit/reopen/sandbox; 30 final samples, maximum 15.505184 ms |

Reference captures have zero nonfinite pixels and zero component residual.
Their Debug CPU ray-oracle/device-wait wall costs are not shipping Release frame
times. SDFTrace and SurfaceCache diagnostics also pass (1080 frames each,
40.391/40.047 seconds). Inspected captures omit the awake box from static SDF and
atlas and include it asleep; cards change 12 -> 18 -> 12. Static caches update
once per eligibility transition rather than climbing each falling frame, and
unchanged restores do not rebuild. Box indirect-light ROIs are 100% nonzero with
zero nonfinite values in both states. No separate AO/colored-bounce claim is made.

Original baseline evidence remains at `C:/dev/mirabilis-work/baseline`, M1 fresh
comparison at `C:/dev/mirabilis-work/physics-baseline`, and reviewed M2 evidence at
`C:/dev/mirabilis-work/physics-gate-m2-reviewed`. Before behavior changes, fresh
reviewed-M2 measurements were captured at `C:/dev/mirabilis-work/scripting-baseline`:
48 cases/170,520 assertions, Python 177, five Debug GPU cases and 23 exact collision
comparisons. Its unchanged approved binary validly measured sandbox 2.607/2.059/
1.898 ms, median 2.059, spread 34.434191%, failing original 2.02125 and historical
M2 1.95615 limits. Fresh Sponza 4.011/3.854/3.723, median 3.854, passes. No noisy
baseline run was removed or substituted.

Final timings use the original protocol unchanged: Release, 1600x900, saved plain
SSGI preset 2, internal 600x338, hidden panels, fixed cameras, 120 warmup plus 600
measured frames, three runs per scene, uncapped MAILBOX request/FIFO fallback,
steady-clock loop start-to-start interval including preceding present/wait.
All six runs have **600/600 focused samples**, real Computer Use focus activations,
and no concurrent engine/build/test/CPU audit. Renderer/exposure settings match;
the session's exposure preference caused no unrelated renderer code change.

| Scene | Final means, ms | Median | Spread | Required original comparison |
| --- | --- | ---: | ---: | --- |
| Sandbox | 1.840 / 1.843 / 1.842 | 1.842 | 0.162866% | PASS -4.311688%, limit 2.02125 |
| Sponza | 3.814 / 4.131 / 3.906 | 3.906 | 8.115719% | PASS -8.266792%, limit 5.696 using original spread rule |

Final supplemental comparisons also pass. Against historical reviewed M2,
sandbox is -1.127214% and Sponza is unchanged, within its 35.074245% allowance.
Against fresh M2, sandbox is -10.539097%; Sponza is +1.349248%, within 14.945511%.
The fresh-M2 sandbox allowance is 68.868383% from its measured spread; it remains
supplemental and does not replace the original limit.

Prior complete `c6ff564` results are retained at `scripting-gate-m3-world-fixed`:
sandbox means 1.801/1.958/1.992, median 1.958, spread 9.754852%; Sponza means
3.669/4.074/4.456, median 4.074, spread 19.317624%. Both passed the original
thresholds, but that sandbox result **FAILED** the historical-M2 supplemental
5% allowance: +5.099302%, 0.00185 ms beyond 1.95615. Its Sponza historical-M2
comparison passed (+4.301075% within 35.074245%), and both fresh-M2 comparisons
passed (-4.905294% / +5.708355%). That failure is not erased or substituted.
The new complete gate follows a real storage fix, with all six new valid runs
retained. These host-varying comparisons do not isolate causal scripting cost.

## Findings and resolutions

The fresh GPT-6 Astra High review at `c6ff564` found one P2 blocker: native
diagnostics retained every script log/exception indefinitely, while selecting a
missing type repeated metadata failure logging each frame. Contract refinement
`3296a76` freezes bounded recent history and a single-entry schema/error cache.
Fix `869f3ef` limits history to 256 entries / 1 MiB total / 64 KiB per entry,
truncates only at UTF-8 boundaries, and preserves complete console output. The
cache is keyed by assembly/type and never stores authored field values or runs a
constructor. Independent regressions `7cb0c78` fail 8 checks on the old code and
pass all 1,210 focused assertions on the fix. Real managed logging produces the
same 1,951,974 console bytes in both runs. Corrected retained modes have 256
entries / 7,936 bytes, 26 entries / 1,040,988 bytes, and 5 entries / 262,255 bytes;
each oversized entry is at most 64 KiB and valid UTF-8. Thirty-two repeated
missing-type queries no longer append errors; saved fields, changing keys and
current enabled state remain correct.

The lead then found that resizing a string could retain its original oversized
allocation. Contract `c055b6d` and fix `23c824a` require a fresh retained prefix,
releasing the original buffer. Independent regression `346b8a6` fails exactly two
capacity checks on `c88d64f`: a truncated entry still held 2,170,063 bytes and
aggregate capacity was 2,170,190. The corrected giant-message mode retains
65,652 logical bytes / 65,662 capacity while preserving the complete 2,170,061
console bytes; all 1,275 focused assertions pass. A preliminary test edit expanded
an unrelated teardown loop and passed without exercising the giant message; that
run is retained as preliminary, not proof. The corrected old/fixed storage logs
are authoritative. The in-progress `c88d64f` GPU batch at
`scripting-gate-m3-reviewed` was explicitly cancelled for this correction and is
incomplete: its scripted lifecycle passed, but the interrupted physics child has
unknown exit status. It is not counted as a completed gate. The final candidate
received a fresh complete batch in `scripting-gate-m3-storage-fixed`.

The reviewer also raised a nonblocking source-level concern about target-only
incremental C# deployment. Both before and after probes on the installed Visual
Studio generator passed: changing only the sample C# source changed the deployed
DLL to match the managed build output while `engine.exe` retained its hash. Fix
`4cf2cbc` simplifies deployment to the always-run managed dependency and removes
the separate ALL target/native post-build duplication, covering builds without a
native relink. Each probe restored the exact original source bytes. JSON hashes
and all six build logs are in `C:/dev/mirabilis-work/scripting-review-fixes`.

Final independent recheck at `824fdab`: **Ship; no unresolved blocking findings**.
The reviewer verified all 474 evidence hashes, current source/deployments/scenes,
42 individual results and raw error scans, all ordered collision payloads against
three baselines, six focused timing calculations, new captures, and old/fixed
regression/probe records. It confirmed the retained-storage and repeated-metadata
fixes and the simplified deployment dependency. It performed no writes, builds
or engine launches during review.

Pre-review lead audit found a real extra-tick delay: an OnDestroy world/body
teleport was wrapped in a generic deferred world setter, which queued the actual
teleport again at the next tick. Independent regression `3078975` (integrated as
`08dd674`) failed 50 assertions on old `ff3dae1`, while the local-transform control
passed. Fix `c6ff564` queues the existing dynamic teleport once, retaining deferred
world-to-local conversion for static/non-body targets. All 138 focused assertions
and the complete final gate pass. Old/fixed logs are retained.

Earlier test-driven corrections discard shutdown-generated commands, remap
whitespace-wrapped entity IDs, retain modifier input until both physical aliases
release, and deliver Escape input while preserving quit. A fixture was corrected
to expect mutation failure when its target is also pending during whole-scene
teardown. An early Release run used an outdated managed fixture while its build
was still progressing; matched frozen deployments were rebuilt and retested.
Those initial failure logs remain; none are represented as passes.

Initial GPU candidate `ff3dae1` evidence remains at
`C:/dev/mirabilis-work/scripting-gate-m3-final`. Its scripted GPU result is valid;
the interrupted raster runner has unknown exit and is not counted. The corrected
candidate received the complete fresh 42-launch gate.

## Evidence and limits

Final evidence root: **`C:/dev/mirabilis-work/scripting-gate-m3-storage-fixed`**.
Read `validation-summary.txt`, `all-engine-results.json`,
`collision-comparisons.json`, `performance-comparisons.json`,
`gpu-observations.json`, `visual-observations.txt`, `timing-focus-activations.json`,
and `input-before.json`/`input-after.json`. Subdirectories contain raw stdout/
stderr, flags, captures, scratch scenes and copied runners. `independent-cpu/`
contains the headless/Python and old/fixed regression logs;
`scripting-review-fixes/` contains the lead build/CTest and deployment probes.
`evidence-sha256.json` covers **474 files**, SHA-256
`7f487cce780c819d2c8ae7d5f60ba6e8282e6c8904707a79e91033c5f883310e`.
The lead independently recomputed all 474 hashes and verified all 175 recorded
source/shader files, 25 scenes, 32 runtime files, the selection marker and the
42 successful result records against current files. The reviewer independently
repeated these checks and issued Ship.

Final tested Debug engine SHA-256:
`dc14e5dc6f0e08e0d6a8343a39c63f7ee47da4f8678b7a8d1ceef4107a4bc53c`.
Release: `fde47fd919f441b0e88caadc39a028b1a9067601f5e556a811772facf9431550`.
The final hash audit verifies 42 valid launches, all exits 0 and no fatal lines;
source/shaders/native+managed deployments/all 25 scenes/selection/HEAD/status
remain unchanged through the gate. Report-only edits follow that code snapshot.

Validation is on Windows 11 Home build 26200, NVIDIA RTX 3060 Laptop driver
32.0.15.5597 (555.97), Balanced power, matching the baseline; the final evidence
includes gpu-driver.json, operating-system.json and power-mode.txt. Native hosting is
Windows x64 in this implementation. Exhaustive manual mouse/keyboard coverage of
every inspector control was not performed; code, serialization, managed fields
and actual SDL integration were checked as described. The selected Vulkan present
mode is not logged. Existing nonfatal layer-manifest and upstream doctest C5285
warnings remain. Metadata defaults differ from runtime field initializers until
values are authored. Script callbacks are synchronous; no asynchronous workflow,
hot reload, runtime unloading, raycasts, spawning API or player/body/portal-body
interaction is supplied. The historical M2 sandbox supplemental timing miss is
retained above. No required validation is marked blocked or unrun as passed.

## Local commits

All local commits through the tested source after reviewed M2 are listed below,
including worker history and integration counterparts retained by merges. The
report-only completion commit is additional. No M3 commits have been pushed.

```text
06c6e4c docs: freeze scripting ABI lifecycle input and saved-field contracts
8d8c5f1 feat: add .NET 10 ScriptCore API and managed Cdecl bridge
980e867 docs: add sample scripts and C# authoring guide
ecbb0fe Persist validated script fields in version 2 scenes
f592682 Edit script identities and public fields in inspector
a4a534a feat: host .NET 10 scripts and integrate scene cleanup snapshots
65da097 feat: connect scripting build play loop and input latches
c72a2cd merge: integrate managed scripting API and samples
fa0106a merge: integrate script inspector and v2 persistence
4c9c1bb feat: add .NET 10 ScriptCore API and managed Cdecl bridge
ae70fb1 fix: share core identity for explicit core assembly lookups
7a0a1c0 merge: retain shared ScriptCore identity for all user lookups
d8181f9 feat: add editable PushBox scripted physics demo
4c3a9e3 test: cover frozen scripting ABI fields input and real managed lifecycle
1309c37 test: cover frozen scripting ABI fields input and real managed lifecycle
0fe1e5a test: expose scripted demo lifecycle and live instance validation hook
d9b3c36 fix: validate native UTF-8 and contain field synchronization errors
36637fc test: extend scripting teardown metadata ABI rejection and GPU lifecycle gate
3724c42 merge: build independent scripting tests against integrated runtime
cd08ca9 test: extend scripting teardown metadata ABI rejection and GPU lifecycle gate
d33a523 test: build isolated managed ABI rejection probes
349f7ae docs: explain independent scripting fixtures and serial GPU gate
2ccc460 Merge commit 'd33a523' into work/scripting-validation
4d35980 fix: discard shutdown commands and remap whitespace entity fields
4f33d08 docs: explain independent scripting fixtures and serial GPU gate
ae1652c test: require shutdown to discard commands and remap whitespace references
e62a585 test: require shutdown to discard commands and remap whitespace references
edd5397 test: expect pending targets during whole-scene cleanup
6f9a3e1 Merge commit 'e62a585' into work/scripting-validation
49d071b test: expect pending targets during whole-scene cleanup
5e43980 Merge commit '49d071b' into work/scripting-validation
b18ef97 merge: integrate validated C# scripting host editor and managed API
ca67474 test: revalidate later script callbacks after sibling destruction
f2a4ff3 test: verify SDL capture focus repeats and play input transitions
61fde9f test: revalidate later script callbacks after sibling destruction
413275a fix: retain modifier input while either physical key is held
b36112a test: require actual SDL capture focus and Play input gate evidence
abc06fc test: require actual SDL capture focus and Play input gate evidence
48f6883 test: distinguish incompatible runtime failure from malformed managed ABI
6780cf0 fix: deliver Escape input before the existing quit action
dac0699 test: distinguish incompatible runtime failure from malformed managed ABI
ff3dae1 test: exercise incompatible installed-runtime handling in isolation
ae3068d Merge commit 'ff3dae1535f3855343741e81337fdb477c3dd569' into work/scripting-validation
3078975 test: require next-step OnDestroy world and rigid body teleports
08dd674 test: require next-step OnDestroy world and rigid body teleports
c6ff564 fix: apply cleanup world teleports on the next fixed step
a580e65 Merge commit 'c6ff56439d28f5c8ab378e5521a2b4dd9ec4d29b' into work/scripting-validation
3296a76 docs: bound diagnostic retention and repeated metadata queries
d1a6d9c Merge commit '3296a76' into work/scripting-validation
869f3ef fix: bound script diagnostics and cache inspector metadata
4cf2cbc build: deploy scripts through the managed target dependency
7e0b86f docs: explain diagnostic history and incremental script deployment
7cb0c78 test: bound real managed diagnostic history and cache metadata errors
c88d64f merge: integrate independent diagnostic and metadata regressions
9c324d1 Merge commit '7e0b86f' into work/scripting-validation
f4cc470 Merge commit 'c88d64f213278c5f0292503540583dcb56f802f1' into work/scripting-validation
c055b6d docs: require truncated diagnostics to release oversized storage
23c824a fix: release oversized log storage after UTF-8 truncation
c4bcfcd Merge commit 'c055b6d' into work/scripting-validation
346b8a6 Test retained diagnostic allocation limits with giant UTF-8 log
824fdab merge: verify truncated diagnostics release original allocations
```
