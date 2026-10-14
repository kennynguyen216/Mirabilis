# Milestone 2 validation

The physics tests are written from `docs/physics_contracts.md` and the milestone
requirements. They link the GPU-free core and can run through the existing
`mirabilis_core_tests` CTest target in Debug and Release. They cover native body
counts/geometry, editor synchronization, public transform ownership, offset
colliders, interpolation/history resets, sleep/wake, deferred in-step deletion,
snapshot/epoch restoration, duplication, commands, known JSON validation,
detachment of parented Dynamics and portal restrictions. The original 23 scenes
still round trip against their frozen collision fixtures. The physics demo saves
to a separate temporary file and reopens every saved component.

Build outside OneDrive, then run the existing ECS gate and the physics GPU gate
sequentially, each into a fresh output directory:

```powershell
ctest --test-dir C:/dev/mirabilis-work/build-physics -C Debug --output-on-failure
ctest --test-dir C:/dev/mirabilis-work/build-physics -C Release --output-on-failure
python tests/ecs_gate.py gate-validation --repo C:/dev/mirabilis-work/physics --output C:/dev/mirabilis-work/physics-gate/ecs-validation
python tests/ecs_gate.py collisions --repo C:/dev/mirabilis-work/physics --output C:/dev/mirabilis-work/physics-gate/collisions
python tests/ecs_gate.py timings --repo C:/dev/mirabilis-work/physics --output C:/dev/mirabilis-work/physics-gate/timings --baseline C:/dev/mirabilis-work/baseline
python tests/physics_gate.py --repo C:/dev/mirabilis-work/physics --output C:/dev/mirabilis-work/physics-gate/physics
```

The physics runner copies authored scene files into scratch storage, then adds a
visual glTF child using the existing `basicmesh.glb` with dormant Static body and
collider settings. It verifies that this child creates no independent body but
adds emitted dynamic draws. Raster runs enable Lumen and ray-query shadows to
exercise their real rebuild counters; performance runs retain saved settings.
Every engine run must exit successfully without Vulkan/fatal errors and retain
`.last_scene` unchanged. Required renderer capabilities must be active; absent
capabilities fail the gate rather than being reported as passes.

The hook emits `PHYSICS phase=...` rows with frame, bodies, sdf, surface, trace,
ray-shadow, eligible, dynamic-draws, history-reset, and the four corresponding
`*-enabled` fields. It uses the existing fixed step at 1/120. Raster phases are
awake/sleep, wake at900, teleport at930, restore at960, edit-restore at980,
unchanged-restore at990, scratch-save/reopened at1020, sandbox at1050, end1080.
The runner rejects per-frame static-cache rebuilds while awake, more than one
rebuild per sleep/wake transition, unchanged Edit restoration rebuilds, missing
subtree cache eligibility changes and missing teleport/restore history resets.

Captures use the same fixed camera. Awake/sleep raster PFM files and path-trace
PFM are retained and converted to PNG with the existing `pfm_to_png.py`. Inspect
the pictures and report the actual visible differences; counters alone do not
prove whether a box receives indirect lighting or what its missing contributions
look like. Path tracing intentionally includes moving objects and rebuilds while
they move. A fourth 1080-frame trace run verifies restore batching and unchanged
Edit restoration with path tracing active. The runner's arithmetic/parser checks need no GPU:

```powershell
python -m unittest discover -s tests -p test_physics_gate.py
```

Fresh reviewed Milestone1 baseline evidence lives separately under
`C:/dev/mirabilis-work/physics-baseline`. Required performance thresholds continue
to use the original frozen `baseline`, whose logs and prior gate remain intact.
