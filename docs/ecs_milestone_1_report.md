# Mirabilis Milestone 1 report

Milestone 1 is complete: the registry-backed ECS foundation, GPU-free core,
typed editor/gameplay/render callers, authored Play snapshots and version 2 scene
serialization. Jolt and C# scripting are outside this milestone.

## Repository and checkpoints

The pinned base is `5486a56d19eea7d37fb248b10936a8ba77302a5b`. The original
checkout was clean on `recovery/r6-validation` at `625d32b`; initially local
`main` was `848dd37` and `origin/main` was the pinned base. At final inspection,
`main` and `origin/main` both resolve to the pinned base and the recovery tip is
unchanged. This implementation did not move `main`, push, or merge into it.
No applicable `AGENTS.md` was found in the repository or its ancestors.

Implementation is in `C:\dev\mirabilis-work\ecs` on `feature/ecs`; worktrees,
builds and measurements are outside OneDrive. The original checkout remains
clean. Authored scene files and `.last_scene` were preserved; save/load tests
used scratch directories.

The instrumentation-only baseline exceptions are `c7863a7` and `5da889d`.
Baseline collision fixtures were captured before behavior changes. The core
extraction checkpoint is `c481fc3`; Debug/Release builds, headless tests and
Python tests passed, and all 23 engine collision dumps exactly matched baseline.
Contracts were frozen at `ebab4e6` before workers started. The combined registry,
editor, serialization, gameplay and test group was built and tested on
`integration/ecs` before merge `89cabeb` into `feature/ecs`.

## Team and dependencies

The lead handled contracts, ECS, lifecycle, engine integration and CMake. Its
inherited chat model label is not exposed by the tools. The editor/scene,
gameplay/render and independent validation workers each ran GPT-6.1 Sol with
High reasoning in separate worktrees, with the document's file ownership rules.
The fresh independent gate reviewer ran GPT-6 Astra with High reasoning, after
the implementation workers retired, and received no implementation chat history.
The validation worker was restarted to add regressions for its findings and
repeat the complete GPU gate on the fixed binaries.

New vendored dependencies are EnTT **v3.15.0** (MIT), doctest **v2.4.12** (MIT),
and simdjson **v3.3.0** (Apache-2.0 or MIT). simdjson is the exact version already
required by fastgltf 0.6.1; it is now vendored rather than downloaded during
configuration. EnTT/doctest are headers; simdjson is statically linked. Licenses
are included. No Jolt or .NET dependency was added.

## Build and run

Validated on Windows using Visual Studio 18 2026 Insiders, MSVC 19.51.36252,
CMake 4.3.1, Vulkan SDK 1.4.357.0 and Python 3.12. From a Visual Studio developer
shell, use an external build directory:

```powershell
cmake -S C:\dev\mirabilis-work\ecs -B C:\dev\mirabilis-work\build-ecs -G "Visual Studio 18 2026" -A x64
cmake --build C:\dev\mirabilis-work\build-ecs --config Debug --parallel 8
cmake --build C:\dev\mirabilis-work\build-ecs --config Release --parallel 8
ctest --test-dir C:\dev\mirabilis-work\build-ecs -C Debug --output-on-failure
ctest --test-dir C:\dev\mirabilis-work\build-ecs -C Release --output-on-failure

cmake -S C:\dev\mirabilis-work\ecs -B C:\dev\mirabilis-work\build-core-only -DMIRABILIS_BUILD_ENGINE=OFF
cmake --build C:\dev\mirabilis-work\build-core-only --config Debug --parallel 8
ctest --test-dir C:\dev\mirabilis-work\build-core-only -C Debug --output-on-failure
```

Run `engine.exe` with working directory `C:\dev\mirabilis-work\ecs\bin\Debug`
or `bin\Release`, so existing relative assets resolve. Load `sandbox.json` for
the existing demo, edit its hierarchy/inspector, enter Play and use **Reset Play**
or return to Edit to restore authored state. There is no new physics/script demo.

For Python tests, this host's inherited PowerShell 7 `PSModulePath` must be
removed from the Python child environment, case-insensitively, so the existing
Windows PowerShell metadata writer can load `Get-FileHash`:

```powershell
python -c "import os,subprocess,sys; environment={k:v for k,v in os.environ.items() if k.upper()!='PSMODULEPATH'}; sys.exit(subprocess.call([sys.executable,'-m','unittest','discover','-s','tests'],env=environment))"
```

This is an environment correction, not a skipped test or source workaround.

## Validation gate

| Check | Baseline | Milestone 1 |
| --- | --- | --- |
| Debug/Release engine and shaders | Both passed | Both passed after final hook correction |
| Python suite | 170 passed with corrected environment | 173 passed with the same correction; no tests dropped |
| Headless C++ suite | New extraction smoke target passed | 26 cases, 158,497 assertions passed in Debug and Release after review fixes |
| Core-only configuration | Extraction core had no Vulkan includes/link | `MIRABILIS_BUILD_ENGINE=OFF` configures, builds and passes CTest without finding Vulkan |
| Existing scenes | 23 baseline collision captures | All 23 v1 scenes load, scratch-save as v2, reload, compare every saved component and ordered roots/children; both loads match collision goldens |
| Engine collision comparison | Ordered authored pairs and no player portals | All 23 final Release dumps have identical counts/order and maximum numeric difference 0 |
| GPU portal/cycle validation | Four bounded Debug cases passed, zero errors | Same four cases pass, exit 0, zero fatal/validation/VUID lines |
| New ECS document lifecycle | Not present | Eight-frame Debug scratch case passes, including replacement-map assertions and document/portal/lifecycle success markers |
| Frame time | Frozen three-run measurements | Both scenes pass; six valid runs with 600/600 focused samples each |
| Independent review | Not applicable | Three blocking P2 findings resolved; final approval recorded below |

The C++ cases cover create/cascade/destroy, recycled and stale full handles,
epoch invalidation with matching persistent IDs, reparent/cycle refusal, ordered
roots/children and deterministic emission, world matrices and extracted gizmo
decomposition under rotated non-uniform parents, dirty descendants, duplication,
pending readable entities and refused mutations, harmless repeated destruction,
queued command discard, commands issued during callbacks, bounded drains,
destruction during registry-view iteration, snapshot restore/high-water counters,
protected runtime subtrees, externally driven authored entities, spawn/time-trial
queries, v1 duplicate-ID reservation and ID 0/64-bit reference round trips,
staging failures, unknown future component blocks, missing asset identities and
both save failure paths. Engine replacement-map and GPU resource behavior is
checked by the bounded engine harness, rather than claimed from headless tests.

Initial Python execution inherited an incompatible `PSModulePath`: baseline
reported 159 run, one failure and one setup error; two intermediate gate retries
showed the same environment problem. Correct case-insensitive removal passed
all 170 baseline and all 173 milestone tests. All failed logs are retained.

The first new ECS lifecycle run failed opening the saved snapshot for reload.
The file independently parsed successfully (13 authored entities, allocator 18).
The test hook held an SDL environment pointer across later environment reads;
Windows SDL reuses that buffer. Commit `9478e69` copies the path immediately and
checks file opening. This was a hook error; production serialization was valid.
The failed GPU run remains in `gate\validation`; the corrected full run is kept
separately in `gate\validation-fixed`.
The post-review timing attempts in `gate\review-timings` and
`gate\review-timings-focused` had zero focused samples and are excluded as
invalid. Windows kept focus on VS Code. The Computer Use `@oai/sky` API activated
the returned engine windows without changing benchmark settings. The complete
six-run protocol then passed in `gate\review-timings-active`; no invalid mean is
included in the comparison.

The repeated baseline cases are minimize/restore cycle on `shadow_showcase`
(20 frames), replacement from `lighting_calibration` to Sponza (100), authored
portal traversal on `gi_portal_pair` (200, `crossed=true`) and player portal
placement on sandbox (180). The new eight-frame sandbox lifecycle case checks
live GPU-map clearing on replacement, authored Play saves without simulation
state, stale references after Reset Play, restored/deleted/created entities,
runtime survivors, settings, queues, allocator exhaustion rejected before
replacement, malformed loads in both modes and both failed-save paths preserving
bytes and dirty state. It separately exercises direct-delete protection and
parent-cascade portal host cleanup with authored pair restoration.

Collision comparison uses absolute `1e-5` plus relative `1e-6` tolerance and
requires exact counts/order for walls, grounds and ramps. The captured core
extraction and final engine differences are both zero. Input hashes and raw
baseline golden provenance are retained in the test fixtures and manifests.

Final validation binary SHA-256 is
`0d2d89a8c399819fb0d60e9f2c43abef3ee9202006f99fc2b7d4753e7d6223aa`;
final Release binary SHA-256 is
`db1fcc0240e40b542cd941e61d71b2c531f03f2be8d3e15dad9df8b954ecd26a`.
All validation runs preserve `.last_scene`; expected diagnostic messages from
deliberately malformed loads and failed saves are retained as exercised checks.

Performance compares the median of three mean frame times after 120 warm-up and
600 measured focused frames per run. The interval is steady-clock loop start to
start, including preceding present/wait. Both runs use Release, validation off,
1600x900, hidden panels and saved renderer/GI settings, with no GI overrides.
Sandbox's fixed camera is `0 2.5 12 -0.08 0`; Sponza's is `0 2.5 12 0 0`.
Baseline sandbox median is 1.925 ms (spread 4.416%, allowance 5%); Sponza median
is 4.258 ms (spread 16.886%, allowance **33.772%**, twice its baseline spread as
required by the specification).

| Scene | Baseline means (ms) | Gate means (ms) | Baseline median | Gate median | Change | Gate spread | Limit |
| --- | --- | --- | --- | --- | --- | --- | --- |
| sandbox.json | 1.954 / 1.925 / 1.869 | 1.931 / 1.908 / 1.881 | 1.925 | 1.908 | 0.88% faster | 2.621% | 2.02125 ms |
| sponza_showcase.json | 4.439 / 3.720 / 4.258 | 4.539 / 4.414 / 4.428 | 4.258 | 4.428 | 3.99% slower | 2.823% | 5.696 ms |

No GPU processes ran concurrently during gate measurements. All six timing runs
completed in raster mode, with zero fatal/Vulkan errors and unchanged
`.last_scene`. These results satisfy the specified threshold; they are not a
claim that ECS itself caused the observed performance changes.

Retained evidence is under `C:\dev\mirabilis-work\baseline` and
`C:\dev\mirabilis-work\gate`. Final evidence directories are `review-manifest`,
`review-validation`, `review-collisions` and `review-timings-active`; earlier
complete gate evidence and failed attempts remain separately preserved.
Final build/test logs are `review-final-build-*.log`, `review-final-core-*.log`
and `review-final-python.log` in `C:\dev\mirabilis-work`. Core fixtures and the reusable `tests/ecs_gate.py`
runner are committed. Reproduce each runner mode into a fresh output directory:

```powershell
python tests/ecs_gate.py gate-validation --repo C:\dev\mirabilis-work\ecs --output C:\dev\mirabilis-work\recheck-validation
python tests/ecs_gate.py collisions --repo C:\dev\mirabilis-work\ecs --output C:\dev\mirabilis-work\recheck-collisions
python tests/ecs_gate.py timings --repo C:\dev\mirabilis-work\ecs --output C:\dev\mirabilis-work\recheck-timings --baseline C:\dev\mirabilis-work\baseline
```

## Independent review

The fresh reviewer identified three blocking P2 findings and reproduced each
outside the repository against the existing Debug core library:

1. Replacing a valid 4,097-entity scene threw after the ordinary 4,096-item drain,
   leaving one pending entity. Snapshot restoration had the same finite teardown
   limit. Commit `855ce1a` gives synchronous clear/restore a budget covering all
   existing entities, while retaining the ordinary per-frame cap and bounded
   callback-generated work.
2. A destruction callback queuing parent local x=10 followed by child world x=5
   yielded child world x=15. The world setter had computed local coordinates
   before preceding commands applied. The same commit queues an owned world
   matrix and converts it at execution time.
3. With initial commands A/B, A could destroy B and enqueue C; erasing B from the
   live deque let C execute in the first batch. Commit `bb0de5f` detaches the
   initial batch, continues checking every target for stale/pending state, and
   leaves newly queued commands for the next batch.

The reviewer relinked its isolated repros against both fixes: replacement
succeeds, child world x=5 matches the request, and the first command batch applies
one command with C queued for the next batch. Independent regression commits
`393274b` and `5c5bb10` exercise all three findings. Their large-scene case covers
clear, replacement, live and already-pending snapshot restoration, readable
callbacks, stale handles, high-water allocation, empty work and the unchanged
ordinary cap (4,096 destroyed with 904 retained). Queued world tests also cover
reparenting before world conversion. One incorrect clear-only epoch assertion
was corrected in `e2bcc9c`: clear destroys full handles; replace/restore advance
the epoch. All 26 cases and 158,497 assertions pass in Debug and Release.
The reviewer independently verified the final focused timings, matching
executable hashes, validation cases and exact collision comparisons, and approved
the Milestone 1 gate at `e2bcc9c`: **Ship. No unresolved blocking or nonblocking
findings.** It independently ran the final Debug test executable as well.
Every required Milestone 1 gate check is passed; no required check remains
blocked or unrun. No work on Milestone 2 or 3 was started.

## Limitations

The core is headless and Vulkan-free. Engine asset reconstruction retains the
existing Sandbox/Floor requirements; core staging accepts generic valid scenes.
Baseline value components remain required; optional tags can be detached.
Renderer resource reconstruction occurs after validated scene commit; it is not
a general GPU rollback transaction. Missing authored paths/settings are retained
with existing asset diagnostics. Physics-controller names such as `PhysicsDt`
remain until Milestone 2's library integration.

The GPU gate runs on one RTX 3060 Laptop GPU with NVIDIA driver 32.0.15.5597
(555.97), Balanced power. It is not cross-vendor or Linux validation. Ray-query
shadows were available and exercised. Sponza path tracing was not run. Present
policy is uncapped, MAILBOX requested with FIFO fallback; the actual selected
mode is not logged, so only the identical request policy is verified. Sponza's
high baseline variance limits performance precision. Existing NVIDIA layer
manifest warnings and the new compiler's nonfatal upstream doctest C5285
`std::tuple` warning remain recorded.
Manual mouse/keyboard testing of every editor panel was not performed; editor
callers were migrated and checked through builds, core tests, engine lifecycle
hooks and independent source review. No check for Jolt or C# was run, because
neither feature is implemented in this milestone.

## Commits

Local commits through the reviewed implementation/test tip follow. The final report commit is additional and is identified in the completion message. No commits were pushed.

- c7863a7 test: add behavior-neutral ECS baseline measurement hooks
- 5da889d test: allow panel-free baseline timing runs
- c481fc3 refactor: extract GPU-free scene and player collision core
- ebab4e6 refactor: freeze ECS identity component and lifecycle contracts
- 0cfe93a refactor: implement registry scene queues snapshots and engine lifecycle
- 5d96892 refactor: migrate core gameplay queries to registry components
- f3ece1e refactor: emit ECS renderables and preserve player portal lifecycle
- a135948 merge: integrate registry gameplay and rendering callers
- 2a5ba0c test: cover frozen ECS lifecycle and identity contracts
- b6eb1ed test: add scene format roundtrips and baseline collision goldens
- 2cd7228 Add staged ECS scene format and atomic serialization
- 53dca09 Migrate editor and scene lifecycle IO to typed entities
- 5755a4e merge: integrate typed editor and staged scene serialization
- 15cd46d merge: integrate independent ECS milestone tests
- c53312a test: compare saved state by identity and avoid Windows near macro
- 06457b6 Preserve v2 settings values and validate serialized JSON
- ee334a2 Align legacy material and engine document validation
- a6bf545 test: add reproducible ECS gate capture and timing checks
- 1475258 test: verify restores clear pending work and dirty changed entities
- c63e291 fix: complete allocator edge and ECS runtime validation hooks
- 4237221 test: stage malformed scene fixture for engine lifecycle gate
- 849bb07 test: report baseline allowance in performance gate summary
- b2bcdf4 fix: retain portal snapshots and invalidate retired GPU cache resources
- 294000e test: exercise queued destruction while iterating a registry view
- ce372b2 test: preserve required value components while removing optional tags
- c71943d fix: retain required core components on detach requests
- a188783 fix: reject runtime ID exhaustion before scene replacement
- adda16e test: exercise transactional runtime ID exhaustion
- 44cd925 test: reject Vulkan messenger errors without VUID text
- e60aeb6 test: defer hierarchy setters issued during destruction callbacks
- 89cabeb merge: complete validated ECS registry caller and v2 format checkpoints
- 9478e69 test: retain Windows scratch path across environment lookups
- 855ce1a fix: finish finite scene teardowns and defer world transform conversion
- bb0de5f fix: isolate command batches from pending-target queue erasure
- 393274b test: regress large teardown and ordered queued world transforms
- 5c5bb10 test: keep commands appended during application in the next batch
- e2bcc9c test: require stale clear handles without prescribing an epoch change
