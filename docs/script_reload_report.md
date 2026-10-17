# Live C# script reload

This is the original implementation report at `fa18bba`. Subsequent independent
review findings, corrections, validation and the authorized main publication are
recorded in [the review report](script_reload_review_report.md).

The requested workflow is implemented: save user C# code during Play, let the
background compiler finish, and continue with the new code in the same live scene.
Compilation and rejected reloads leave the previous working scripts running.
This extends completed Milestone 3 at the user's explicit request; it does not
change the historical milestone reports or their original scope.

## Checkout and ownership

- Branch: `feature/script-hot-reload`, based on reviewed M3/report tip
  `3d33fc3548773d15c9b16be94e3b609560044b27`.
- Source: `C:/dev/mirabilis-work/scripting`.
- Build: `C:/dev/mirabilis-work/build-scripting`.
- Evidence: `C:/dev/mirabilis-work/reload-check`.
- The Codex lead implemented and audited this extension serially; its exact runtime
  model label was not exposed in tool metadata. No new subagents or
  independent reviewer were used for this extension. The earlier M3 independent
  approval applies to M3, not to these subsequent changes.
- All commits are local. Main remains
  `e46908d3e10cd17276a2410c6b597a8ae05123cb`; no push or merge into main.

The user's pre-existing modified demo and selection marker were preserved:

| File | SHA-256 |
| --- | --- |
| `assets/scenes/.last_scene` | `f922093298c0ddd64b8bf545164113332fa6f7bbaf27ef8ee86d2209e440c517` |
| `assets/scenes/scripted_physics_demo.json` | `c6ef9a48431c44e8a197267d8df35fc8082415874fdd6be225662b494936dab9` |

GPU runners use scratch scene copies and verify every authored JSON and selection
marker hash. The separate physics worktree's selection marker is also unchanged.

## Behavior and implementation

The frozen contract is in `docs/script_reload_contracts.md`. User projects under
`scripting/` are watched by default, excluding ScriptCore and generated bin/obj
folders. Debounced source changes build all user projects asynchronously into an
isolated generation. Windows job ownership terminates compiler children when the
host shuts down. Re-enabling automatic reload requests the latest sources.

The engine swaps scripts on its main thread between frames. ScriptCore and .NET
remain loaded; user assemblies use replaceable collectible AssemblyLoadContexts.
DLLs and symbols load from memory streams, avoiding locks on build outputs.
ABI v2 appends the reload entry and updates both mirrors and rejection fixtures.

Every replacement object is prepared before any live handle is replaced. A bad
build, dependency/type failure, or constructor exception keeps all previous
instances. Constructors cannot mutate native scene state during preparation.
Success invalidates metadata caches and allows faulted callbacks to recover.
Retired contexts are weakly tracked and checked before another replacement;
retained references block further reload with a diagnostic instead of accumulating
an unbounded sequence of contexts.

Writable instance fields of the six existing supported types survive, including
private fields, inherited private fields, and auto-property backing fields.
Fields match by declaring type, name, and type. Statics and `[NonSerialized]`
fields reset; added fields use initializers. Removed/type-changed authored data
remains recoverable. Private runtime fields do not enter scene serialization.

Reload does not call OnCreate/OnDestroy, restore a snapshot, change the scene
epoch, recreate bodies, reset velocity, discard queued commands, or invalidate
motion history. The normal Reset/leave Play behavior still restores the authored
snapshot. Compilation happens while Play continues; the short managed replacement
itself occurs synchronously between frames.

## Build and try it

From the outside-OneDrive checkout:

```powershell
cmake --build C:/dev/mirabilis-work/build-scripting --config Debug --parallel 4
ctest --test-dir C:/dev/mirabilis-work/build-scripting -C Debug --output-on-failure
Set-Location C:/dev/mirabilis-work/scripting/bin/Debug
.\engine.exe
```

Load `scripted_physics_demo.json`, enter Play, and edit a callback in
`scripting/Mirabilis.SampleScripts/Samples.cs` or another attached user script.
Save it and inspect **Scripts > Auto Build & Reload** status. New callback code
runs after compilation without resetting the scene. **Build & Reload Scripts**
requests it manually. A syntax error shows the compiler error while old code
continues. Source and SDK must be available for compilation; deployed scripts
still run when build controls are unavailable. See `docs/scripting.md` for the API.

No new dependencies were added. The existing Microsoft .NET SDK 10.0.203,
runtime/host pack 10.0.7 (MIT), CMake 4.3.1, VS 2026 Insiders/MSVC 14.51, Vulkan
SDK 1.4.357, and Python 3.12.10 were used. Existing Jolt and other dependency
versions/licenses remain as recorded in the milestone reports.

## Validation

The tested implementation is commit `54593bc`. Subsequent changes are documentation
only. All paths below are relative to `C:/dev/mirabilis-work/reload-check`.

| Check | Result | Evidence |
| --- | --- | --- |
| Debug and Release builds | PASS | `build-debug-approved-source.log`, `build-release-approved-source.log` |
| Debug CTest | PASS, all 7 suites; native 78 cases / 175,250 assertions; managed 105 assertions | `ctest-debug-approved-source.log` (42.49 s) |
| Release CTest | PASS, all 7 suites; native 78 cases / 175,250 assertions; managed 105 assertions | `ctest-release-approved-source.log` (36.60 s) |
| Python regressions | PASS, 181 tests | `python.log` (27.739 s; Python sources unchanged by later C++/C# fixes) |
| Source-save/build/reload in real Debug engine | PASS | `gpu-debug-final/` |
| Source-save/build/reload in real Release engine | PASS | `gpu-release-final/` |
| Existing scripting lifecycle/input GPU gate | PASS | `script-lifecycle/` (13.812 s) |
| Existing cycle, scene switch, portal traversal/placement, ECS lifecycle | PASS, all 5 launches; no fatal validation diagnostics | `ecs-regressions/validation-summary.json` |
| Authored scenes, both selection markers, deployed sample restoration, unchanged main, final source/binary provenance | PASS | `verification-manifest.json`, runner `preservation.json` and lifecycle input-before/input-after records |

Debug's reload preserved epoch 3, 3 bodies, 1 instance, velocity 0.25, Updates 231
and private observation 271 at frame 231; the next frame used +10 behavior while
private state advanced by one. Release did the same at frame 4620, Updates 4620,
private observation 4660. Both retained Creates=1 and passed exact before/after
world-state assertions. Engine SHA-256 values:

- Debug: `4b7ccecf5da377e8cc85d76d84ca66fd14e6b70030b7847a533e5efb7c5f294d`.
- Release: `b1237534c449b3228b9e23ea73158449cc7ae6e656c244c98f898c66a3df733e`.

To repeat the live source-edit gate, use a new evidence directory:

```powershell
python tests/script_reload_gate.py --repo C:/dev/mirabilis-work/scripting --output C:/dev/mirabilis-work/reload-repeat-debug --configuration Debug
```

The test suite covers state transfer, all-instance rollback, throwing and
native-mutating constructors, missing types/directories, fault recovery, changed
fields, repeated unload/reload, pending destruction and nonempty command queues,
physics identity/velocity/history, authored snapshot restoration, multiple user
projects, automatic reload toggles, real compiler failure/recovery, Core restart
requirements, unavailable source configuration, and ABI/runtime rejection.

The real-engine runner changes a source file from Updates += 1 to Updates += 10
while Play is active. It asserts the exact pose, velocity, epoch, player position,
accumulator, body/instance counts, pending commands and history-reset count across
replacement, then observes new code with continuing private state and Creates=1.
It restores deployed sample binaries and removes only its own temporary source.

Earlier evidence is retained. `gpu-debug` initially failed because the bounded
test harness started in Edit mode and had no live instance. The harness was fixed
to enter Play explicitly; subsequent Debug/Release runs exercised actual reload.
The failure is not counted as a pass. `ctest-debug-final-source.log` records the
later failed unavailable-request/toggle check; its resolution is covered by both
final passing suites. Negative compiler/constructor/ABI/runtime
diagnostics in successful suites are intentional rejection probes.

Existing M1/M2/M3 baselines and performance evidence remain intact. No new frame
time comparison or full renderer/cache benchmark sweep is claimed for this
extension. Renderer/exposure settings were not changed.

| Behavior | Completed M3 baseline (`3d33fc3`) | Reload extension |
| --- | --- | --- |
| Adopt edited callback code | Rebuild and restart | Save source during Play; background build and frame-boundary swap |
| Live scene on code change | No in-Play replacement | Epoch, identity, pose, velocity, queues and history retained |
| Private runtime fields | No reload migration | Supported writable private/inherited/backing fields retained |
| Lifecycle and authored reset | OnCreate/OnDestroy and snapshot restoration | Same lifecycle/reset behavior; reload itself repeats neither hook |

## Audit findings and limits

- Automatic reload initially did not rebuild edits made while disabled when it
  was re-enabled. Re-enabling now requests a build; regression coverage checks it.
- Unavailable source/SDK configuration now disables compile controls and reports
  the limitation without repeatedly logging errors.
- Staging initially copied unrelated native PDBs. It now copies symbols only
  beside matching DLLs, avoiding large redundant native debug files.
- The live-engine harness's initial Edit-mode setup was corrected as above.
- Public `[NonSerialized]` fields could be restored through inspector binding
  despite migration excluding them. The shared editable-field filter now excludes
  them too; a public-field regression verifies reset and absence from scene data.
- The auto-toggle test caught a manual build request made while source was
  unavailable surviving until later configuration. Such requests are now ignored
  when compilation is unavailable. The failed gate log is retained.

This is the requested Unity-style workflow, with the current Mirabilis field API:
float, int, bool, string, Vec3, and Entity. Arbitrary object graphs, collections,
async state machines, and script-owned background threads are not migrated.
Externally retained objects/delegates can prevent context unloading; release them
before reload or restart the engine. Core/native changes require restart.
Constructors run again and external side effects outside the native bridge cannot
be rolled back. No separate reload lifecycle callbacks were added.

No manual inspector interaction sweep, new independent review, or performance
benchmark sweep was performed for this extension. Automated engine tests exercise
the source-save path and existing lifecycle/input/portal behavior.

## Local commits

- `5b27149` freeze reload/state preservation contracts.
- `931f3a9` transactional managed/native reload retaining the live world.
- `abd3154` asynchronous multi-project compilation and source watching.
- `4147260` real-engine source-edit validation and author guidance.
- `f4a9e7e` unavailable compiler handling and grouped rollback validation.
- `f4486c8` rebuild pending edits when automatic reload is re-enabled.
- `54593bc` `[NonSerialized]` handling and unavailable-request correction.
- Final documentation-only commit: this report and clarified author guidance
  (see local Git log for its hash).
