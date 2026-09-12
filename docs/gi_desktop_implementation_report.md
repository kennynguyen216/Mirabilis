# Desktop GI implementation report

Desktop implementation completed; checkpoint evidence retained below. Controlling specification: `global_illumination_reference_design.md`, read completely. User desktop-only scope overrides the document's laptop acceptance and one-milestone stop instructions. No hardware RT, RTX, laptop implementation, cross-machine transport, commits, pushes, or stashing of user work.

## Initial repository state

- Worktree: `C:\Users\kenne\Documents\GitHub\Mirabilis`, branch `main`, HEAD `5a54b17c331ac3f9e7a1a41156f87e45f3f0615c`.
- Pre-existing tracked edits: `assets/scenes/.last_scene`, `assets/scenes/screen_space_buffer_lab.json`, `assets/scenes/shadow_showcase.json`, `shaders/input_structures.glsl`, `shaders/ssao.comp`, `src/camera.cpp`, `src/vk_engine.h`; `docs/` was untracked.
- Original tracked patch saved locally to `tmp/gi-checkpoints/preexisting.patch`; initial status to `tmp/gi-checkpoints/initial-status.txt`. User changes remain in place. The specified stray backtick in camera.cpp is intentionally removed.

## Milestone 0

Completed desktop build/raster baseline. Files: `src/camera.cpp` removes the stray backtick; `src/vk_engine.cpp` adds an opt-in bounded run using `MIRABILIS_TEST_FRAMES`, freezes the editor camera, disables test layout persistence, and exits through normal cleanup. This minimal test scaffolding is the only scope expansion.

Commands (repository root unless stated):

- `git status --short`, `git diff --binary --output=tmp/gi-checkpoints/preexisting.patch`, `git rev-parse HEAD`, `git branch --show-current`: initial state recorded.
- `cmake --build build --config Debug --parallel 6`: first sandbox run failed in MSBuild FileTracker with access denied. Authorized elevated retry passed. Log: `tmp/gi-checkpoints/m0-build.log`.
- From `bin/Debug`: `$env:MIRABILIS_TEST_FRAMES='16'; $env:VK_LAYER_ENABLES='VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT'; .\engine.exe`: exit 0, existing shadow_showcase raster scene loaded, no Vulkan errors. One deprecated layer setting warning; later tests use `VK_LAYER_VALIDATE_SYNC=1`. Log: `tmp/gi-checkpoints/m0-raster.log`.

Conceptual check: a successful compile does not establish synchronization correctness; validation must observe actual queue submissions and normal destruction. Remaining uncertainty: raster appearance still requires a visual inspection; no claim of pixel-identical baseline has yet been made.

## Milestone 1

Desktop camera/compute plumbing completed with numerical GPU checks; subjective orientation inspection remains pending. New files: `shaders/path_trace.comp`, `src/vk_engine_path_trace.cpp`. Shared files: `src/CMakeLists.txt` registers the module; `src/vk_engine.h` owns isolated trace state; `src/vk_engine_editor.cpp` adds the selector; `src/vk_engine.cpp` adds initialization/destruction, explicit draw branch and bounded mode/resize exercise; `src/main.cpp` flushes diagnostic output and suppresses unattended abort dialogs.

- `cmake -S . -B build`, `cmake --build build --config Debug --parallel 6`: passed (`m1-configure.log`, `m1-build.log`). MSVC getenv deprecation warnings only.
- From `bin/Debug`: `$env:MIRABILIS_TEST_FRAMES='16'; $env:MIRABILIS_TEST_TRACE='1'; $env:VK_LAYER_VALIDATE_SYNC='1'; .\engine.exe`: passed, exit 0 (`m1-trace.log`). RX 5700, driver integer 8388961, Vulkan 1.4.315; RGBA32F storage and compute queue supported. Nine center/edge/corner rays on a 17x13 dispatch agree with CPU to 1.2287812e-7 (threshold 2e-6).
- Initial diagnostic runs stalled after accessing unmapped readback memory. Fixed with explicit VMA map/unmap; only those test-owned processes were terminated.
- Set `MIRABILIS_TEST_FRAMES=20` and `MIRABILIS_TEST_CYCLE=1` in addition: traces, switches to raster at frame 4, back at 8, resizes to 803x457 at 10, changes render scale at 12. First run exposed an existing acquire bug: SUBOPTIMAL signals the acquire semaphore but returned without consuming it. Minimal shared synchronization correction now consumes the image before resizing. This is a justified scope expansion preserving raster output. Final result recorded in `m1-cycle.log`.

Conceptual check: primary rays use the camera basis, negative-Z forward, 70-degree vertical FOV and current draw aspect; framebuffer Y points downward. The reference display has its own transform and bypasses raster FXAA/SSAO/shadows/portals. Resources share the existing draw allocation bounds; live extent changes do not require reallocating them. Cross-vendor acceptance is pending, not claimed.

## Milestone 2

Desktop extraction and GPU record check completed. New `src/path_trace_scene.h` declares checked 128-byte triangle / 48-byte material ABIs. `src/vk_types.h` retains shared CPU mesh arrays and independent trace material values; `src/vk_engine_resources.cpp` retains upload inputs and primitive material values; `src/vk_loader.cpp` carries glTF constant factors; `src/vk_engine_scene_materials.cpp` updates edited trace factors. Trace module/header/shader add world-space baking, independent revision/hash, storage buffers and record diagnostics. Degenerate triangles, singular transforms and invalid indices are skipped. Raster descriptors and shader inputs are unchanged.

- `cmake --build build --config Debug --parallel 6`: passed (`m2-build.log`). A local edit helper initially failed because Python is not installed; equivalent PowerShell edits succeeded.
- From `bin/Debug`, same 20-frame trace/cycle/sync-validation command as M1: exit 0, no validation messages (`m2-cycle.log`). Current shadow_showcase extraction: 266 triangles, 23 materials. GPU position/material readback of 221 records: exact agreement (error 0). Camera check remains passing.

Conceptual check: a world-space triangle bake applies object transforms to positions and inverse-transpose transforms to normals. An independent content hash/revision tracks GPU validity; unsaved-editor state does not. Most likely subtle limitation: materials currently carry constant factors only, and texture fidelity remains a later checkpoint. Full record/bounds and intersection testing expands in M3.

## Milestone 3

Desktop BVH/primary-hit checkpoint completed numerically. New `src/path_trace_scene.cpp` and `shaders/path_trace_intersect.glsl` implement median splits, four-triangle leaves, checked child/leaf ranges, 64-entry iterative stacks, parallel-slab handling, Moller-Trumbore intersections, oriented geometric/shading normals and traversal counters. Existing trace header/module/shader and source CMake register buffers and debug views. No raster shader or prepass behavior changed.

- `cmake -S . -B build`, `cmake --build build --config Debug --parallel 6`: passed (`m3-configure.log`, `m3-build.log`).
- Same 20-frame trace/cycle/synchronization-validation command: exit 0, no Vulkan messages (`m3-cycle.log`). CPU BVH vs brute force: 4,096 deterministic rays including six axis-parallel directions, exact distance agreement. GPU vs CPU brute force on 221 rays: relative distance error <=1.8311805e-7; no hit/miss disagreement or traversal fault.
- Actual raster prepass depth was sampled by the diagnostic compute shader and read back. One 17x13 coverage boundary lies 0.00015258789 pixels from a projected edge, within the GPU's 1/256-pixel vertex precision. The initial comparison incorrectly omitted edges crossing the near plane; its helper now clips them before measuring. Maximum raster-vs-ray relative distance difference 0.00054521736 passes a per-triangle depth-gradient bound derived from subpixel precision, not the strict ray tolerance (the generic log label still prints 2e-6). Unexplained coverage/depth differences remain hard failures.

Conceptual check: BVH construction reorders triangle records but retains integer material references. Traversal prunes against the current closest hit, while brute force provides an independent intersection oracle. Raster vertices are quantized; tiny edge-coverage and depth-interpolation differences are expected rather than evidence of a ray convention error. Reference consulted: https://docs.vulkan.org/spec/latest/chapters/primsrast.html . Subjective high-resolution silhouette/normal review and laptop acceptance remain pending.

## Milestone 4

Desktop diffuse transport completed. Added `shaders/path_trace_transport.glsl` (PCG, cosine-weighted sampling, geometric offsets, bounded four-event maximum with two-event default). Trace shader/module/header add jitter, depth/seed controls and transport; trace module writes linear PFM and display BMP captures through mapped GPU readback. `src/vk_engine.cpp` adds opt-in end-of-test capture. `src/vk_engine_resources.cpp` accepts `MIRABILIS_TEST_SCENE` without changing the remembered user scene. New `assets/scenes/gi_open_box.json` supplies the red/green open environment box.

- A build was temporarily rejected by automatic approval review due to a usage limit; after the user requested continuation, the normal approved build path worked. No bypass was used.
- `cmake -S . -B build`, `cmake --build build --config Debug --parallel 6`: passed (`m4-configure.log`, `m4-build.log`).
- From `bin/Debug`: `$env:MIRABILIS_TEST_FRAMES='16'; $env:MIRABILIS_TEST_TRACE='1'; $env:MIRABILIS_TEST_SCENE='gi_open_box.json'; $env:MIRABILIS_TEST_DEPTH='1'; $env:MIRABILIS_CAPTURE='../../tmp/gi-checkpoints/m4-depth1'; $env:VK_LAYER_VALIDATE_SYNC='1'; .\engine.exe`: exit 0, no validation errors, 1280x720, linear mean 0.58558601888, no nonfinite pixels. Repeat with depth 2 and capture `m4-depth2`: exit 0, mean 0.58859441406, no nonfinite pixels. Both preserve all earlier CPU/GPU checks. Logs/captures use the matching names in `tmp/gi-checkpoints`.
- Agent inspected the GPU BMP captures: correct room silhouettes, lit diffuse surfaces at depth 1, additional colored bounce samples at depth 2. The single-sample result is intentionally very noisy; stable bleeding is checked after M5 convergence.

Conceptual check: max depth counts scattering events, so the loop still evaluates an environment miss after its final scatter. Lambertian BRDF/PDF cancellation leaves throughput multiplied by albedo. No raster ambient, shadow map, SSAO or FXAA enters the trace estimate. Most likely remaining subtle issue is material/normal fidelity on arbitrary imported content, beyond the flat diffuse validation scene.

## Milestone 5

Desktop progressive accumulation completed. Trace shader/module/header add RGBA32F equal-weight averaging, deterministic pixel/sample/base-seed streams, monotonic sample count, input fingerprint invalidation, exposure-independent history, exact sRGB display encoding after exposure/Reinhard mapping, sample/debug controls, GPU timestamps, linear PFM/BMP capture and capture metadata. `src/vk_engine.cpp` adds an opt-in invalidation exercise. `src/vk_engine_scene_io.cpp` suppresses remembered-scene writes during tests; test cleanup suppresses scene autosave. The loader originally updated `.last_scene` during test-scene loads; this was discovered and restored to the exact original `shadow_showcase.json` value from the saved pre-existing patch, then guarded for future tests.

- `cmake --build build --config Debug --parallel 6`: passed (`m5-build.log`; final test-persistence guard is rebuilt next).
- Same trace command with `MIRABILIS_TEST_FRAMES=128`, `MIRABILIS_TEST_SCENE=gi_open_box.json`, `MIRABILIS_CAPTURE=../../tmp/gi-checkpoints/m5-open128`: passed (`m5-open128.log`). Known constant GPU samples 1,3,8 times RGB(1,2,4) average to (4,8,16) exactly on all 221 diagnostic pixels. First-sample overwrite ignores old diagnostic history.
- Repeated identical 128-sample capture (`m5-repeat.log`): PFM SHA256 matches exactly: `01C7A2E1A9213EFA77297DB28AD50FF9FC89066C70B0C477F06FF9A613347F18`. Linear mean 0.588562701339; no nonfinite pixels. Maximum measured compute dispatch 1.78096 ms on RX 5700, driver 8388961, Vulkan 1.4.315, Debug, 1280x720, 74 triangles, depth 2. This is measured for this scene only, not an arbitrary-scene timeout guarantee.
- `MIRABILIS_TEST_FRAMES=16`, `MIRABILIS_TEST_INVALIDATION=1` with trace/open-box flags: passed (`m5-invalidation.log`). Exposure preserves sample count; seed, camera, depth, material color, visibility, transform, render scale and manual reset each return to sample 1. No scene edits are saved.
- 20-frame mode/resize/render-scale cycle: passed (`m5-cycle.log`), no Vulkan validation messages. Agent inspected the converged BMP: silhouettes and indirect color patches are stable, with expected residual Monte Carlo noise.

Conceptual check: the n-th update is `old + (sample-old)/(n+1)`; samples retain equal weight. A content fingerprint invalidates history when physical inputs change, while exposure is applied only after averaging. Cross-vendor and broad visual acceptance remain pending.

## Milestone 6

Desktop direct-light checkpoint completed. Trace transport adds explicitly sampled directional sun and emissive triangles (a rectangle is two triangles), visibility rays, separate bounce-classified direct/indirect contributions and independent running averages. Emitters are two-sided. To avoid double counting before MIS, diffuse BSDF hits of emitters after the camera vertex are suppressed because those paths are covered by next-event area sampling. Additional trace buffers/images are synchronized and destroyed with the isolated trace resources.

Scene data changes: `src/scene.h` adds emission color/strength; `src/vk_engine_scene_io.cpp` round-trips those optional fields and an independent `referenceLighting` block (sun radiance, gradient intensity, black environment). These values do not change raster shading. `src/vk_engine_scene_materials.cpp` and `src/vk_loader.cpp` retain authored/glTF emission; `src/vk_engine_editor.cpp` exposes emission only in trace mode. `src/path_trace_scene.h`, engine header/module and trace shaders carry the lighting ABI, emitter indices and component resources. `src/vk_engine.cpp` accepts a deterministic test camera. New `assets/scenes/gi_cornell_box.json` and `gi_cornell_box_dark.json` are sealed validation scenes; open-box reference lighting explicitly disables the sun.

- `cmake --build build --config Debug --parallel 6`: passed (`m6-build.log`).
- From `bin/Debug`: trace flags, `MIRABILIS_TEST_FRAMES=16`, `MIRABILIS_TEST_SCENE=gi_cornell_box_dark.json`, `MIRABILIS_TEST_CAMERA='0 2 3.5 0 0'`, `MIRABILIS_CAPTURE=../../tmp/gi-checkpoints/m6-dark`, `MIRABILIS_EXPECT_BLACK=1`, `VK_LAYER_VALIDATE_SYNC=1`: exit 0, no validation messages. Every combined/direct/indirect pixel is exactly black; no nonfinite values. Max observed dispatch 17.44788 ms.
- Repeat with `MIRABILIS_TEST_FRAMES=256`, scene `gi_cornell_box.json`, capture `m6-lit`, `MIRABILIS_EXPECT_LIT=1` (unset EXPECT_BLACK): exit 0, all prior diagnostics pass, direct mean 0.420220100520 and indirect mean 0.0542392381312, combined 0.474459331020. Component residual <=9.536743e-7 from float subtraction. No nonfinite pixels; maximum dispatch 25.95416 ms. Agent inspected the BMP: the enclosed room is lit by the ceiling rectangle, with shadows and red/green indirect tint; residual sample noise is visible.
- Shadow showcase with 20-frame trace/raster/resize cycle and capture `m6-sun-cycle`: exit 0, no validation messages; sun-lit reference and nonzero indirect output, max dispatch 22.13812 ms. Remembered scene remains `shadow_showcase.json` after test runs.

Conceptual check: an ideal directional sun must be sampled explicitly; random hemisphere rays cannot find its zero-solid-angle direction. Area sampling divides by its selection/area PDF and geometric conversion. Direct means first-visible-surface illumination (plus visible sky/emission); contributions after an additional surface scatter are indirect. Linear component captures are saved alongside the combined PFM and metadata.

## Milestone 7

Desktop material features implemented and tested: linear base-color texture filtering, interpolated UVs/vertex colors, GGX metallic-roughness reflection, perfect dielectric transmission with authored IOR, exact dielectric Fresnel/total internal reflection, and unbiased Russian roulette on longer non-delta paths. A Lambertian-only model remains selectable. GGX roughness has a 0.045 minimum; this implementation uses single-scattering isotropic GGX, not a multi-scattering or full glTF extension renderer.

Files: new `shaders/path_trace_material.glsl`, `assets/scenes/gi_material_lab.json`, `assets/textures/gi_color_test.ppm`; trace scene ABI/module/shaders and `src/vk_types.h` retain texture/vertex data; `src/vk_engine_resources.cpp` retains source RGBA8 pixels; `src/vk_engine_materials.cpp` attaches trace texture ownership; loader/material/editor/scene I/O files carry transmission and IOR. Texture decoding is explicit in the tracer, including legacy glTF UNORM uploads; raster image formats and shaders remain unchanged. A draw-input fingerprint avoids repeated static mesh baking/texture packing.

- `cmake -S . -B build`, `cmake --build build --config Debug --parallel 6`: passed (`m7-configure.log`, `m7-build.log`).
- Trace `gi_material_lab.json`, camera `0 2.5 7 -0.08 0`, depth 4, 128 frames, capture `m7-material`, synchronization validation enabled: exit 0, no validation messages. All earlier diagnostics pass. Fresnel/TIR/sRGB scalar error 1.5359765e-8; Snell refraction and paired eta-squared factors exact; uploaded sRGB texel decode error <=4.1295309e-7. GGX normal-incidence white furnace samples remain in [0,1], mean 0.94439775. No nonfinite capture pixels. Maximum dispatch 15.5862 ms.
- Agent inspected the capture: repeated colored floor texture, reflective gold block, and refracting glass block are visible.
- Sealed dark-box regression after material changes: exit 0, all components exactly black (`m7-dark.log`), maximum dispatch 17.65244 ms.

Conceptual check: GGX uses the complete BRDF and mixture PDF in `f*cos/pdf`; dielectric reflection/refraction probabilities use Fresnel and radiance-mode eta factors. Base-color texels are decoded before bilinear interpolation. References: https://www.pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory and https://www.pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission . Known material scope limits: base-color textures use repeating bilinear level-zero sampling; normal maps, metallic-roughness textures, emissive textures, texture transforms/alternate UV sets, volume absorption, rough transmission, alpha masking and transparency remain unsupported. GGX area-light paths use next-event sampling rather than MIS; narrow highlights may converge slowly.

## Milestone 8

Same-scene desktop portal transport is implemented. `shaders/path_trace_portal.glsl` intersects rectangular apertures before opaque geometry, transfers position/direction with the engine's existing rigid portal transform, and bounds total traversal independently of surface scattering. The packed portal table includes placed player portals and authored pairs. Unlinked apertures produce an explicit invalid-path diagnostic; reaching the traversal cap terminates in black. No portal networking or transport between machines exists.

The direct-light sampler also samples bounded virtual portal chains, transforms light connections back through those chains, validates the prescribed aperture sequence, and divides by its discrete selection PDF. This permits directional sunlight and external emitters to illuminate a sealed room through a portal. Uniform chain sampling is deliberately simple and can be noisy.

- `cmake --build build --config Debug --parallel 6`: passed (`m8-build.log`). A temporary automatic approval usage-limit rejection was resolved by the normal retry after the user continued.
- Every run below used `MIRABILIS_TEST_TRACE=1`, `VK_LAYER_VALIDATE_SYNC=1`, a bounded `MIRABILIS_TEST_FRAMES`, and `MIRABILIS_CAPTURE=../../tmp/gi-checkpoints/<name>`, launching `.\engine.exe` from `bin/Debug`. All exited 0 without Vulkan validation errors. The final reusable script contains these cases and explicit environment values.
- `m8-pair`: `gi_portal_pair.json`, camera `0 2.5 7 -0.08 0`, 32 samples. CPU/GPU portal point error <=7.981573e-8, directions exact; one traversal without cap. Validated before recursion/multi-pair.
- `m8-cycle`: `gi_portal_cycle.json`, camera `0 2 2 0 0`, 32 samples. CPU/GPU agree on two traversals followed by cap termination; point error <=9.7589464e-8. No hang or nonfinite output.
- `m8-multi`: `gi_portal_multi_pair.json`, camera `0 2.5 7 -0.08 0`, 32 samples. Four apertures/two authored pairs, transformed-point/direction and chain diagnostics pass. No nonfinite output.
- `m8-sun`: `gi_portal_sun.json`, camera `0 2 3.5 0 0`, 128 samples, `MIRABILIS_EXPECT_LIT=1`. Direct mean 0.013331121745, indirect 0.008437112131; max compute dispatch 33.55476 ms. Agent inspected the BMP: portal-admitted sun patch and colored indirect illumination in the closed room.
- `m8-sun-disabled`: same scene/camera, 8 samples, `MIRABILIS_TEST_PORTAL_LIMIT=0`, `MIRABILIS_EXPECT_BLACK=1`. Every component exactly black.
- `m8-emitter`: `gi_portal_emitter.json`, same camera, 128 samples, EXPECT_LIT. Direct mean 0.021075974391, indirect 0.017742421654; max dispatch 39.11868 ms. No nonfinite values.

Conceptual check: portal transforms preserve ray direction lengths and rigid-space distances. The next surface receives throughput accumulated before teleportation; teleportation consumes a portal event, not a material event. Portal-chain sampling includes the chain's discrete PDF, and ordinary direct connections reject intervening apertures. Likely subtle limitations are high variance for longer chains, tolerance effects at wall/aperture boundaries, and perfect-dielectric caustics without dedicated light-path sampling.

## Final desktop delivery

The feasible desktop implementation through Milestone 8 is complete. Validation was performed on **AMD Radeon RX 5700**, driver integer **8388961**, Vulkan **1.4.315**, Windows, Debug configuration. No NVIDIA device, laptop, hardware RT extension, shader binding table, acceleration-structure API, or cross-machine communication was used. The implementation remains a progressive reference renderer; it is not a real-time GI replacement.

### Final milestone status

| Milestone | Desktop implementation and observed checks | Remaining acceptance |
| --- | --- | --- |
| 0 | Complete: corrected camera syntax, Debug build, raster runtime baseline | Human raster appearance comparison |
| 1 | Complete: selectable standalone compute path, camera rays, image synchronization and fallback | Human rotating-camera review; laptop checkpoint 1 |
| 2 | Complete: retained geometry/material sources, packed upload, dedicated revision, record readback | Broader imported-asset coverage |
| 3 | Complete: CPU BVH/GPU traversal, debug views, brute-force oracle and raster prepass comparison | High-resolution silhouette review; laptop checkpoint 3 |
| 4 | Complete: cosine diffuse/environment paths, scattering-depth semantics, open-box captures | Longer visual convergence review |
| 5 | Complete: equal-weight accumulation, deterministic seeds, physical-input reset, exposure independence | Laptop checkpoint 5 |
| 6 | Complete: sun and emissive-area next-event sampling, separate HDR components, black/lit sealed controls | Longer quantitative convergence baseline |
| 7 | Complete within documented initial material scope: base-color texture decoding, GGX, perfect dielectric, roulette | Full glTF material conformance and difficult specular paths remain unsupported |
| 8 | Complete: same-scene aperture transfer, bounded recursion, authored multi-pair and portal-only lighting tests | Human aperture alignment/interactive placement review |

No desktop milestone is blocked. Full specification acceptance is **partial** because the explicitly excluded laptop checks and human visual acceptance have not been performed. Later hardware RT/RTX, cross-machine transport, SSGI integration and production optimizations are deferred, not silently implemented.

### Final repository and worktree state

The work remains **uncommitted on the existing `main` branch**, HEAD `5a54b17c331ac3f9e7a1a41156f87e45f3f0615c`, in the original single worktree `C:\Users\kenne\Documents\GitHub\Mirabilis`. The index was not changed. No commits, pushes, new branches, worktrees, resets, or stashes were created. Consequently these changes cannot yet be obtained by pulling a remote branch: the owner must first review and commit/publish them, or transfer the working files separately.

Original user modifications were preserved. Git blob hashes were checked against the initial saved patch for `.last_scene`, both user scene JSONs and both user shaders; all match. The pre-existing SSAO-disabled and FXAA 0.10/0.30 settings/comments remain in `vk_engine.h`. Only the explicitly specified camera backtick was removed; its final content matches HEAD (Git can still list it due to working-copy line-ending metadata). `tmp/pdfs/` was pre-existing and untouched. The controlling design was initially untracked and was read in full before implementation; its original specification remains, with a desktop addendum appended.

### Every implementation file changed and why

Paths are relative to the repository. Entries marked new were created for this work. Pre-existing unrelated edits are listed separately below.

| File | Reason |
| --- | --- |
| `.gitignore` | Ignore only local `tmp/gi-checkpoints/` artifacts |
| `src/CMakeLists.txt` | Register trace integration and CPU BVH modules |
| `src/camera.cpp` | Remove the controlling specification's stray backtick; no other camera behavior change |
| `src/main.cpp` | Flush unattended diagnostics and suppress test abort dialogs |
| `src/vk_engine.h` | Own isolated trace mode, buffers/images, settings, revisions, diagnostics and timing |
| `src/vk_engine.cpp` | Trace lifecycle and explicit render branch; minimal acquire-SUBOPTIMAL semaphore correction; bounded validation, capture, mode/resize/minimize and invalidation hooks |
| `src/vk_engine_path_trace.cpp` (new) | Compute resources, descriptors, scene bake/cache/upload, controls, synchronization, timing, GPU/CPU comparisons, HDR and display captures |
| `src/path_trace_scene.h` (new) | Checked packed CPU/GPU scene ABI and CPU oracle declarations |
| `src/path_trace_scene.cpp` (new) | CPU BVH build, intersections, deterministic brute-force validation |
| `src/vk_types.h` | Shared immutable CPU mesh/texture ownership and independent trace material factors |
| `src/vk_engine_resources.cpp` | Retain mesh/texture upload sources and primitive trace factors; opt-in test scene selection |
| `src/vk_engine_materials.cpp` | Retain color texture ownership for tracing without changing raster descriptors |
| `src/vk_loader.cpp` | Retain glTF constant color/metal/roughness/emission/transmission/IOR values |
| `src/scene.h` | Optional authored emission, transmission and IOR fields |
| `src/vk_engine_scene_materials.cpp` | Propagate authored trace factors and UV/texture changes independently of raster material updates |
| `src/vk_engine_scene_io.cpp` | Round-trip optional physical-light/material fields; prevent unattended tests from overwriting the remembered scene |
| `src/vk_engine_editor.cpp` | Renderer selector and trace-only emission/transmission/IOR editing |
| `shaders/path_trace.comp` (new) | Camera/diagnostic dispatch, progressive component histories, persistent fault flags, display resolve/debug views |
| `shaders/path_trace_intersect.glsl` (new) | Bounds-checked GPU BVH, triangle/AABB intersections and normals |
| `shaders/path_trace_transport.glsl` (new) | Deterministic sampling, surface paths, lighting/visibility, roulette and component classification |
| `shaders/path_trace_material.glsl` (new) | Linear texture filtering, Lambert/GGX BRDF/PDF sampling, dielectric Fresnel/refraction |
| `shaders/path_trace_portal.glsl` (new) | Same-scene aperture traversal and prescribed portal-light visibility chains |
| `assets/scenes/gi_open_box.json` (new) | Environment-lit diffuse/color-bleeding fixture |
| `assets/scenes/gi_cornell_box.json` (new) | Sealed room with internal emitter |
| `assets/scenes/gi_cornell_box_dark.json` (new) | Sealed room with no light, exact-black control |
| `assets/scenes/gi_material_lab.json` (new) | Textured floor, metal and perfect dielectric fixture |
| `assets/scenes/gi_portal_pair.json` (new) | First single linked-pair validation |
| `assets/scenes/gi_portal_cycle.json` (new) | Deliberate cyclic portal path and traversal cap |
| `assets/scenes/gi_portal_multi_pair.json` (new) | Two authored portal pairs/four apertures |
| `assets/scenes/gi_portal_sun.json` (new) | Sealed room illuminated only by portal-admitted sun |
| `assets/scenes/gi_portal_emitter.json` (new) | External emitter illuminating the room only through a portal |
| `assets/textures/gi_color_test.ppm` (new) | Known 2x2 sRGB color-decoding/filtering fixture |
| `scripts/validate_software_trace.ps1` (new) | Reproducible 17-case desktop validation, timeout/error detection, capture/source manifests and environment restoration |
| `docs/global_illumination_reference_design.md` | Append concrete desktop choices and acceptance boundary to the preserved controlling design |
| `docs/gi_desktop_implementation_report.md` (new) | This checkpoint, validation and handoff record |

Untouched pre-existing modified files: `assets/scenes/.last_scene`, `assets/scenes/screen_space_buffer_lab.json`, `assets/scenes/shadow_showcase.json`, `shaders/input_structures.glsl`, `shaders/ssao.comp`. User edits within `src/vk_engine.h` were retained while trace fields were added. These appear in `git diff` but are not GI changes. CMake-generated binaries, object files and SPIR-V live in existing ignored build locations. Internal checkpoints, logs, GPU captures, source manifests and the initial preservation patch live under ignored `tmp/gi-checkpoints/`; they are not source commits.

### Final validation commands and interpretation

All commands below run from the repository root, except individual engine launches explicitly documented per milestone.

```powershell
cmake -S . -B build
cmake --build build --config Debug --parallel 6
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_software_trace.ps1
& 'C:\VulkanSDK\1.4.357.0\Bin\spirv-val.exe' --target-env vulkan1.3 shaders/path_trace.comp.spv
git diff --check
git status --short
git diff --cached --stat
git worktree list --porcelain
```

The script's ExecutionPolicy option applies only to that process; no machine policy is changed. It is necessary here because direct `.ps1` invocation is disabled. The script launches the existing Debug executable, sets synchronization validation, checks normal frame-budget completion and captured output, checks for Vulkan VUID/errors, verifies remembered-scene preservation, and compares repeated PFM files byte-for-byte. Source SHA256 manifests include tracked and new source/scene files; executable and shader hashes are recorded separately. Capture settings include source/worktree identity, camera, scene revision/hash, dimensions, samples, seed, depth, material model, portal limit, light inputs and timings. The scene source manifest supplies complete authored material values.

The 17 cases cover two existing raster scenes, explicit unavailable-tracer fallback, raster/trace switching, odd-size resize/render-scale/minimize/restore, physical-input invalidation, dark and lit Cornell boxes, deterministic open-box replay, material diagnostics, one portal pair, recursion cap, two pairs, sun/emitter portal illumination, and black controls at portal limit zero. Invalidation checks exposure preserving history and resets for seed, camera, depth, color, visibility, transform, extent/scale, manual reset, sun radiance, environment intensity, material model, portal budget, sun direction, emission and portal position. Internal CPU/GPU diagnostics run before each trace case. No test scene/layout/remembered-scene changes are saved.

Final build succeeds; SPIR-V validation exits 0; `git diff --check` reports no whitespace errors (Git emits existing LF/CRLF conversion advisories). Debug compilation retains MSVC `getenv` deprecation warnings. Release configuration, sanitizers, and cross-vendor runs were not executed. Per-milestone commands/results above document each build and checkpoint rather than implying only a final build was used.

Resolved validation/tooling failures are retained in the local logs: MSBuild FileTracker access denied in the sandbox (normal authorized build succeeded); temporary automatic approval usage limits (normal retry after user continuation succeeded); initial unmapped readback access (explicit VMA map/invalidate/unmap fixed); acquire semaphore reuse on SUBOPTIMAL (consumed before resize); raster-edge comparison lacking near-plane clipping (fixed and bounded by actual subpixel precision); test remembered-scene persistence (restored and guarded); PowerShell script invocation policy, malformed initial `git --output` argument and process ExitCode handle retention (runner fixed); emission reset test modifying a black emitter (fixture corrected); one rebuild attempted while the validation executable was still running caused LNK1168 (waited for its normal exit, then rebuilt successfully). No unresolved validation failure is being hidden or stashed.

### Resource, synchronization and fallback behavior

Raster remains the startup/default branch and retains its pass sequence, shaders, descriptors, SSAO/FXAA settings, shadowing and raster portal composition. Tracing uses its own compute pipeline/descriptors/SSBOs and three RGBA32F histories. Returning to Raster executes the existing renderer; returning to tracing resets accumulation. The unavailable capability/missing shader/pipeline paths keep Raster with a clear status, and the test-only disable override exercises that fallback without renaming or deleting a shader.

History and display transitions use the existing synchronization2 helper between compute, transfer and later frames. CPU uploads and descriptor replacement retire readers with `vkDeviceWaitIdle`; normal cleanup waits idle and destroys trace resources before shared device/allocator teardown. Per-frame timestamp slots are read after that slot's fence. SUBOPTIMAL acquisition is consumed before rebuild. Smaller/odd draw extents never dispatch outside the backing allocation; scale/extent changes invalidate history. Minimize pauses drawing and restore uses swapchain rebuild. Large windows remain capped to the original 1280x720 draw allocation, matching existing raster behavior.

### Known limitations and remaining uncertainty

- These are numerical and runtime checks plus inspected trace captures, not a pixel-identical before/after raster screenshot comparison. Human raster appearance remains a required check.
- No laptop or NVIDIA acceptance has been performed. Bit-identical output is demonstrated only for repeated runs on this desktop/build; another GPU can legitimately differ in floating-point rounding.
- One sample per dispatch, depth 2, portal limit 2, and progressive accumulation are the conservative RX 5700 defaults. Maximum depth/traversal is 4. Measured tested-scene dispatches are tens of milliseconds, well below typical Windows timeout durations, but this is not an arbitrary-scene safety guarantee. There is no tiled scheduler, automatic budget reduction, denoiser or reprojection. Very large scenes/complex portal chains need profiling first.
- Scene edits rebuild a world-space BVH and can stall on a device-idle wait. Retaining CPU meshes/texels adds memory even in Raster mode; three 1280x720 RGBA32F histories add about 42.2 MiB GPU memory. Texture snapshots may also retain unused uploaded images such as the skybox. Existing engine-wide allocation failures remain fatal; memory-exhaustion recovery was not redesigned.
- Only opaque draw-list geometry is baked. Alpha blending/masking, animated/deformed mesh updates, normal maps, metallic-roughness/emissive texture channels, alternate UV sets/transforms and sampler modes beyond repeating bilinear level zero are unsupported. Trace gradient environment is independent of the raster skybox. Singular/degenerate/invalid triangles are skipped.
- Materials use isotropic single-scattering GGX with roughness floor 0.045, constant metallic/roughness, and perfect dielectric transmission. There is no rough transmission, nested-medium stack, volume absorption, spectral dispersion or dedicated caustics estimator. Shading-normal corrections on arbitrary smooth assets need further comparison. MIS is absent; area lights use next-event sampling with corresponding non-delta hit suppression. Long/narrow specular paths can converge slowly.
- Direct/indirect classification is by visible-surface scattering depth; visible sky/emission and first-surface illumination are direct. The physical reference is bounded by the chosen scattering and portal limits, so it deliberately omits longer paths.
- Portal transport assumes rigid linked frames in one scene, uses finite aperture/offset tolerances, and uniformly samples direct-light chains. Many/long chains can have high variance. Unlinked apertures are magenta errors; hitting the cap ends the path in black. Wall/aperture grazing behavior needs interactive visual review.
- Capture support is currently exposed through the reproducible bounded-run environment/script, not an interactive file-picker/export button. PFM captures are linear RGB float; BMP previews use exposure/Reinhard/sRGB. Debug false colors and final tone mapping should not be used as quantitative HDR data.

### Required visual checks on the desktop

1. Launch `bin/Debug/engine.exe` with that directory as the working directory. Press **Tab** for the editor; open **Render Settings** and change **Renderer** between **Raster** and **Path Traced Reference (Software)**. Confirm both display correctly and switching back restores expected raster shadows, portals, SSAO/FXAA behavior and fine detail.
2. In `shadow_showcase.json` and `screen_space_buffer_lab.json`, compare the current raster appearance against your prior work. Rotate/move the camera, resize both smaller and larger, minimize/restore, and verify no stale or flashing pixels.
3. Use **File > Open Scene** for the `gi_*` fixtures. Compare geometric/shading normals, distance and albedo against opaque raster silhouettes. Check camera FOV/orientation at window edges, including thin geometry.
4. Inspect the generated open/Cornell/material BMP captures and their PFM components. Confirm red/green indirect bleeding, exact darkness in the unlit sealed box, emitter shadows, repeated color texture, reflective metal and refracting glass. Allow more samples for subjective convergence.
5. Check single and multiple portals, move/place the transient player pair, and inspect **Portal traversals**, **Portal limit**, and **Nonfinite / traversal errors** views. Confirm aperture alignment and bounded cycles, and understand that an unlinked portal is intentionally an error.
6. Hold a fixed camera to see samples accumulate; move/edit to see immediate reset. Change exposure and confirm the sample count continues. Any persistent magenta fault on a supported valid fixture should be treated as a failed reference capture.

### Exact laptop handoff steps (instructions only; no laptop work performed)

The owner must first make these uncommitted desktop changes available on a branch, or transfer the changed source files. There is currently nothing new to pull remotely. After the owner has published the intended branch and selected that branch on the laptop, preserve any laptop-local work before pulling; do not force reset it.

From the laptop repository root in a fresh PowerShell terminal, with the existing project prerequisites installed (Git, CMake, Visual Studio C++ workload, Vulkan SDK and a Vulkan-capable driver):

```powershell
git status --short
# Resolve/preserve any local edits yourself before continuing.
git pull --ff-only
cmake -S . -B build
cmake --build build --config Debug --parallel 6
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_software_trace.ps1
Push-Location .\bin\Debug
.\engine.exe
Pop-Location
```

Use **Tab > Render Settings > Renderer > Path Traced Reference (Software)**. Keep the software backend; there is no hardware-RT option to enable. Review the new `tmp/gi-checkpoints/validation-<timestamp>/` logs and captures. Checkpoint 1 camera rays, checkpoint 3 visibility/BVH, and checkpoint 5 accumulation must pass on the laptop using matching committed source, scene, Debug configuration, camera, seed and settings before claiming the design's dual-machine acceptance. The script records the needed evidence and leaves the remembered scene/environment unchanged. Do not copy desktop build products or `build/CMakeCache.txt` to the laptop.

### Recommended next implementation step

First complete the human desktop review and retain longer, fixed-camera linear-HDR convergence baselines for the open box, lit/dark Cornell box and material lab. The next code step should be a quantitative capture-comparison harness (error/variance against those baselines), with targeted tests for smooth shading normals and difficult specular paths. Use that evidence to decide whether MIS or tiled dispatch is needed before beginning SSGI integration. Hardware RT and cross-machine transport remain outside this delivery.

## Final observed result (2026-09-12)

The final runner completed **17/17 cases, exit 0**, with explicit frame-budget completion, capture-existence and minimize/restore assertions enabled. No Vulkan VUID/validation errors, persistent transport faults, or nonfinite capture pixels were reported. Both existing raster scenes and the forced unavailable-tracer fallback completed normally. All physical-input reset assertions passed. Direct plus indirect equals combined with **zero** measured residual in final captures; both disabled portal-light controls and the ordinary sealed dark box are exactly black.

- Command: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_software_trace.ps1`.
- Console log: `tmp/gi-checkpoints/final-validation.log`.
- Final artifacts: `tmp/gi-checkpoints/validation-20260912-061653/`. This directory includes the 17 case logs, capture settings, source manifest, working patch, binary hashes, and linear PFM/display BMP files.
- Final repeated open-box PFM SHA256 (both runs): `DC9B21356AB01CE62D37CE1043384CA3843401D3C638E19A2AA4E35536893BB4`.
- Final source-manifest SHA256: `DC9B3873CDD51858E8516309630EB35898ADF98F5640D11112FA34AA40D1BDAB`.
- Maximum observed dispatch in this final suite: **39.3415 ms**, portal-emitter scene, 1280x720, depth 2, portal limit 2, 64 samples; open box maximum **2.7296 ms**, lit Cornell **31.331 ms**, material lab depth 4 **18.7583 ms**. These are measured dispatch durations, not frame-rate guarantees.
- Final Debug build, SPIR-V validation, whitespace check, preservation-hash checks and unchanged-index check all pass. Final state is saved in `tmp/gi-checkpoints/final-status.txt` and tracked working diff in `final-working.patch`. All new files remain present and uncommitted in the worktree.
`cmake -S . -B build` and `cmake --build build --config Debug --parallel 6` were repeated after final validation to register every new shader include in the existing build tree. Both passed (`final-configure.log`, `final-reconfigure-build.log`); executable and shader SHA256 values are unchanged from the tested binaries. A local final source archive is saved as `tmp/gi-checkpoints/desktop-source-checkpoint.zip`.

### Gameplay fixture spawn fix

After interactive review exposed that the trace-only fixtures had no gameplay spawn or collision, all nine `gi_*.json` scenes were updated with a hidden `Gameplay Spawn` marker at `(0, 0, 2.5)` and a safe `nextActor` value. GI fixture loading now enables the authored floor and unit-cube room geometry as gameplay collision while leaving other scenes' collision settings unchanged. JSON parsing for all nine scenes passes, the Debug rebuild passes (`tmp/gi-checkpoints/spawn-fix-build.log`), and `gi_cornell_box.json` reruns all GPU diagnostics successfully (`tmp/gi-checkpoints/spawn-fix-cornell.log`). Use F1 to respawn inside a GI room after reloading it; F2 can then toggle no-clip.

### Tab resumes gameplay without respawning

Interactive review also exposed an existing unconditional respawn in `set_editor_mode(false)`. `src/vk_engine.cpp` now preserves the paused player's position and camera when returning from the editor, synchronizes the previous physics position, and clears stale velocity/jump input. `src/vk_engine_scene_io.cpp` applies the authored spawn on successful scene load, so opening another scene still starts gameplay in the right place. F1 remains the explicit respawn action.

The new `MIRABILIS_TEST_EDITOR_RESUME=1` bounded check exercises real Tab/F1 key handling, three editor/play cycles away from spawn, an independently moved editor camera, 240 stationary physics steps, and loading another scene. The runner includes this as `editor-resume`, bringing its case count to 18; the earlier 17-case results above remain historical results for the earlier build.

- `cmake --build build --config Debug --parallel 6`: passed (`tmp/gi-checkpoints/tab-resume-build.log`).
- From `bin/Debug`, `MIRABILIS_TEST_FRAMES=8`, `MIRABILIS_TEST_SCENE=gi_cornell_box.json`, `MIRABILIS_TEST_EDITOR_RESUME=1`, `VK_LAYER_VALIDATE_SYNC=1`, then `.\engine.exe`: passed all resume, camera, floor support, F1 and scene-spawn assertions; exit 0 (`tab-resume-test.log`).
- From `bin/Debug`, `MIRABILIS_TEST_FRAMES=20`, `MIRABILIS_TEST_TRACE=1`, `MIRABILIS_TEST_SCENE=gi_cornell_box.json`, `MIRABILIS_TEST_CAMERA='0 2 3.5 0 0'`, `MIRABILIS_TEST_CYCLE=1`, `VK_LAYER_VALIDATE_SYNC=1`, then `.\engine.exe`: all GPU diagnostics and raster/trace/resize/minimize cycle passed; exit 0 (`tab-resume-trace-cycle.log`). No Vulkan validation errors in either run. The remembered scene remains `gi_cornell_box.json`.
