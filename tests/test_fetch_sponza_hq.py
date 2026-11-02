import io
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from scripts.fetch_sponza_hq import RemoteZip, safe_relative, selected_files, write_preview_profile


class SponzaDownloadTests(unittest.TestCase):
    def test_package_paths_reject_traversal_and_windows_drives(self):
        self.assertEqual(str(safe_relative("./textures/stone%20wall.png")), "textures/stone wall.png")
        for name in ("../outside", "/outside", "C:/outside", "C:\\outside", "..\\outside", "%2e%2e/outside", "//server/share"):
            with self.subTest(name=name), self.assertRaises(ValueError): safe_relative(name)

    def test_download_selection_only_includes_gltf_dependencies_and_license(self):
        data = io.BytesIO()
        model = {"buffers": [{"uri": "mesh.bin"}], "images": [{"uri": "./textures/color.png"}]}
        with zipfile.ZipFile(data, "w") as archive:
            for name, content in {"package/scene.gltf": json.dumps(model), "package/mesh.bin": "mesh",
                                  "package/textures/color.png": "pixels", "package/credits_license.txt": "license",
                                  "package/unused.max": "skip", "package/unused.fbx": "skip"}.items():
                archive.writestr(name, content)
        with zipfile.ZipFile(data) as archive:
            self.assertEqual({entry.filename for entry in selected_files(archive)}, {
                "package/scene.gltf", "package/mesh.bin", "package/textures/color.png", "package/credits_license.txt"})

    def test_remote_zip_range_cache_and_end_of_file(self):
        data = bytes(range(256)) * 400
        calls = []
        def response(args, **kwargs):
            start, end = map(int, args[args.index("--range") + 1].split("-"))
            calls.append((start, end))
            return SimpleNamespace(returncode=0, stderr=b"", stdout=data[start:end + 1])
        with patch("scripts.fetch_sponza_hq.subprocess.run", side_effect=response):
            stream = RemoteZip("https://example.invalid/archive.zip", len(data))
            self.assertEqual(stream.read(24), data[:24])
            stream.seek(1024); self.assertEqual(stream.read(16), data[1024:1040])
            self.assertEqual(len(calls), 1)
            stream.seek(-7, 2); self.assertEqual(stream.read(), data[-7:])
            self.assertEqual(stream.read(1), b"")

    def test_incorrect_range_response_is_rejected(self):
        with patch("scripts.fetch_sponza_hq.subprocess.run", return_value=SimpleNamespace(returncode=0, stderr=b"", stdout=b"wrong")):
            with self.assertRaises(OSError): RemoteZip("https://example.invalid/archive.zip", 100).read(10)

    def test_hq_scene_keeps_required_roots_and_both_models(self):
        root = Path(__file__).resolve().parent.parent
        scene = json.loads((root / "assets/scenes/sponza_hq_full_4k.json").read_text())
        self.assertTrue(any(obj["name"] == "Sandbox" and obj["parent"] == -1 for obj in scene["objects"]))
        self.assertTrue(any(obj["asset"] == "floor" for obj in scene["objects"]))
        imports = [obj for obj in scene["objects"] if obj["asset"] == "gltf"]
        self.assertEqual(len(imports), 2)
        self.assertTrue(any("main_sponza/" in obj["modelPath"] for obj in imports))
        self.assertTrue(any("pkg_a_curtains/" in obj["modelPath"] for obj in imports))

    def test_preview_keeps_4k_sources_and_uses_existing_data_maps(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            original = root / "main_sponza"
            optimized = root / "old/textures"
            (original / "textures").mkdir(parents=True)
            optimized.mkdir(parents=True)
            for name in ("color.png", "normal.png", "rough.png"):
                (original / "textures" / name).write_bytes(b"original 4K bytes")
                (optimized / name).write_bytes(b"existing 2K bytes")
            model = {"buffers": [{"uri": "mesh.bin"}],
                     "images": [{"uri": "textures/" + name} for name in ("color.png", "normal.png", "rough.png")],
                     "textures": [{"source": i} for i in range(3)],
                     "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}]}
            source = original / "NewSponza_Main_glTF_003.gltf"
            source.write_text(json.dumps(model))
            before = source.read_bytes()
            write_preview_profile(root, optimized.parent)
            directory = root / "runtime_6gb"
            preview = json.loads((directory / "Sponza_4K_Color.gltf").read_text())
            self.assertEqual((directory / preview["images"][0]["uri"]).read_bytes(), b"original 4K bytes")
            self.assertEqual((directory / preview["images"][1]["uri"]).read_bytes(), b"existing 2K bytes")
            self.assertEqual((directory / preview["images"][2]["uri"]).read_bytes(), b"existing 2K bytes")
            self.assertEqual(source.read_bytes(), before)
