# Milestone 1 gate measurements

Use an outside-OneDrive build and an engine executable in the repository's
`bin/Debug` or `bin/Release`. Run GPU commands sequentially. Every invocation
requires a fresh output directory; prior evidence is never overwritten.

```powershell
python tests/ecs_gate.py manifest --output C:/dev/mirabilis-work/gate/manifest
python tests/ecs_gate.py collisions --output C:/dev/mirabilis-work/gate/collisions
python tests/ecs_gate.py timings --output C:/dev/mirabilis-work/gate/timings --baseline C:/dev/mirabilis-work/baseline
python tests/ecs_gate.py gate-validation --output C:/dev/mirabilis-work/gate/validation
```

`--repo` selects another checkout. `validation` repeats the original four baseline
Debug cases without the new ECS hooks; `gate-validation` adds the ECS lifecycle
assertion and cache-map clearing markers, copying scenes to scratch destinations.

The Release timing protocol uses the scene's saved renderer/GI settings, fixed
1600x900 window, closed panels, camera `0 2.5 12 -0.08 0` for sandbox and
`0 2.5 12 0 0` for Sponza, 120 warm-up plus 600 measured frames, three runs,
validation off and frame cap disabled. The measured interval is steady-clock
loop start to start, including the preceding present/wait. The engine requests
MAILBOX with FIFO fallback in both baseline and gate. All 600 samples must retain
input focus. Baseline medians/variance determine the allowed slowdown exactly as
the milestone prompt specifies.

The baseline GPU was RTX 3060 Laptop GPU, NVIDIA driver 555.97 / Windows
32.0.15.5597, Windows Balanced power scheme. Ray-query shadows were available and
exercised. The four original Debug cases are mode/resize/minimize cycle (20 frames),
lighting-calibration to Sponza scene replacement (100), authored portal traversal
(200), and player portal placement (180). The baseline had zero VUID or validation
error lines; the existing NVIDIA loader layer-file-version warning was retained.

The inherited PowerShell 7 `PSModulePath` must be removed for Python's Windows
PowerShell subprocess metadata tests, then restored:

```powershell
$savedModulePath=$env:PSModulePath
try {
    Remove-Item Env:PSModulePath -ErrorAction SilentlyContinue
    python -m unittest discover -s tests
} finally { $env:PSModulePath=$savedModulePath }
```

This is an environment correction, not a skipped test. The corrected baseline
passed 170 tests. Capture scripts never save into `assets/scenes`.
