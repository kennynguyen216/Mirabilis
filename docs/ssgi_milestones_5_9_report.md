# SSGI Milestones 5-9 Implementation Report

Date: 2026-09-12  
Status: Milestones 5-9 implemented and runtime-validated; cross-machine and
remaining human acceptance are listed explicitly below

This report continues [the Milestones 1-4 report](ssgi_milestones_1_4_report.md).
Debug views 15-18 show temporal indirect, history rejection, reprojection,
and the final filtered buffer. `MIRABILIS_RENDER_DEBUG_VIEW` can select any
view directly for a bounded or interactive launch.

## Milestone 5: temporal accumulation

The one-to-four-ray estimate now feeds a ping-pong `R16G16B16A16_SFLOAT` temporal
history. Current-to-previous motion vectors reproject the history and an EMA
uses a default history weight of 0.92.

Each history side has packed geometric metadata: reversed depth, an
octahedrally encoded view-space normal, and the portal mask. History is rejected
for invalid/off-screen reprojection, depth mismatch, normal mismatch, portal
mask transitions, or excessive velocity. A loose radiance clamp catches large
stale outliers after the geometric tests. Resolution changes, preset changes,
and disabling SSGI invalidate history. Temporal accumulation is a separate
compute dispatch from ray generation, so its true 3x3 current-frame
neighborhood min/max clamp never races unfinished neighboring workgroups.

The rejection debug view uses green for accepted history, blue for unavailable
or off-screen history, red for depth rejection, yellow for normal rejection,
and magenta for portal/velocity rejection. The reprojection view shows velocity
magnitude in red and accepted/rejected state in green/blue.

Automated Vulkan validation is clean. User testing confirmed that stationary
pixels accept history (green) and fast rotation rejects it (magenta) without a
reported persistent trail. Newly exposed pixels remain temporarily noisy by
design; the quality presets now trade one, two, or four rays per pixel against
that motion noise.

## Milestone 6: spatial filtering

A separable, full- or half-resolution bilateral filter runs horizontal then
vertical over temporal indirect. Spatial Gaussian, reversed-depth similarity,
and view-normal similarity weights prevent ordinary blur from crossing depth
and orientation edges. Radius, depth falloff, and normal exponent are exposed
in the editor. The final filtered buffer has its own debug view.

Automated Vulkan validation is clean. Human acceptance still needs inspection
of foreground silhouettes in the filtered view.

## Milestone 7: compositing

The filtered indirect image is additively composited into the main HDR raster
target before portals, FXAA, and presentation. Indirect intensity defaults to
the validation value 1.0.

The flat raster ambient term is now suppressed for the main camera while SSGI
is active, so indirect light is not counted twice. Because SSAO previously
modulated only that ambient term, its contribution is also bypassed while SSGI
is active; the directional sun and shadow visibility are unchanged. Portal
cameras retain the old ambient path because they do not own a matching SSGI
G-buffer.

The composite runs cleanly under synchronization validation. Raster SSGI can
now be captured directly as a linear-HDR PFM with
`MIRABILIS_SSGI_CAPTURE=<prefix>`. Supplying
`MIRABILIS_SSGI_REFERENCE=<reference.indirect.pfm>` also writes an absolute
difference PFM and records MAE, RMSE, relative MAE, and maximum channel error.
Debug view 20 displays a false-colour difference against that loaded PFM.

## Milestone 8: portal handling

The implementation uses the design's explicit Option A. Visible portal
apertures are stamped into an `R8_UNORM` mask before the ray march. A ray that
enters the mask terminates to the environment fallback; the exact accumulated
fallback contribution has its own image/debug view, and temporal history is
also rejected across portal-mask transitions. Portal interiors are composed
after SSGI, so their foreign-camera depth and normals never enter main-camera
screen-space reconstruction.

This intentionally produces less indirect light near and through portals than
the reference path tracer, which teleports rays. The missing cross-portal
bounce and possible boundary discontinuity are documented limitations, not
SSGI defects. The authored multi-pair portal scene runs without validation
messages.

## Milestone 9: performance and presets

Five presets are available:

| Preset | Resolution | Rays/pixel | Ray steps | Filter radius | History weight |
|---|---:|---:|---:|---:|---:|
| Validation | Full | 4 | 32 | 3 | 0.92 |
| High | Half | 4 | 48 | 4 | 0.94 |
| Balanced | Half | 2 | 32 | 3 | 0.92 |
| Performance | Half | 1 | 16 | 2 | 0.90 |
| Peak | Full | 8 | 96 | 5 | 0.96 |

The Render Settings panel also provides `Apply Maximum Fidelity`. It selects
render scale 1.0, the Peak SSGI preset, 4096x4096 directional shadows,
six-texel deterministic 49-sample tent shadow filtering, conservative receiver
biases, detail-preserving FXAA, and indirect intensity 0.35. `Shadow Softness`
can adjust that filtering from zero to twelve shadow-map texels without
recompiling shaders. SSAO retains its
64-sample quality setting but is disabled while SSGI is active because its
ambient term is intentionally bypassed; running it would add cost without
changing the composited image. `MIRABILIS_MAX_FIDELITY=1` applies the same
configuration for automated launches.

Half resolution dispatches one quarter as many GI pixels. The full-resolution
composite performs a 3x3 depth- and normal-aware bilateral upsample rather than
naive bilinear scaling. Debug views expand half-resolution texels to the full
screen so the active result remains inspectable.

GPU timestamps separately measure ray generation, temporal accumulation,
filtering, and composite.
On the AMD Radeon RX 5700, driver 8388961, Vulkan 1.4.315, at 1280x720 in the
`screen_space_buffer_lab.json` scene, 95 measured frames after warm-up gave:

| Preset | SSGI extent | Mean SSGI GPU time |
|---|---:|---:|
| Validation | 1280x720 | 8.452 ms |
| High | 640x360 | 1.806 ms |
| Balanced | 640x360 | 0.934 ms |
| Performance | 640x360 | 0.358 ms |
| Peak | 1280x720 | 35.984 ms |

The measured average frame times were 9.297, 6.007, 5.988, and 6.010 ms in
preset order. These numbers are machine-, driver-, scene-, and
build-specific, as required by the hardware addendum. Reproduce with
`MIRABILIS_SSGI_BENCHMARK=1`, `MIRABILIS_SSGI_PRESET=0..4`, and a bounded
`MIRABILIS_TEST_FRAMES` run from `bin/Debug`.

## Automated validation

- Debug build completed successfully.
- `spirv-val` accepted the ray/temporal, bilateral-filter, composite,
  render-debug, and forward fragment shaders.
- Final, temporal, rejection, reprojection, filtered, exact-fallback, and
  loaded-reference difference views completed
  bounded runs under Vulkan synchronization validation without messages.
- Balanced half-resolution filtered output and the multi-pair portal scene
  each completed a 60-frame synchronization-validation run.
- The full software-reference/raster regression suite passed all **18/18**
  cases, including resize/mode changes, deterministic captures, materials, and
  portal transport. Artifacts are in
  `tmp/gi-checkpoints/validation-20260912-160143/`.
- A 120-frame full-resolution SSGI capture contained no non-finite pixels.
  Against a 256-sample fixed-camera reference it measured MAE 0.029855, RMSE
  0.095802, and relative MAE 9.327. This is evidence that the comparison path
  works, not an equivalence claim: environment fallback dominates many misses
  and is intentionally an approximation for unseen geometry.
- The sealed, emitter-free, black-environment Cornell capture measured mean
  indirect radiance `1.61e-7` with zero non-finite pixels, effectively black.
- `git diff --check` reported no whitespace errors; Git emitted only the
  repository's existing LF/CRLF conversion advisories.

## Remaining acceptance work

All implementation milestones and required debug stages are present. Remaining
acceptance is deliberately not represented as completed:

- visually inspect the filtered buffer at difficult silhouettes for cross-edge
  bleeding;
- visually characterize the expected Option-A indirect-light discontinuity at
  portal boundaries;
- repeat labelled performance measurement on the second development machine;
- decide an error budget for the structurally approximate fallback before the
  numerical comparison can become a pass/fail gate.

Hardware ray tracing for off-screen light is not part of this design. The
implemented miss path is the specified analytic environment fallback. Adding a
BVH/RT fallback would be a separate hybrid-GI project and must not be inferred
from completion of these SSGI milestones.
