# Mirabilis

Mirabilis is a C++20/Vulkan engine prototype for editor-authored scenes, Source/Quake-style movement, and momentum-preserving player portals. It combines an EnTT scene system, Jolt rigid-body physics, and C# gameplay scripts that can reload while the scene keeps running.

[Watch the project demo](https://kennynguyen216.github.io/portfolio/media/mirabilis-milestone2-demo.mp4) | [View my portfolio](https://kennynguyen216.github.io/portfolio/) | [C# scripting guide](docs/scripting.md)

## Latest captures

### Real-time shadow pass

| Before | After |
| --- | --- |
| ![Sponza before the shadow pass](docs/media/mirabilis-before-shadows.png) | ![Sponza after the shadow pass](docs/media/mirabilis-after-shadows.png) |

[Watch before shadows](docs/media/mirabilis-before-shadows.mp4) · [Watch the shadow pass](docs/media/mirabilis-after-shadows.mp4)

### Timed movement course

[![Mirabilis timed movement course](docs/media/mirabilis-movement-time-trial.jpg)](docs/media/mirabilis-movement-time-trial.mp4)

Click the preview to watch the run.

## Highlights

### Vulkan rendering

- Renders GPU mesh and material pipelines with glTF/GLB scene loading.
- Manages Vulkan command buffers, synchronization, descriptor sets, dynamic rendering, and swapchain presentation.
- Includes real-time shadows, ambient occlusion, screen-space GI, optional Lumen-inspired world GI, and a software path-tracing reference mode.
- Select GPU software distance-field GI with `MIRABILIS_LUMEN_HARDWARE=0`, independently of hardware sun shadows (`MIRABILIS_IQ_RT_SHADOWS`). Software GI defaults to screen-first tracing and requires complete mesh distance-field bakes. Hardware GI retains its direct-world-tracing default on ray-query devices; it groups material ranges into reusable mesh BLASes and one TLAS instance per transform, preserving per-geometry hit materials and moving opaque bodies without distance-field bakes. Devices without ray queries use software GI.
- Relights the surface cache in bounded strips in the frame command buffer, retaining sky/emission pages during sun or punctual-light edits. Raster, cache and reference lighting share punctual-light attenuation.
- Raster GI borrows immutable CPU mesh/texture sources, stores 20-byte triangle references for albedo voxelization, and uploads only emitter geometry. Both distance-field and hardware GI avoid full CPU triangle/BVH and packed texture copies; the software reference and CPU visibility diagnostics keep their full payload. Voxelization reuses subdivision scratch across triangles.
- Offers optional smooth/rough opaque scene reflections with GGX visible-normal sampling and separate static-view temporal history. Moving cameras, moving geometry and lighting changes reject that history. Cache-lit hits and scalar-material direct fallback are approximations; textured uncached hits remain conservative.
- Includes shadowed point/spot lights, two-sided cloth lighting, material AO/specular controls, approximate thin-surface subsurface and screen-space glass refraction.
- Offers optional cel shading, multi-scale HDR bloom, and ACES/Reinhard/PBR Neutral tone mapping.
- Uses CPU-precomputed normal matrices in the prepass and instance batches, fast push-constant transforms for ordinary draws, with opt-in static-mesh GPU cluster culling for the main camera.
- Includes a compute-rendered background and ongoing portal-rendering experiments.

### Linked portals

- Uses stencil-mask passes to restrict linked-world rendering to each visible portal surface.
- Includes an experimental recursive stencil path for a linked portal visible through another portal.
- Places portals on eligible walls, carves portal-shaped openings from collision, and rejects overlapping or invalid placements.
- Transforms player position, velocity, and view orientation through linked portal frames while preserving momentum.
- Keeps the player’s blue/orange portal-gun pair transient, while scene-authored freestanding portal links persist independently. Up to seven linked editor pairs can be placed and loaded per scene.

### Scene editor and tooling

- Uses EnTT entities with versioned identities, hierarchical transforms, and deferred scene changes.
- Provides a dark Qt host with an embedded Vulkan viewport, breadcrumb navigation, asset cards and asynchronous texture thumbnails, scene tree, and selection/duplicate/delete actions.
- Exposes rendering styles, bloom, tone mapping, world GI, scene reflections, experimental screen probes, GI presets, primitive material properties, and point/spot light authoring in Qt. `graphics_showcase.json` demonstrates the new rendering features.
- Keeps ImGui for profiler, render settings, and component diagnostics; advanced component controls remain there during the Qt properties migration.
- Supports translate, rotate, and scale gizmos with optional snapping.
- Exposes editable box colliders and gameplay roles for scene-authored objects.
- Saves and loads scene hierarchies, transforms, colliders, rigid bodies, scripts, imported assets, and gameplay roles in version 2 JSON scenes.

### Jolt physics

- Integrates static and dynamic rigid bodies, gravity, contact, friction, restitution, forces, and impulses.
- Exposes body settings in the inspector and synchronizes simulated transforms with scene rendering, interpolation, and motion history.
- Restores the authored scene when leaving or resetting Play, and keeps visual children attached to moving bodies.

### C# scripting and hot reload

- Hosts .NET 10 scripts with `OnCreate`, `OnFixedUpdate`, `OnUpdate`, and `OnDestroy` callbacks.
- Exposes entity references, transforms, input, logging, and rigid-body commands through a managed API, with editable public fields in the inspector.
- Builds saved user-script changes in the background and reloads them during Play, preserving the live scene, physics state, and supported public/private fields.
- Keeps the previous working scripts running when compilation or reload fails. ScriptCore and native C++ changes require a restart.

### Movement and time trials

- Runs Source/Quake-inspired movement on a fixed timestep with acceleration, friction, air control, jump buffering, and bunny hopping.
- Supports surf ramps and scene-authored spawn points, start triggers, finish triggers, and a timer HUD.

## Technology

- **Languages:** C++20, C# / .NET 10
- **Graphics:** Vulkan, GLSL
- **Scene entities:** EnTT
- **Rigid-body physics:** Jolt Physics
- **Platform and input:** SDL2
- **Editor UI:** Qt 6 Widgets; ImGui diagnostics and ImGuizmo viewport tools
- **Assets and math:** fastgltf, simdjson, GLM, stb_image
- **Build:** CMake

EnTT, Jolt, SDL2, fmt, vk-bootstrap, VMA, GLM, ImGui, ImGuizmo, fastgltf, simdjson, and other supporting libraries are included under `third_party/`. EnTT provides entity storage and Jolt provides rigid-body simulation; Mirabilis implements their scene lifecycle, editor, rendering, and gameplay integration, alongside its movement, portals, and .NET host.

## Build and run

### Requirements

The current build targets Windows x64. Install:

- Git
- CMake 3.21 or newer
- Visual Studio with the **Desktop development with C++** workload and a C++20-capable compiler
- The [Vulkan SDK](https://vulkan.lunarg.com/sdk/home/)
- The x64 .NET 10 SDK, including the Windows x64 hosting pack
- Qt 6.8 or newer, MSVC x64 Widgets SDK (tested with 6.12); CMake deploys its runtime plugins

A Vulkan 1.3-capable GPU is required for the editor. The .NET SDK includes the runtime needed by scripts; the SDK is also required to compile scripts and build the native host.

The Vulkan SDK installer should configure the `VULKAN_SDK` environment variable. Restart your terminal after installing it.

### Clone

Keep the checkout and build output outside OneDrive or other synced folders. For example:

```powershell
git clone https://github.com/kennynguyen216/Mirabilis.git C:/dev/Mirabilis
cd C:/dev/Mirabilis
```

### Configure and build

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.12.0/msvc2022_64
cmake --build build --config Debug --parallel
```

CMake builds and deploys ScriptCore, the sample scripts, and the hosting DLL beside the executable automatically.

Alternatively, with Ninja installed, use the shared presets from a Visual Studio
Developer PowerShell:

```powershell
cmake --preset windows -DCMAKE_PREFIX_PATH=C:/Qt/6.12.0/msvc2022_64
cmake --build --preset debug --parallel 2
ctest --preset debug
```

Use `release` for Release builds/tests. The `windows-core` configure preset and
`core-debug` / `core-release` build/test presets run the CPU and managed checks
without Vulkan or UI dependencies. Keep machine-specific settings in the ignored
`CMakeUserPresets.json`. Build configurations sequentially: they share runtime
deployment and generated shader outputs.

### Run

```powershell
Push-Location .\bin\Debug
.\engine.exe
Pop-Location
```

The Qt editor opens by default. Its content browser navigates the asset root,
filters the current folder, previews selected images, opens scene JSON files,
imports glTF/GLB models, assigns a selected image as base color, and reimports
models. Asset changes apply on the engine thread in Edit mode. Reimport builds
replacement resources before swapping; failed parses/textures retain the old model.
Save/Discard/Cancel protects unsaved scenes on open and close.

Use **Debug > ImGui diagnostics** for profiling, rendering controls and the
advanced component inspector. Qt edits names, transforms, primitive PBR materials,
and point/spot lights. Its **Rendering** tab controls cel shading, bloom, tone
mapping, render scale and GI quality. **Ultra / half** uses Peak's 8 rays and
96 steps at half resolution with depth/normal-aware reconstruction; it trades
fine GI detail for speed. GPU cluster culling is experimental and off by default.
Physics and scripts remain in the ImGui diagnostics inspector.
`MIRABILIS_ASSET_ROOT` overrides the browser root. The existing scene directory
is `assets/scenes`; browsing does not rename or move files and break references.

`MIRABILIS_QT_EDITOR=0` runs the SDL host. Configure with
`-DMIRABILIS_BUILD_QT_EDITOR=OFF` to omit the Qt dependency entirely. Bounded
GPU tests use SDL unless Qt is explicitly enabled. Press `Esc` or close to exit.

### Original 4K Sponza and curtains

Download Intel's original Sponza and Colorful Curtains glTF assets with:

```powershell
python scripts/fetch_sponza_hq.py
```

The installer fetches only the glTF files, geometry, referenced PNG textures and
credits from [Intel's official packages](https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html).
It verifies archive CRCs, resumes by checking completed files, and retains all
80 original 4096x4096 textures without resizing. The selected files occupy about
2.27 GiB; the large downloads stay in the ignored `assets/sponza_hq/` directory.
The source licenses and attribution are retained beside both models.

Two new scenes are available through the Qt content browser:

- `sponza_hq_preview.json`: 4K base-color maps and all original 4K curtain maps,
  with existing 2K normal/roughness maps for the building. This optional profile
  references the original assets; it does not modify or resize the downloads.
  It is validated on the 6 GB RTX 3060 Laptop GPU.
- `sponza_hq_full_4k.json`: original 4K maps for both building and curtains. The current
  RGBA8 GPU uploads need about 6.67 GiB for these textures alone, before meshes,
  render targets and other resources. It fails with device-memory exhaustion on
  the 6 GB GPU; GPU texture compression or streaming is needed for that card.

The original `sponza_showcase.json` and its 2K assets remain unchanged so prior
benchmark results still refer to the same workload. When opening the preview,
the hanging red/blue curtains and green banners come from Intel's add-on package.

### Profiling and shared services

The **Profiler** debug window is available with the editor's debug panels. CPU
profiling is off by default. Enable it to inspect the last completed host frame,
inclusive operation times and call counts, and a bounded frame history. Timings
include waits; they are not CPU utilization measurements. The completed GPU
submission view breaks down shadows, cache lighting/radiosity, forward shading,
GI, reflections, portals and post-processing. Its frame span includes the child
passes; do not add the frame row to their sum. Queries are read after the owning
frame fence without waiting for unfinished query results.

Set `MIRABILIS_GPU_FRAME_LOG` to a writable CSV path to export submitted frame IDs,
GPU pass times, CPU fence/acquire/preparation/record/submit/present times and
representation rebuild counts. Bounded runs drain the final delayed submissions
outside the measured loop; editor shutdown also drains its pending submissions. `MIRABILIS_GPU_SCOPES=0` disables the additional pass
queries for overhead controls. Existing AO/GI timers remain available.

Heap usage/budget is sampled every 60 submitted frames. `VK_EXT_memory_budget`
is enabled when available; unsupported devices report VMA estimates. Driver
budget usage, allocated bytes and residency are different measurements. The
reported peak is sampled and may miss transient allocations. Detailed allocation
statistics remain available on request; scratch capacity/growths show retained
CPU memory.

Use **Start trace**, **Stop trace** and **Save trace** to export
`mirabilis-profile.json` in the working directory. Captures retain at most 65,536
events and report dropped events when full. Starting a new capture discards the
previous capture. Export copies a consistent snapshot before writing it; tracing
adds synchronization overhead and should be disabled for ordinary performance
comparisons.

For an unattended trace including startup, set an output path whose parent exists:

```powershell
New-Item -ItemType Directory -Force .\tmp | Out-Null
$env:MIRABILIS_PROFILE_TRACE = Join-Path $PWD "tmp/profile.json"
Push-Location .\bin\Debug
.\engine.exe
Pop-Location
Remove-Item Env:\MIRABILIS_PROFILE_TRACE
```

`MIRABILIS_PROFILE=1` enables CPU summaries without tracing. Trace JSON contains
CPU duration events and counters, with timestamps/durations in microseconds.
GPU counters are reported when completed results are received; they do not claim
to locate GPU work on the CPU timeline. A failed requested trace export reports
an error rather than claiming a successful capture.

The focused lighting checks and matched HQ Sponza/curtains benchmark are:

```powershell
python tests/lighting_visibility_gate.py --configuration Debug --output C:/temp/lighting-visibility
python tests/cache_lookup_gate.py --configuration Debug --output C:/temp/cache-lookup
python tests/forward_depth_gate.py --configuration Debug --output C:/temp/forward-depth
python tests/software_visibility_budget_gate.py --configuration Debug --output C:/temp/visibility-budget
python tests/reflection_motion_gate.py --configuration Debug --output C:/temp/reflection-motion
python tests/lighting_benchmark.py --baseline C:/path/to/frozen-baseline --output C:/temp/sponza-lighting
```

`MIRABILIS_SURFACE_CACHE_LINEAR_LOOKUP=1` disables exact-direction group skips
for an exhaustive reference. `MIRABILIS_SURFACE_CACHE_BOX_LOOKUP=1` restores the
full mesh-box candidate index. The default index uses occupied capture tiles,
including complete texel footprints and conservative depth/blend margins. Its
depth readback runs on recapture, without adding a per-frame readback. The cache lookup gate compares finite HDR captures
exactly, including rotated/mirrored geometry, reflections and cells over 64 cards.
`MIRABILIS_PREPASS_REJECT=0` disables hidden-fragment rejection for image and
timing controls. `MIRABILIS_REFLECTION_HISTORY=0` disables temporal reflection
reuse. Reflection reprojection validates previous depth, normals, roughness and
hit identity/position; scene edits, camera cuts and unfinished relights reject
history. The motion gate reads actual GPU sample counts after the bounded run.

The baseline must contain `bin/Release/engine.exe`, its runtime dependencies,
compiled `shaders`, and the same `assets`. Benchmark profiles preserve the
hardware direct-world and software screen-first policies, with 32 cache sky
samples in both revisions. Each profile gets three alternating before/after
pairs, 900 warmup and 300 focused samples. The candidate exports complete GPU
submissions; static windows reject representation rebuilds. Visibility and
coverage fixes may change images, so their timing changes are not automatically
equal-image optimization gains. A separate direct-hit hardware GI baseline is
still needed to compare cached GI against directly shaded RT GI.

Use `--screen-trace on` to compare both tracing backends with the same screen
policy. `--baseline-gpu-scopes` exports timings from a baseline that supports
them. For a same-binary optimization comparison, point `--baseline` at the
current checkout and add `--reference-controls`; this requires identical
executable/shader hashes. An extra reference repeat measures run-to-run HDR
variation: optimized image errors must stay within three times that variation,
with a 0.5% hard mean-relative-error ceiling and small rounding floors. Small
regression fixtures still require exact equality. `--profiles hardware software`
limits the matrix to cached GI; `--scene sponza_hq_full_4k.json --reflections`
includes all-4K textures and scene reflections.

`FrameClock` and `Profiler` live in the GPU-free core. The frame clock preserves
unclamped wall time and caps simulation deltas at 100 ms; reload resets its time
origin. `VulkanEngine::tick_simulation` shares the existing fixed-step update and
accepts input-focus permission from the host without pumping events or calling
ImGui. The Qt host pumps on the owner thread before simulation and shares these
services with the SDL fallback.

The core task scheduler uses pinned worker fibers, priority queues, nested
`TaskGroup` waits and `parallel_for`. Windows x64 uses a separate MASM switch
preserving nonvolatile registers, floating-point controls and stack metadata.
Processes with hardware shadow stacks use Win32 fibers automatically. Waiting
jobs retain their scratch storage; exhausted fiber pools execute descendants on
the worker root stack with a checked nesting limit. Exceptions propagate after
joining the group, cancellation skips queued work, and shutdown drains accepted
jobs and their children. Callers must join groups before captured references die;
cyclic dependencies are invalid. A task must not shut down its own scheduler.

`VirtualArena` reserves address space and commits pages on demand, supports
checked alignment/reallocation and scoped rewinds, and can decommit on reset.
`FixedBlockPool` reuses aligned slots and rejects invalid/double releases.
`SizeClassResource` serves 16–2048-byte allocations from pools, with heap fallback
for larger/over-aligned requests. Task queues use these pools; larger tangent jobs
use per-task PMR scratch with heap fallback when its arena fills. Arena reset
never runs destructors: PMR containers must be destroyed first. Profiling reports
committed task scratch and small-pool memory; heap fallback allocations are not
included in those figures.

The glTF loader decodes only referenced images in batches of two on workers,
reuses one decode for sRGB/linear GPU variants, and performs Vulkan uploads on the
owner thread. Each decoded RGBA source is limited to 256 MiB; unsupported or failed
images use the checkerboard on initial load. Embedded image views honor their byte
offsets and lengths. This reduces duplicate decoding and temporary allocation;
no frame-rate or load-time speedup is claimed without a dedicated benchmark.

After building, run the profiling GPU gate sequentially with other GPU checks:

```powershell
python tests/profiling_gate.py --output C:/temp/mirabilis-profile-check
```

Use a fresh output directory. The gate checks engine trace events, steady scratch
capacity, unchanged scene files, and identical profiling-on/off raster captures
in a static scene and during player portal traversal. Add `--configuration Release`
to exercise the Release binary.

The Qt integration gate uses disposable assets and saved scenes:

```powershell
python tests/qt_editor_gate.py --output C:/temp/mirabilis-qt-check
```

It checks import, shared color-space decodes, embedded image offsets, texture
assignment/failure, transforms, save/reopen, stale identities, reimport, dock/
window lifecycle behavior, ImGui diagnostics in Edit/Play and close Cancel/Discard. Native CTest coverage also exercises both fiber
backends, pool exhaustion, nested tasks, cancellation, shutdown and allocator
alignment/overflow/OOM paths.

### Comparing engine performance

The world-GI correctness gate and focused benchmark use fresh output directories:

```powershell
python tests/lumen_gate.py --output C:/temp/mirabilis-lumen-check --configuration Debug
python tests/software_gi_gate.py --output C:/temp/mirabilis-software-gi-check --configuration Debug
python tests/physics_gate.py --repo . --software-gi --output C:/temp/mirabilis-software-physics --configuration Release
python tests/reflection_history_gate.py --output C:/temp/mirabilis-reflection-history
python tests/trace_texture_gate.py --output C:/temp/mirabilis-trace-textures
python tests/ray_geometry_gate.py --output C:/temp/mirabilis-ray-geometry
python tests/lumen_benchmark.py --output C:/temp/mirabilis-lumen-timing
python tests/lumen_benchmark.py --output C:/temp/mirabilis-relight-timing --hitches
```

The gate checks point/spot bounce, darkness, offscreen emissive reflections,
roughness ownership, white-furnace energy, compact-upload parity and both portal
paths with Vulkan synchronization validation. It requires actual hardware world
GI, finite HDR output and unchanged authored scene selection. The benchmark
alternates three runs of plain SSGI, world GI, probes and reflections at a fixed
camera. It rejects missing focus, fallback and unfinished cache warm-up. The
relight comparison uses a forced blocking/full-relight control and the budgeted
source-reuse path during a scripted light edit; it is not a historical binary
comparison. CSV percentiles measure render-loop work. Startup CPU BVH brute-force
validation is explicitly skipped during these timings. The GI and raster-physics
gates exercise compact production geometry; the GI gate also compares full-upload
output. The texture parity gate compares complete CPU albedo volumes. Other
bounded checks retain CPU BVH validation, as does the software reference.
`--atlas 2048` bounds cache size for memory-limited scenes;
the default is 4096. Probes remain experimental and off by default.
Use `--profiles lumen plain reflections` to measure selected modes in that order
(reversed on the second repeat).

Software GI traces distance fields in GPU compute shaders; it is separate from
the triangle-BVH software reference renderer. Start the desired scene once to
queue missing mesh geometry, run the existing baker with its Python dependencies,
then restart the scene:

```powershell
python scripts/bake_sdf.py --queue assets/sdf/queue assets/sdf/cache
python tests/lumen_benchmark.py --compare-backends --scene sponza_hq_preview.json --camera "0 2.5 0 -0.05 1.5708" --warmup 900 --atlas 2048 --output C:/temp/mirabilis-gi-backends
```

This comparison enables screen tracing and uses 32 cache-sky samples for both
backends. It rejects incomplete distance fields, unready lighting and missing
focus. Distance fields approximate geometry, so backend FPS comparisons are
quality tradeoffs rather than equal-image optimizations. The software gate checks
punctual bounce, darkness, both portal paths and independence from the sun-shadow
backend. The existing hardware GI gates explicitly select hardware tracing.
Set `MIRABILIS_SSGI_SCREEN_TRACE=1` for hardware screen-first tracing, or `0` for
software direct-world tracing. The defaults preserve the faster measured hardware
path while making the software hierarchy explicit. Dedicated scene reflections
retain their optional hardware path. Mesh distance-field GPU images are released
after composition; static-field rebuilds reload them from the bake cache. This
reduces retained GPU memory at the cost of extra loading during rebuilds.
`MIRABILIS_SDF_MAX_DISTANCE_VOXELS` overrides the stored distance clamp (2–64,
default 8) for tracing experiments; larger ranges increase composition work and
do not guarantee faster tracing.

Awake opaque physics bodies use a separate 96-cubed software distance field.
Motion updates this field in the frame command buffer without recapturing the
static atlas or rebuilding its distance field. Empty regions are cleared on
each update, and inactive mesh volumes retire after the frame fence. Moving
meshes still require their geometry bake; their hit lighting currently uses
sun/punctual visibility and the first material's diffuse factor. A widely spread set of bodies shares this
bounded field and therefore loses detail. Sleeping bodies retain dynamic-field
membership and stop updating when their pose is unchanged; sleep/wake does not
rebuild static caches. Editor geometry/material edits can still rebuild them.
Per-instance cache membership reaches prepass, screen gathers and portal shading
so moving surfaces do not borrow cached sky/emitter lighting from nearby walls.
`MIRABILIS_DYNAMIC_GI=0` disables the moving software field for diagnostics.
Static software and hardware gathers use specialized pipelines that remove
unused backend and moving-field code. Active software moving geometry retains
the generic gather. `MIRABILIS_GI_GENERIC_WORLD=1` selects the generic shader
for same-binary image and performance comparisons.

Software rays escape their origin shell with a bounded offset that stops when
it enters a neighboring surface after allowing for the first voxel's filtered
shell uncertainty. Sub-voxel geometry remains approximate. Outside baked volume
bounds, a projected-distance lower bound avoids artificial distance dips.
Diffuse gathers and radiosity use per-pixel/texel low-discrepancy sequences;
integer-wrapped angular phases preserve precision in long sessions.
`MIRABILIS_GI_RANDOM_SAMPLING=1` selects the previous random-sampling control.
Relighting uses short, validated GI history instead of resetting the entire
image each edit. Disabling every cached light source clears irradiance and bounce
history in one GPU clear batch. Reflections validate each hit and cap history at four samples
during geometry/lighting changes, retaining sixteen in a stable scene.

Run the moving-field, close-gap, lights-off and noise checks sequentially:

```powershell
python tests/dynamic_gi_gate.py --configuration Release --output C:/temp/mirabilis-dynamic-gi
python tests/reflection_motion_gate.py --configuration Release --output C:/temp/mirabilis-reflection-motion
python tests/reflection_history_gate.py --configuration Release --output C:/temp/mirabilis-reactive-reflections
python tests/gather_backend_gate.py --configuration Release --output C:/temp/mirabilis-gather-backends
```

`tests/dynamic_lighting_benchmark.py --runs 3 --output C:/temp/mirabilis-dynamic-sponza`
measures all-4K Sponza with banners, one oscillating rigid body and one continuously
edited point light. It uses 900 warmup frames and 300 measured frames per run,
with reflections enabled. This stress workload is separate from the static
before/after comparison below.

For a matched comparison across renderer and shader changes, preserve the old
Release executable and all of its generated `.spv` files before rebuilding:

```powershell
python tests/lumen_comparison.py --baseline-engine engine-before.exe --baseline-shaders C:/temp/old-shaders --output C:/temp/mirabilis-lumen-comparison
```

The old executable must be beside `bin/Release/engine.exe`. This harness alternates
three old/new pairs at the same executable path, restores the current executable
and shaders on exit, and checks hashes, focused samples, cache convergence and HDR
parity. Its defaults target HQ Sponza with banners, a 2048 atlas limit and 900 warm-up
frames. Run it without other engine instances or builds. The Windows runner
activates the benchmark window; any measured loss of focus still invalidates a run.

Build both checkouts in Release with matching assets and generated shaders, then
run GPU workloads sequentially with the comparison harness:

```powershell
python tests/benchmark_comparison.py --baseline C:/dev/Mirabilis-baseline --candidate . --output C:/temp/mirabilis-comparison
```

Use a fresh output directory. The harness verifies matching shader/asset bytes
and rendered captures, primes caches, then alternates five old/new runs in the
sandbox, Sponza and a portal scene. Each run uses a fixed 1600×900 window, the
scene's saved quality settings, 120 warm-up frames and 600 measured frames with
the cap/panels/profiling off. It requires input focus for every measured sample.
Window focus changes invalidate a run. No other engine instance or build should
run during measurement.

Results include wall-frame FPS, run variance, descriptive paired bootstrap
intervals, render-loop duration percentiles, startup/glTF log intervals, GPU
clock/temperature samples and input hashes. Loading numbers include uploads and
use warmed filesystem/driver caches. Percentiles from the engine CSV measure
render-loop work rather than independent presentation timestamps. The comparison
uses the same SDL host on both revisions; Qt editor overhead is excluded. Small
changes or intervals crossing zero are reported without claiming a speedup.

## Status

ECS scenes, Jolt physics, C# scripting, and live script reload are implemented and validated. Mirabilis remains an active engine and rendering prototype, with experimental lighting paths and a deliberately limited gameplay API.

Dynamic rigid bodies currently use root-level box colliders. The player retains its dedicated movement controller, and rigid bodies do not yet travel through portals. Hot reload preserves the six supported field types; complex object graphs and collections are not migrated. See the [scripting guide](docs/scripting.md) for the supported API and reload behavior.

Ambient occlusion has a persistent **Globally Enabled** switch and quality/
blur preferences in Render Settings. These are saved in SDL's per-user
Mirabilis directory (`ambient_occlusion.cfg`) and shared by Debug and Release.
Loading a scene does not change them. Scenes without an `ssao` block use the
project defaults (radius 0.75, normal offset bias 0.075, intensity 1, power 1.5).
Enable **Override For This Scene** to edit and save level-specific values;
**Reset to Project Defaults** removes that override on the next scene save.
Existing scene `ssao` blocks remain explicit overrides.


The graphics GPU gate uses disposable scenes to check cel/bloom output, light
shadows, glass, stencil/offscreen portal transparency, and exact visible-image
parity with conservative cluster culling. Run it sequentially with other GPU tests:

```powershell
python tests/graphics_gate.py --configuration Debug --output C:/temp/mirabilis-graphics-check
```

The gate requires NumPy and Pillow, as do the existing image-validation tools.

Referenced glTF textures can use optional BC7 color/data and BC5 normal-map caches
to reduce GPU memory at the original pixel dimensions. Install Microsoft's
[DirectXTex texconv](https://github.com/microsoft/DirectXTex/releases), then run:

```powershell
python scripts/cache_textures.py assets/sponza_hq/main_sponza/NewSponza_Main_glTF_003.gltf assets/sponza_hq/pkg_a_curtains/NewSponza_Curtains_glTF.gltf --texconv C:/tools/texconv.exe --gpu 0 --report C:/temp/texture-quality.json
```

This prepares the all-4K scene. Substitute
`assets/sponza_hq/runtime_6gb/Sponza_4K_Color.gltf` for the preview profile.
Choose the texconv adapter index for your GPU. The compiler writes ignored `.mbc`
caches beside the original images, with full mip chains and source/payload
checksums. Base-level quality checks accept color/data at 35 dB RGB PSNR or better
and at most 1/255 mean alpha error. BC5 normals must stay below 1 degree mean and
5 degrees p99 angular error after reconstruction. Textures that fail stay on the original RGBA path;
alpha-masked color textures always use the original to preserve cutout coverage.
Missing, stale, corrupt, role-incompatible or unsupported caches also fall back.
Set `MIRABILIS_TEXTURE_CACHE=0` for an uncompressed comparison. Originals are
retained, including the CPU path tracer's reference texture bytes.

```powershell
python tests/texture_cache_gate.py --configuration Debug --texconv C:/tools/texconv.exe --gpu 0 --output C:/temp/mirabilis-texture-check
```
