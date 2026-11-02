"""Compare two Release engine checkouts with matched, sequential GPU workloads."""
import argparse
import csv
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import re
import statistics
import subprocess
import threading
import time

SCENES = {
    "sandbox.json": "0 2.5 12 -0.08 0",
    "sponza_showcase.json": "0 2.5 12 0 0",
    "gi_portal_pair.json": "0 2 6 0 0",
}
FATAL = re.compile(r"GI TEST FAIL|Validation Error|\[ERROR:|VUID-|Device lost|Unhandled renderer exception|Detected Vulkan error")
FLAGS = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def percentile(values, fraction):
    values = sorted(values)
    position = (len(values) - 1) * fraction
    lower = math.floor(position)
    return values[lower] + (values[math.ceil(position)] - values[lower]) * (position - lower)


def comparison(before, after):
    assert len(before) == len(after) and len(before) >= 3
    assert all(math.isfinite(x) and x > 0 for x in [*before, *after])
    old, new = statistics.median(before), statistics.median(after)
    rng = random.Random(216)
    bootstrap = []
    for _ in range(10000):
        indices = [rng.randrange(len(before)) for _ in before]
        bootstrap.append((statistics.median([before[i] for i in indices]) /
                          statistics.median([after[i] for i in indices]) - 1) * 100)
    gain = (old / new - 1) * 100
    low, high = percentile(bootstrap, .025), percentile(bootstrap, .975)
    # Five-percent reporting floor avoids turning small laptop fluctuations into a win.
    verdict = "no clear change"
    if low > 0 and gain >= 5: verdict = "improvement in this sample"
    if high < 0 and gain <= -5: verdict = "regression in this sample"
    return {"baselineMs": old, "candidateMs": new, "baselineFps": 1000 / old,
            "candidateFps": 1000 / new, "fpsChangePercent": gain,
            "pairedBootstrap95Percent": [low, high], "verdict": verdict,
            "baselineSpreadPercent": (max(before) - min(before)) / old * 100,
            "candidateSpreadPercent": (max(after) - min(after)) / new * 100,
            "baselineRunsMs": before, "candidateRunsMs": after}


def clean_environment():
    return {key: value for key, value in os.environ.items()
            if not (key.startswith(("MIRABILIS_", "VK_LAYER", "VK_KHRONOS_VALIDATION_")) or
                    key in ("VK_INSTANCE_LAYERS", "VK_LOADER_LAYERS_ENABLE"))}


def run(repo, output, scene, warmup, measured, capture=False):
    output.mkdir(parents=True, exist_ok=False)
    exe = repo / "bin/Release/engine.exe"
    flags = {"MIRABILIS_QT_EDITOR": "0", "MIRABILIS_TEST_SCENE": scene,
             "MIRABILIS_TEST_CAMERA": SCENES[scene], "MIRABILIS_TEST_WINDOW": "1600x900",
             "MIRABILIS_TEST_HIDE_PANELS": "1", "MIRABILIS_TEST_FRAMES": str(warmup + measured),
             "MIRABILIS_BENCHMARK_WARMUP_FRAMES": str(warmup), "MIRABILIS_SSGI_BENCHMARK": "1",
             "MIRABILIS_R4_UNCAP_FRAMES": "1", "MIRABILIS_R4_FRAME_TIME_LOG": str(output / "frames.csv")}
    if capture: flags["MIRABILIS_RASTER_CAPTURE"] = str(output / "raster")
    (output / "environment.json").write_text(json.dumps(flags, indent=2))
    env = clean_environment(); env.update(flags)
    started = time.perf_counter()
    utc = time.time()
    startup = None
    loads, active = [], None
    lines = []
    with (output / "stdout.log").open("w") as stdout, (output / "stderr.log").open("w") as stderr:
        process = subprocess.Popen([str(exe)], cwd=exe.parent, env=env, stdout=subprocess.PIPE,
                                   stderr=stderr, text=True, errors="replace", bufsize=1, creationflags=FLAGS)
        timer = threading.Timer(400, process.kill); timer.start()
        try:
            for line in process.stdout:
                now = time.perf_counter(); stdout.write(line); lines.append(line)
                if line.startswith("Loading GLTF scene:"):
                    active = (line.split(":", 1)[1].strip(), now)
                if line.startswith("GLTF meshes:") and active:
                    loads.append({"asset": Path(active[0]).name, "milliseconds": (now - active[1]) * 1000})
                    active = None
                if line.startswith("GI benchmark warmup:"): startup = now - started
            code = process.wait()
        finally:
            timer.cancel(); process.stdout.close()
    elapsed = time.perf_counter() - started
    logs = "".join(lines) + (output / "stderr.log").read_text(errors="replace")
    match = re.search(r"average-frame-ms=([\d.]+).*samples=(\d+)", logs)
    focused = re.search(r"GI benchmark focused samples: (\d+)/(\d+)", logs)
    result = {"exit": code, "startedUnix": utc, "elapsedSeconds": elapsed,
              "startupToRunSeconds": startup, "gltfLoads": loads,
              "fatalLines": [line for line in logs.splitlines() if FATAL.search(line)]}
    valid = (code == 0 and not result["fatalLines"] and match and focused and startup is not None and
             int(match[2]) == measured and int(focused[1]) == measured and int(focused[2]) == measured and
             f"GI bounded run complete: frames={warmup + measured} renderer=raster" in logs and
             "Loaded editor scene:" in logs and scene in logs)
    result["valid"] = bool(valid)
    if match: result["meanFrameMs"] = float(match[1]); result["fps"] = 1000 / float(match[1])
    if focused: result["focusedSamples"] = [int(focused[1]), int(focused[2])]
    if (output / "frames.csv").exists():
        with (output / "frames.csv").open() as stream:
            rows = [row for row in csv.DictReader(stream) if int(row["frame"]) > warmup]
        times = [float(row["ms"]) for row in rows]
        if len(rows) != measured or not all(math.isfinite(value) and value >= 0 for value in times):
            valid = False; result["valid"] = False
            result["failureReason"] = "Frame log has missing, extra or invalid samples"
        elif times:
            result["renderLoopP95Ms"] = percentile(times, .95)
            result["renderLoopP99Ms"] = percentile(times, .99)
            result["ssgiGpuMedianMs"] = statistics.median(float(row["ssgi_gpu_ms"]) for row in rows)
    (output / "result.json").write_text(json.dumps(result, indent=2))
    print(output.name, json.dumps({key: result.get(key) for key in
          ("valid", "meanFrameMs", "fps", "startupToRunSeconds", "focusedSamples")}), flush=True)
    assert valid, f"Invalid benchmark; see {output}"
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=120)
    parser.add_argument("--frames", type=int, default=600)
    args = parser.parse_args()
    assert args.runs >= 3 and args.warmup >= 120 and args.frames >= 600
    roots = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve()}
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    authored = {}; manifest = {"protocol": vars(args).copy(), "checkouts": {}}
    manifest["protocol"] = {key: str(value) if isinstance(value, Path) else value for key, value in manifest["protocol"].items()}
    manifest["notes"] = ["Release, profiling/validation disabled, identical SDL host and viewport; Qt overhead excluded",
                         "Wall start-to-start mean frames include present/waits; FPS = 1000 / median run mean ms",
                         "CSV percentiles measure render-loop work duration, not independently measured display presents",
                         f"Scene settings preserved; cap off; {args.warmup} warmup + {args.frames} focused samples; paired orders alternate",
                         "Startup and glTF log intervals use host receipt timestamps, include uploads, and are warm-cache measurements",
                         "Bootstrap intervals are descriptive on this laptop, not a population-level hardware claim"]
    for name, repo in roots.items():
        assert (repo / "bin/Release/engine.exe").exists()
        manifest["checkouts"][name] = {
            "path": str(repo), "revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
            "status": subprocess.check_output(["git", "status", "--short"], cwd=repo, text=True),
            "executableSha256": digest(repo / "bin/Release/engine.exe"),
            "shaders": {p.name: digest(p) for p in sorted((repo / "shaders").glob("*.spv"))},
            "sceneHashes": {p.name: digest(p) for p in sorted((repo / "assets/scenes").glob("*.json"))}}
        authored[name] = {str(p): digest(p) for p in [*sorted((repo / "assets/scenes").glob("*.json")), repo / "assets/scenes/.last_scene"]}
    for field in ("shaders", "sceneHashes"):
        assert manifest["checkouts"]["baseline"][field] == manifest["checkouts"]["candidate"][field], f"Mismatched {field}"
    for name, repo in roots.items():
        manifest["checkouts"][name]["assets"] = {
            str(p.relative_to(repo / "assets")): digest(p)
            for p in sorted((repo / "assets").rglob("*")) if p.is_file()}
    assert manifest["checkouts"]["baseline"]["assets"] == manifest["checkouts"]["candidate"]["assets"], "Mismatched asset bytes"
    class PowerStatus(ctypes.Structure):
        _fields_ = [("ac", ctypes.c_ubyte), ("flags", ctypes.c_ubyte),
                    ("percent", ctypes.c_ubyte), ("reserved", ctypes.c_ubyte),
                    ("remaining", ctypes.c_ulong), ("full", ctypes.c_ulong)]
    power = PowerStatus()
    if ctypes.windll.kernel32.GetSystemPowerStatus(ctypes.byref(power)):
        manifest["power"] = {"acLineStatus": power.ac, "batteryPercent": power.percent}
    manifest["platform"] = platform.platform(); manifest["processor"] = platform.processor()
    manifest["logicalCpus"] = os.cpu_count()
    manifest["powerScheme"] = subprocess.check_output(["powercfg", "/getactivescheme"], text=True).strip()
    manifest["gpu"] = subprocess.check_output(["nvidia-smi", "--query-gpu=name,driver_version", "--format=csv"], text=True).strip()
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2))
    samples = {scene: {name: [] for name in roots} for scene in SCENES}
    telemetry_file = (output / "gpu.csv").open("w")
    telemetry = subprocess.Popen(["nvidia-smi", "--query-gpu=timestamp,pstate,temperature.gpu,utilization.gpu,power.draw,clocks.current.graphics,clocks.current.memory", "--format=csv", "--loop-ms=1000"], stdout=telemetry_file, stderr=subprocess.DEVNULL, creationflags=FLAGS)
    try:
        # Discard a longer GPU warmup, then populate each process/filesystem/driver path once.
        run(roots["baseline"], output / "precondition", "sponza_showcase.json", 120, 3480)
        for scene in SCENES:
            images = []
            for name, repo in roots.items():
                directory = output / ("prime-" + Path(scene).stem + "-" + name)
                run(repo, directory, scene, args.warmup, args.frames, capture=True)
                images.append(digest(directory / "raster.pfm"))
            assert images[0] == images[1], f"Rendered images differ in {scene}"
        for repeat in range(args.runs):
            scenes = list(SCENES)
            scenes = scenes[repeat % len(scenes):] + scenes[:repeat % len(scenes)]
            for scene in scenes:
                order = list(roots) if repeat % 2 == 0 else list(reversed(roots))
                for name in order:
                    result = run(roots[name], output / f"r{repeat + 1}-{Path(scene).stem}-{name}", scene, args.warmup, args.frames)
                    samples[scene][name].append(result)
                (output / "samples.json").write_text(json.dumps(samples, indent=2))
    finally:
        telemetry.terminate(); telemetry.wait(timeout=10); telemetry_file.close()
        for name, files in authored.items():
            assert files == {path: digest(Path(path)) for path in files}, f"Authored files changed: {name}"
    summary = {}
    for scene, variants in samples.items():
        summary[scene] = comparison([r["meanFrameMs"] for r in variants["baseline"]], [r["meanFrameMs"] for r in variants["candidate"]])
        for name, runs in variants.items():
            summary[scene][name + "StartupSeconds"] = statistics.median(r["startupToRunSeconds"] for r in runs)
            summary[scene][name + "RenderLoopP99Ms"] = statistics.median(r["renderLoopP99Ms"] for r in runs)
            summary[scene][name + "GltfLoads"] = [r["gltfLoads"] for r in runs]
    (output / "summary.json").write_text(json.dumps(summary, indent=2))
    print("BENCHMARK COMPLETE", json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()
