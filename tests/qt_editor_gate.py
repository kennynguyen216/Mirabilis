"""Exercise the Qt content browser against the live Vulkan engine using disposable files."""
import argparse
import json
from pathlib import Path
import shutil
import struct
import zlib
import ecs_gate


def fixtures(root, repo):
    (root / "scenes").mkdir(parents=True)
    shutil.copy2(repo / "assets/scenes/sandbox.json", root / "scenes/sandbox.json")
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes([0, 180, 100, 240, 255]))) + chunk(b"IEND", b""))
    (root / "color.png").write_bytes(png)
    (root / "broken.png").write_text("not an image")
    vertices = 4096  # exercise the worker tangent path and its PMR scratch
    position_bytes, uv_bytes = vertices * 12, vertices * 8
    geometry = (struct.pack("<9f", -1, 0, 0, 1, 0, 0, 0, 1, 0) + bytes((vertices - 3) * 12)
                + struct.pack("<6f", 0, 0, 1, 0, .5, 1) + bytes((vertices - 3) * 8) + struct.pack("<3H", 0, 1, 2))
    image_offset = (len(geometry) + 3) & ~3
    data = geometry.ljust(image_offset, b"\0") + png
    (root / "triangle.bin").write_bytes(data)
    model = {"asset": {"version": "2.0"}, "buffers": [{"uri": "triangle.bin", "byteLength": len(data)}],
             "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": position_bytes},
                             {"buffer": 0, "byteOffset": position_bytes, "byteLength": uv_bytes},
                             {"buffer": 0, "byteOffset": position_bytes + uv_bytes, "byteLength": 6},
                             {"buffer": 0, "byteOffset": image_offset, "byteLength": len(png)}],
             "accessors": [{"bufferView": 0, "componentType": 5126, "count": vertices, "type": "VEC3", "min": [-1, 0, 0], "max": [1, 1, 0]},
                           {"bufferView": 1, "componentType": 5126, "count": vertices, "type": "VEC2"},
                           {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}],
             "images": [{"uri": "color.png"}, {"bufferView": 3, "mimeType": "image/png"}, {"uri": "unused-missing.png"}],
             "textures": [{"source": 0}, {"source": 1}],
             "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicRoughnessTexture": {"index": 0}}, "normalTexture": {"index": 1}}],
             "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2, "material": 0}]}],
             "nodes": [{"mesh": 0}], "scenes": [{"nodes": [0]}], "scene": 0}
    (root / "triangle.gltf").write_text(json.dumps(model))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--configuration", choices=("Debug", "Release"), default="Debug")
    args = parser.parse_args()
    repo, output = args.repo.resolve(), args.output.resolve()
    assert not output.exists() or not any(output.iterdir()), "Use a fresh output directory"
    output.mkdir(parents=True, exist_ok=True)
    authored = [*sorted((repo / "assets/scenes").glob("*.json")), repo / "assets/scenes/.last_scene"]
    before = {str(path): ecs_gate.digest(path) for path in authored}
    root = output / "assets"
    fixtures(root, repo)
    ecs_gate.REPO, ecs_gate.OUT = repo, output
    logs, result = ecs_gate.run("qt-browser", args.configuration, {
        "MIRABILIS_QT_EDITOR": "1", "MIRABILIS_TEST_QT_BROWSER": "1", "MIRABILIS_TEST_FRAMES": "10000",
        "MIRABILIS_TEST_SCENE": "sandbox.json", "MIRABILIS_TEST_SCENE_DIRECTORY": str(root / "scenes"),
        "MIRABILIS_ASSET_ROOT": str(root), "MIRABILIS_TEST_QT_RASTER": str(output / "viewport"), "MIRABILIS_TEST_QT_CAPTURE": str(output / "editor.png"),
        "MIRABILIS_SSGI_DISABLE": "1", "MIRABILIS_R4_UNCAP_FRAMES": "1",
        "MIRABILIS_PROFILE_TRACE": str(output / "trace.json"),
        "VK_LAYER_VALIDATE_SYNC": "1", "VK_KHRONOS_VALIDATION_VALIDATE_SYNC": "true"}, seconds=150)
    assert "QT BROWSER PASS:" in logs
    assert (output / "viewport.pfm").is_file()
    assert "GLTF image decode failed" not in logs, "Referenced image did not decode"
    assert logs.count("GLTF decode batches: 2 sources, 3 color-space uploads (triangle.gltf)") == 3
    scene = json.loads((root / "scenes/sandbox.json").read_text())
    assert scene["rendering"]["lumenGI"] and scene["rendering"]["sceneReflections"], "Qt GI/reflection edits did not save"
    assert scene["rendering"]["celShading"] and scene["rendering"]["bloom"], "Qt rendering edits did not save"
    assert not any("light" in obj["components"] for obj in scene["objects"]), "Temporary test light was not deleted"
    assert len(scene["objects"]) == len(json.loads((repo / "assets/scenes/sandbox.json").read_text())["objects"]) + 1, "Discard autosaved the test cube"
    imported = [obj["components"] for obj in scene["objects"] if obj["components"]["name"]["value"].startswith("triangle")]
    assert len(imported) == 1 and abs(imported[0]["transform"]["position"][0] - 2.75) < 1e-6
    assert Path(imported[0]["material"]["baseColorTexture"]).name == "color.png"
    trace = json.loads((output / "trace.json").read_text())["traceEvents"]
    assert {"Task execution", "Image decode"} <= {event["name"] for event in trace}
    decode_threads = {event["tid"] for event in trace if event["name"] == "Image decode"}
    frame_threads = {event["tid"] for event in trace if event["name"] == "Frame"}
    assert decode_threads and decode_threads.isdisjoint(frame_threads), "Image decode ran on the frame thread"
    assert before == {str(path): ecs_gate.digest(path) for path in authored}, "Authored scene files changed"
    (output / "summary.json").write_text(json.dumps(result, indent=2))
    print("QT EDITOR GPU GATE PASS", flush=True)


if __name__ == "__main__":
    main()
