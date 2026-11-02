"""Exercise real engine traces and compare profiling on/off raster captures."""
import argparse
import hashlib
import json
import math
from pathlib import Path

import ecs_gate


def check_trace(path, minimum_frames):
    data = json.loads(path.read_text(encoding="utf-8"))
    assert data["otherData"]["droppedEvents"] == 0, "Trace filled during the fixture"
    events = data["traceEvents"]
    required = {"Frame", "Events", "Simulation", "Render", "Geometry", "GPU fence wait"}
    assert required <= {event["name"] for event in events}, "Missing engine operations"
    assert sum(event["name"] == "Frame" for event in events) >= minimum_frames
    for event in events:
        assert event["ph"] in ("X", "C")
        assert math.isfinite(event["ts"]) and event["ts"] >= 0
        if event["ph"] == "X":
            assert math.isfinite(event["dur"]) and event["dur"] >= 0
        else:
            assert math.isfinite(event["args"]["value"])
    values = lambda name: [event["args"]["value"] for event in events
                           if event["ph"] == "C" and event["name"] == name]
    growths = values("Draw scratch growths")
    assert len(growths) >= minimum_frames and len(set(growths[-8:])) == 1, "Scratch keeps growing after warmup"
    assert values("Draw calls")[-1] > 0 and values("Triangles")[-1] > 0, "Fixture drew no geometry"
    return {"events": len(events), "scratchGrowths": growths[-1],
            "scratchBytes": values("Draw scratch bytes")[-1]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--configuration", choices=("Debug", "Release"), default="Debug")
    args = parser.parse_args()
    repo, output = args.repo.resolve(), args.output.resolve()
    assert not output.exists() or not any(output.iterdir()), "Use a fresh output directory"
    output.mkdir(parents=True, exist_ok=True)
    ecs_gate.REPO, ecs_gate.OUT = repo, output
    authored = [*sorted((repo / "assets/scenes").glob("*.json")), repo / "assets/scenes/.last_scene"]
    before = {str(path.relative_to(repo)): ecs_gate.digest(path) for path in authored}
    summary = {}
    for name, scene, frames, extra in (
            ("sandbox", "sandbox.json", 24, {}),
            ("portal-traversal", "gi_portal_pair.json", 200, {"MIRABILIS_TEST_PORTAL_TRAVERSAL": "1"})):
        captures = []
        for trace in (False, True):
            label = name + ("-trace" if trace else "-off")
            directory = output / label
            directory.mkdir()
            prefix = directory / "raster"
            flags = {"MIRABILIS_TEST_SCENE": scene, "MIRABILIS_TEST_FRAMES": str(frames),
                     "MIRABILIS_TEST_WINDOW": "640x360", "MIRABILIS_TEST_HIDE_PANELS": "1",
                     "MIRABILIS_R4_UNCAP_FRAMES": "1", "MIRABILIS_SSGI_DISABLE": "1",
                     "MIRABILIS_AUTO_EXPOSURE": "0", "MIRABILIS_RASTER_CAPTURE": str(prefix),
                     "VK_LAYER_VALIDATE_SYNC": "1", "VK_KHRONOS_VALIDATION_VALIDATE_SYNC": "true", **extra}
            if trace:
                flags["MIRABILIS_PROFILE_TRACE"] = str(directory / "trace.json")
            logs, result = ecs_gate.run(label, args.configuration, flags)
            assert f"GI bounded run complete: frames={frames} renderer=raster" in logs
            if extra: assert "crossed=true" in logs, "Portal traversal was not exercised"
            capture = prefix.with_suffix(".pfm")
            assert capture.is_file(), "Missing raster capture"
            captures.append(hashlib.sha256(capture.read_bytes()).hexdigest())
            if trace: result.update(check_trace(Path(flags["MIRABILIS_PROFILE_TRACE"]), frames))
            summary[label] = result
        assert captures[0] == captures[1], f"Profiling changed the {name} raster output"
    after = {str(path.relative_to(repo)): ecs_gate.digest(path) for path in authored}
    assert before == after, "Authored scene data or selection changed"
    (output / "summary.json").write_text(json.dumps(summary, indent=2))
    (output / "authored-files.json").write_text(json.dumps(after, indent=2))
    print("PROFILING GPU GATE PASS", flush=True)


if __name__ == "__main__":
    main()
