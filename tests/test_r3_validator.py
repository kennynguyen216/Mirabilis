"""End-to-end checks for scripts/validate_r3_capture.py.

The validator is what decides whether a capture directory may be called an R3
reference, so a validator that passes everything is worse than no validator: it
launders bad captures into accepted evidence.  These tests build a synthetic
capture directory in the engine's own file formats, run the tool as a
subprocess, and assert it passes clean data and fails each defect it exists to
catch.

Assertions here require a line that actually reported FAIL.  Every check prints
its label whether it passes or fails, so asserting that a label appears proves
nothing: an earlier version of this file passed against a validator whose
checks had been defeated one by one.

No GPU and no engine: the point is the tool's judgement, not the renderer's
output.

Run: python -m unittest discover -s tests
"""
import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parent.parent
VALIDATOR = ROOT / "scripts" / "validate_r3_capture.py"
CAPTURE_SCRIPT = ROOT / "scripts" / "capture_r3_references.ps1"

WIDTH, HEIGHT = 12, 8
SAMPLES = 512
EXPOSURE = 0.0
COMMIT = "142c8a7e7841dcec082dab0dae64a93746a72b91"
SEEDS = (1337, 2026, 90210)

SCENES = {
    "cornell": {"file": "gi_cornell_box.json", "camera": "0 2 3.5 0 0"},
    "living-room": {"file": "living_room_showcase.json", "camera": "0 1.6 -3 0 3.14"},
}

SIDE_FILES = ["source.txt", "binary-sha256.txt", "shader-sha256.txt",
              "scene-sha256.txt", "r3_regions.json"]


def engine_tonemap(linear, exposure_ev):
    value = np.maximum(linear, 0.0) * (2.0 ** exposure_ev)
    value = value / (1.0 + value)
    srgb = np.where(
        value <= 0.0031308,
        12.92 * value,
        1.055 * np.power(np.clip(value, 0, None), 1.0 / 2.4) - 0.055)
    return np.clip(srgb * 255.0 + 0.5, 0, 255).astype(np.uint8)


def write_pfm(path, image):
    """Bottom-row-first, little-endian: exactly what capture_path_trace does."""
    height, width, _ = image.shape
    with open(path, "wb") as file:
        file.write(b"PF\n")
        file.write(f"{width} {height}\n".encode())
        file.write(b"-1.0\n")
        for y in range(height - 1, -1, -1):
            file.write(image[y].astype("<f4").tobytes())


def write_bmp(path, srgb8):
    """24-bit bottom-up BGR, as the engine writes its preview."""
    height, width, _ = srgb8.shape
    stride = (width * 3 + 3) & ~3
    body = bytearray()
    for y in range(height - 1, -1, -1):
        row = bytearray()
        for x in range(width):
            row += bytes(srgb8[y, x][::-1])
        row += b"\x00" * (stride - width * 3)
        body += row
    with open(path, "wb") as file:
        file.write(b"BM")
        file.write(struct.pack("<I", 54 + len(body)))
        file.write(struct.pack("<II", 0, 54))
        file.write(struct.pack("<IiiHH", 40, width, height, 1, 24))
        file.write(struct.pack("<IIiiII", 0, len(body), 2835, 2835, 0, 0))
        file.write(bytes(body))


def asymmetric_scene(seed):
    """A gradient plus a bright corner: not symmetric on either axis."""
    generator = np.random.default_rng(seed)
    image = np.zeros((HEIGHT, WIDTH, 3), dtype=np.float32)
    image += np.linspace(0.05, 0.9, HEIGHT, dtype=np.float32)[:, None, None]
    image[:2, :3, 0] += 3.0  # bright red patch, top-left only
    image += generator.normal(0.0, 0.01, image.shape).astype(np.float32)
    return np.abs(image)


def sidecar_camera(matrix_camera):
    x, y, z, pitch, yaw = matrix_camera.split()
    return f"{x},{y},{z} pitch={pitch} yaw={yaw}"


def build_capture(directory, scene, seed, image=None, fields=None, environment=None,
                  samples=SAMPLES):
    combined = asymmetric_scene(seed) if image is None else image
    indirect = (combined * 0.25).astype(np.float32)
    direct = (combined - indirect).astype(np.float32)
    base = directory / f"{scene}-seed{seed}"
    definition = SCENES[scene]

    write_pfm(base.with_suffix(".pfm"), combined)
    write_pfm(directory / f"{base.name}.direct.pfm", direct)
    write_pfm(directory / f"{base.name}.indirect.pfm", indirect)
    write_bmp(base.with_suffix(".bmp"), engine_tonemap(combined, EXPOSURE))

    values = {
        "GPU": "NVIDIA GeForce RTX 3060 Laptop GPU",
        "Driver integer": "2290286592",
        "Vulkan API integer": "4206814",
        "Build": "Release",
        "Scene": definition["file"],
        "Trace revision": "7",
        "Scene hash": "123456789",
        "Dimensions": f"{WIDTH} x {HEIGHT}",
        "Render scale": "1",
        "Samples": str(samples),
        "Seed": str(seed),
        "Depth": "2",
        "Exposure EV": str(EXPOSURE),
        "Material model": "1",
        "Portal limit": "0",
        "Source revision/state": COMMIT,
        "Camera": sidecar_camera(definition["camera"]),
        "Sun radiance": "3,3,3",
        "Environment intensity": "1 black=0",
        "Emitter triangles": "2",
        "Direct mean": "0.4",
        "Indirect mean": "0.1",
    }
    values.update(fields or {})
    base.with_suffix(".txt").write_text(
        "".join(f"{k}: {v}\n" for k, v in values.items()), encoding="utf-8")

    env = {
        "MIRABILIS_CAPTURE": str(base),
        "MIRABILIS_SOURCE_STATE": COMMIT,
        "MIRABILIS_TEST_CAMERA": definition["camera"],
        "MIRABILIS_TEST_FRAMES": str(samples),
        "MIRABILIS_TEST_SCENE": definition["file"],
        "MIRABILIS_TEST_SEED": str(seed),
        "MIRABILIS_TEST_TRACE": "1",
        "VK_LAYER_VALIDATE_SYNC": "1",
    }
    env.update(environment or {})
    (directory / f"{base.name}.env.txt").write_text(
        "".join(f"{k}={v}\n" for k, v in sorted(env.items())), encoding="utf-8")
    return base


FAKE_HASH = "A" * 64


def write_side_files(directory, frames=SAMPLES, acceptable="yes", dirty="no"):
    (directory / "manifest.txt").write_text("\n".join([
        "R3 reference capture manifest",
        f"Acceptable as an R3 reference: {acceptable}",
        f"Commit: {COMMIT}",
        "Branch: recovery/r3-matched-references",
        f"Dirty tree: {dirty}",
        "Invocation: capture_r3_references.ps1",
        "Seeds: " + ", ".join(str(seed) for seed in SEEDS),
        f"Frames (accumulated samples per capture): {frames}",
        "Build configuration: Release",
    ]) + "\n", encoding="utf-8")
    (directory / "source.txt").write_text("placeholder\n", encoding="utf-8")
    (directory / "binary-sha256.txt").write_text(
        f"Algorithm : SHA256\nHash      : {FAKE_HASH}\nPath      : engine.exe\n",
        encoding="utf-8")
    (directory / "shader-sha256.txt").write_text(
        f"{FAKE_HASH}  ssgi_body.glsl\n{'B' * 64}  material_brdf.glsl\n",
        encoding="utf-8")
    (directory / "scene-sha256.txt").write_text(
        "".join(f"{FAKE_HASH}  {SCENES[scene]['file']}\n" for scene in SCENES),
        encoding="utf-8")
    frozen = directory / "r3_regions.json"
    frozen.write_text(
        (ROOT / "scripts" / "r3_regions.json").read_text(encoding="utf-8"),
        encoding="utf-8")
    digest = hashlib.sha256(frozen.read_bytes()).hexdigest().upper()
    (directory / "regions-sha256.txt").write_text(
        f"{digest}  r3_regions.json\n", encoding="utf-8")


def failing_lines(text, label):
    return [line for line in text.splitlines()
            if line.startswith("FAIL") and label in line]


def run_validator(directory):
    result = subprocess.run(
        [sys.executable, str(VALIDATOR), str(directory)],
        capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


class R3Validator(unittest.TestCase):
    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory()
        self.directory = pathlib.Path(self._temporary.name)
        self.addCleanup(self._temporary.cleanup)

    def build_clean(self, scenes=tuple(SCENES), seeds=SEEDS, **kwargs):
        write_side_files(self.directory,
                         frames=kwargs.pop("frames", SAMPLES),
                         acceptable=kwargs.pop("acceptable", "yes"),
                         dirty=kwargs.pop("dirty", "no"))
        for scene in scenes:
            for seed in seeds:
                build_capture(self.directory, scene, seed, **kwargs)

    def reference_files(self):
        return sorted(path.name for path in self.directory.glob("*-reference*"))

    # --- the happy path -------------------------------------------------
    def test_clean_capture_set_passes(self):
        self.build_clean()
        code, text = run_validator(self.directory)
        self.assertEqual(code, 0, text)
        self.assertIn("0 failed, 0 blocked", text)

    # --- finding 1: both scene groups required --------------------------
    def test_missing_cornell_fails(self):
        self.build_clean(scenes=("living-room",))
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "required scene cornell present"), text)

    def test_missing_living_room_fails(self):
        self.build_clean(scenes=("cornell",))
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "required scene living-room present"), text)

    def test_unexpected_scene_group_fails(self):
        self.build_clean()
        build_capture(self.directory, "cornell", 5)
        (self.directory / "cornell-seed5.pfm").rename(self.directory / "sponza-seed5.pfm")
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "no unexpected scene groups"), text)

    # --- finding 2: provenance ------------------------------------------
    def test_missing_section_7_1_field_fails(self):
        self.build_clean()
        build_capture(self.directory, "cornell", 1337, fields={"GPU": ""})
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "section 7.1 fields complete"), text)

    def test_debug_build_fails(self):
        self.build_clean()
        build_capture(self.directory, "cornell", 1337, fields={"Build": "Debug"})
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "Release build"), text)

    def test_wrong_source_revision_fails(self):
        self.build_clean()
        build_capture(self.directory, "cornell", 1337,
                      fields={"Source revision/state": "deadbeef"})
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "source revision equals manifest commit"),
                        text)

    def test_source_revision_with_the_commit_only_as_a_prefix_fails(self):
        """An accepted set must name the commit exactly, not merely start with it.

        "<commit> (dirty)" begins with the commit, so a prefix test would let a
        capture taken from an uncommitted tree pass inside a set the manifest
        calls clean.
        """
        self.build_clean()
        build_capture(self.directory, "cornell", 1337,
                      fields={"Source revision/state": COMMIT + " (dirty)"})
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "source revision equals manifest commit"),
                        text)

    def test_diagnostic_set_still_allows_the_dirty_suffix(self):
        """The suffix is only refused where exactness is claimed."""
        self.build_clean(acceptable="NO - diagnostic only", dirty="YES")
        build_capture(self.directory, "cornell", 1337,
                      fields={"Source revision/state": COMMIT + " (dirty)"})
        code, text = run_validator(self.directory)
        self.assertEqual(code, 4, text)
        self.assertFalse(failing_lines(text, "source revision"), text)

    def test_manifest_seed_set_must_equal_the_captured_seeds(self):
        self.build_clean()
        manifest = self.directory / "manifest.txt"
        manifest.write_text(
            manifest.read_text(encoding="utf-8").replace(
                "Seeds: 1337, 2026, 90210", "Seeds: 1337, 2026, 55555"),
            encoding="utf-8")
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(
            failing_lines(text, "capture seeds equal the manifest seed set"), text)

    def test_manifest_seed_superset_also_fails(self):
        """A manifest naming a run that never happened is not a record of one."""
        self.build_clean()
        manifest = self.directory / "manifest.txt"
        manifest.write_text(
            manifest.read_text(encoding="utf-8").replace(
                "Seeds: 1337, 2026, 90210", "Seeds: 1337, 2026, 90210, 4242"),
            encoding="utf-8")
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(
            failing_lines(text, "capture seeds equal the manifest seed set"), text)

    def test_debug_manifest_build_configuration_fails(self):
        self.build_clean()
        manifest = self.directory / "manifest.txt"
        manifest.write_text(
            manifest.read_text(encoding="utf-8").replace(
                "Build configuration: Release", "Build configuration: Debug"),
            encoding="utf-8")
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(
            failing_lines(text, "manifest: build configuration is Release"), text)

    def test_camera_off_the_frozen_matrix_fails(self):
        self.build_clean()
        build_capture(self.directory, "cornell", 1337,
                      fields={"Camera": "0,2,9.5 pitch=0 yaw=0"})
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "rendered camera position"), text)

    def test_camera_orientation_off_the_matrix_fails(self):
        self.build_clean()
        build_capture(self.directory, "cornell", 1337,
                      fields={"Camera": "0,2,3.5 pitch=0.9 yaw=0"})
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "rendered camera orientation"), text)

    def test_env_camera_off_the_matrix_fails(self):
        self.build_clean()
        build_capture(self.directory, "cornell", 1337,
                      environment={"MIRABILIS_TEST_CAMERA": "1 1 1 0 0"})
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "run camera matches frozen matrix"), text)

    def test_missing_env_file_fails(self):
        self.build_clean()
        (self.directory / "cornell-seed1337.env.txt").unlink()
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "run environment recorded"), text)

    def test_three_consistently_wrong_sample_counts_fail(self):
        """Agreement among the captures is not evidence; the manifest is."""
        self.build_clean(samples=64)
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "sample count matches manifest"), text)

    def test_missing_manifest_fails(self):
        self.build_clean()
        (self.directory / "manifest.txt").unlink()
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "manifest.txt present"), text)

    def test_missing_side_file_fails(self):
        self.build_clean()
        (self.directory / "shader-sha256.txt").unlink()
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "shader-sha256.txt present"), text)

    # --- decoding and image content -------------------------------------
    def test_fewer_than_three_seeds_fails(self):
        self.build_clean(seeds=(1337, 2026))
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "at least three distinct seeds"), text)

    def test_non_finite_value_fails(self):
        self.build_clean()
        broken = asymmetric_scene(1337)
        broken[3, 4, 1] = np.float32("nan")
        write_pfm(self.directory / "cornell-seed1337.pfm", broken)
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "all finite"), text)

    def test_negative_radiance_fails(self):
        self.build_clean()
        broken = asymmetric_scene(2026)
        broken[1, 1, 0] = np.float32(-0.5)
        write_pfm(self.directory / "cornell-seed2026.pfm", broken)
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "non-negative"), text)

    def test_sidecar_seed_mismatch_fails(self):
        self.build_clean()
        build_capture(self.directory, "cornell", 1337, fields={"Seed": "999"})
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "sidecar seed matches filename"), text)

    def test_dimension_mismatch_with_sidecar_fails(self):
        self.build_clean()
        build_capture(self.directory, "cornell", 1337,
                      fields={"Dimensions": "999 x 999"})
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "dimensions match sidecar"), text)

    def test_upside_down_pfm_is_caught_by_the_bmp(self):
        self.build_clean()
        # Flip every component but leave the BMP correct: that is the real
        # defect shape, a writer emitting rows the wrong way round while the
        # preview stays right. Flipping only the combined image would also
        # break the direct+indirect residual and pass for the wrong reason.
        combined = asymmetric_scene(1337)
        indirect = (combined * 0.25).astype(np.float32)
        direct = (combined - indirect).astype(np.float32)
        write_pfm(self.directory / "cornell-seed1337.pfm", np.flipud(combined).copy())
        write_pfm(self.directory / "cornell-seed1337.direct.pfm", np.flipud(direct).copy())
        write_pfm(self.directory / "cornell-seed1337.indirect.pfm", np.flipud(indirect).copy())
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertFalse(failing_lines(text, "reconstructs combined"),
                         "residual must still hold; only orientation is wrong")
        self.assertTrue(failing_lines(text, "align with engine bmp"), text)

    def test_vertically_symmetric_image_blocks_rather_than_passes(self):
        symmetric = np.ones((HEIGHT, WIDTH, 3), dtype=np.float32) * 0.4
        self.build_clean(image=symmetric)
        code, text = run_validator(self.directory)
        self.assertEqual(code, 3, text)
        self.assertIn("too vertically symmetric", text)
        self.assertIn("Blocked checks are not passes", text)

    # --- finding 3: the statistic is named for what it is ----------------
    def test_noise_reports_a_named_standard_deviation(self):
        self.build_clean()
        code, _ = run_validator(self.directory)
        self.assertEqual(code, 0)
        document = json.loads((self.directory / "noise.json").read_text(encoding="utf-8"))
        noise = document["noise"]
        self.assertEqual(set(noise["cornell"]), {"combined", "direct", "indirect"})
        for component in noise["cornell"].values():
            whole = component["whole"]
            self.assertIn("mean_seed_stddev", whole)
            self.assertNotIn("mean_abs_deviation", whole,
                             "a standard deviation must not be labelled a MAD")
            self.assertIn("deep_interior", component)
            # The numbers have to be real. Structure alone passed against a
            # version whose standard deviation had been multiplied by zero.
            self.assertGreater(whole["mean_seed_stddev"], 0.0)
            self.assertGreater(whole["relative_noise"], 0.0)
            self.assertEqual(len(set(whole["seed_means"])), 3)

    def test_reported_stddev_matches_numpy(self):
        """The label is only honest if the arithmetic behind it is."""
        self.build_clean()
        code, _ = run_validator(self.directory)
        self.assertEqual(code, 0)
        document = json.loads((self.directory / "noise.json").read_text(encoding="utf-8"))
        stack = np.stack([asymmetric_scene(seed) for seed in SEEDS])
        expected = float(stack.std(axis=0, ddof=1).mean())
        reported = document["noise"]["cornell"]["combined"]["whole"]["mean_seed_stddev"]
        self.assertAlmostEqual(reported, expected, places=5)

    # --- finding 5: canonical reference ----------------------------------
    def test_canonical_ensemble_reference_is_written(self):
        self.build_clean()
        code, text = run_validator(self.directory)
        self.assertEqual(code, 0, text)
        for scene in SCENES:
            for suffix in ("", ".direct", ".indirect"):
                self.assertTrue(
                    (self.directory / f"{scene}-reference{suffix}.pfm").exists(),
                    f"missing ensemble {scene}{suffix}")
            record = (self.directory / f"{scene}-reference.txt").read_text(
                encoding="utf-8")
            for seed in SEEDS:
                self.assertIn(str(seed), record, "contributing seeds not recorded")
            self.assertIn("mean of the contributing seeds", record)

    def test_ensemble_is_the_mean_not_a_chosen_seed(self):
        self.build_clean()
        self.assertEqual(run_validator(self.directory)[0], 0)
        sys.path.insert(0, str(ROOT / "scripts"))
        try:
            import importlib.util
            spec = importlib.util.spec_from_file_location("validator", VALIDATOR)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            ensemble, _ = module.read_pfm(self.directory / "cornell-reference.pfm")
        finally:
            sys.path.pop(0)
        expected = np.stack([asymmetric_scene(seed) for seed in SEEDS]).mean(axis=0)
        np.testing.assert_allclose(ensemble, expected, rtol=1e-5, atol=1e-6)
        for seed in SEEDS:
            self.assertGreater(
                float(np.abs(ensemble - asymmetric_scene(seed)).max()), 0.0,
                "ensemble is identical to a single seed")

    # --- two-phase global publication ------------------------------------
    def test_dirty_manifest_publishes_nothing_and_returns_non_acceptance(self):
        """A diagnostic set is not a reference, however clean its captures are."""
        self.build_clean(dirty="YES", acceptable="NO - diagnostic only")
        code, text = run_validator(self.directory)
        self.assertEqual(code, 4, text)
        self.assertEqual(self.reference_files(), [],
                         "a diagnostic capture set must publish no reference")
        self.assertIn("not acceptable as an R3 reference", text)

    def test_dirty_tree_alone_blocks_publication(self):
        """Isolates the dirty-tree gate: the manifest still says acceptable."""
        self.build_clean(dirty="YES")
        code, text = run_validator(self.directory)
        self.assertEqual(code, 4, text)
        self.assertEqual(self.reference_files(), [])

    def test_diagnostic_manifest_alone_blocks_publication(self):
        """Isolates the acceptability gate: the tree is clean, the set is not.

        Without this, disabling the acceptability check still passed, because
        every diagnostic fixture also had a dirty tree and the other gate
        caught it.
        """
        self.build_clean(acceptable="NO - diagnostic only")
        code, text = run_validator(self.directory)
        self.assertEqual(code, 4, text)
        self.assertEqual(self.reference_files(), [],
                         "a manifest marked diagnostic must publish nothing "
                         "even from a clean tree")

    def test_one_failing_scene_withholds_the_other_scenes_reference(self):
        """Publication is a claim about the set, so Cornell cannot pass alone."""
        self.build_clean()
        build_capture(self.directory, "living-room", 1337, fields={"Build": "Debug"})
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "Release build"), text)
        self.assertEqual(self.reference_files(), [],
                         "Cornell passed but must not be published while "
                         "living-room failed")

    def test_blocked_check_withholds_publication(self):
        symmetric = np.ones((HEIGHT, WIDTH, 3), dtype=np.float32) * 0.4
        self.build_clean(image=symmetric)
        code, _ = run_validator(self.directory)
        self.assertEqual(code, 3)
        self.assertEqual(self.reference_files(), [])

    def test_stale_reference_does_not_survive_a_later_failure(self):
        """Validate clean, corrupt a capture, validate again: nothing stale left."""
        self.build_clean()
        self.assertEqual(run_validator(self.directory)[0], 0)
        self.assertTrue(self.reference_files(), "first run should have published")

        broken = asymmetric_scene(1337)
        broken[2, 2, 0] = np.float32("nan")
        write_pfm(self.directory / "cornell-seed1337.pfm", broken)
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertEqual(self.reference_files(), [],
                         "an earlier run's reference survived a failing re-run")
        self.assertIn("Removed stale canonical outputs", text)

    # --- frozen regions ---------------------------------------------------
    def test_validation_uses_the_frozen_regions_not_the_repository_copy(self):
        """Edit the repository's regions after capture; validation must ignore it.

        The regions are frozen before the captures exist. If validation read
        today's file, someone could redefine "deep interior" after seeing a
        result and the numbers would silently change meaning.
        """
        self.build_clean()
        # Give the frozen copy a region name that exists nowhere else, and
        # rehash it so the integrity check still passes.
        frozen = self.directory / "r3_regions.json"
        document = json.loads(frozen.read_text(encoding="utf-8"))
        document["gi_cornell_box.json"]["regions"]["frozen_marker"] = {
            "x0": 0.1, "y0": 0.1, "x1": 0.4, "y1": 0.4}
        frozen.write_text(json.dumps(document, indent=2), encoding="utf-8")
        (self.directory / "regions-sha256.txt").write_text(
            f"{hashlib.sha256(frozen.read_bytes()).hexdigest().upper()}  r3_regions.json\n",
            encoding="utf-8")

        repository = ROOT / "scripts" / "r3_regions.json"
        original = repository.read_bytes()
        try:
            edited = json.loads(original.decode("utf-8"))
            edited["gi_cornell_box.json"]["regions"] = {
                "repository_only": {"x0": 0.0, "y0": 0.0, "x1": 0.2, "y1": 0.2}}
            repository.write_text(json.dumps(edited, indent=2), encoding="utf-8")
            code, text = run_validator(self.directory)
        finally:
            repository.write_bytes(original)

        self.assertEqual(code, 0, text)
        noise = json.loads(
            (self.directory / "noise.json").read_text(encoding="utf-8"))["noise"]
        regions = noise["cornell"]["combined"]
        self.assertIn("frozen_marker", regions,
                      "validation did not use the frozen regions file")
        self.assertNotIn("repository_only", regions,
                         "validation read the repository copy instead of the frozen one")

    def test_tampered_frozen_regions_fail_their_recorded_hash(self):
        self.build_clean()
        frozen = self.directory / "r3_regions.json"
        document = json.loads(frozen.read_text(encoding="utf-8"))
        document["gi_cornell_box.json"]["regions"]["deep_interior"]["x0"] = 0.999
        # Rewritten without updating regions-sha256.txt, which is the point.
        frozen.write_text(json.dumps(document, indent=2), encoding="utf-8")
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(
            failing_lines(text, "frozen regions match their recorded SHA-256"), text)

    def test_missing_regions_hash_file_fails(self):
        self.build_clean()
        (self.directory / "regions-sha256.txt").unlink()
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "regions-sha256.txt present"), text)

    # --- finding 6: hash manifests ---------------------------------------
    def test_malformed_shader_hash_records_fail(self):
        self.build_clean()
        (self.directory / "shader-sha256.txt").write_text(
            "not-a-hash  ssgi_body.glsl\n", encoding="utf-8")
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "shader-sha256.txt records"), text)

    def test_binary_hash_without_a_sha256_fails(self):
        self.build_clean()
        (self.directory / "binary-sha256.txt").write_text(
            "Algorithm : SHA256\nHash      : oops\n", encoding="utf-8")
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "binary-sha256.txt holds a SHA-256"), text)

    def test_scene_hash_missing_a_required_scene_fails(self):
        self.build_clean()
        (self.directory / "scene-sha256.txt").write_text(
            f"{FAKE_HASH}  gi_cornell_box.json\n", encoding="utf-8")
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(
            failing_lines(text, "scene hash recorded for living-room"), text)

    def test_empty_scene_hash_file_fails(self):
        self.build_clean()
        (self.directory / "scene-sha256.txt").write_text("", encoding="utf-8")
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertTrue(failing_lines(text, "scene-sha256.txt records"), text)

    def test_no_canonical_reference_when_the_set_fails(self):
        self.build_clean()
        build_capture(self.directory, "cornell", 1337, fields={"Build": "Debug"})
        code, text = run_validator(self.directory)
        self.assertEqual(code, 1)
        self.assertFalse((self.directory / "cornell-reference.pfm").exists(),
                         "a failing capture set must not produce a reference")
        self.assertIn("No canonical reference written", text)


class LastSceneProtection(unittest.TestCase):
    """Finding 4 lives in PowerShell, so these guard its structure."""

    def setUp(self):
        self.script = CAPTURE_SCRIPT.read_text(encoding="utf-8")

    def test_original_bytes_are_kept_not_just_a_hash(self):
        self.assertIn("[IO.File]::ReadAllBytes($lastScene)", self.script)
        self.assertIn("[IO.File]::WriteAllBytes($lastScene, $lastSceneBytes)", self.script)

    def test_checked_after_every_engine_process(self):
        capture_body = self.script[self.script.index("function Invoke-Capture"):
                                   self.script.index("Push-Location $repo",
                                                     self.script.index("function Invoke-Capture"))]
        self.assertIn("Assert-LastScene", capture_body,
                      "the check must run after each engine process, not only at the end")

    def test_checked_again_in_finally(self):
        finally_body = self.script[self.script.rindex("} finally {"):]
        self.assertIn("Test-LastScene", finally_body)
        self.assertIn("Restore-LastScene", finally_body)

    def test_a_modified_file_still_fails_the_capture(self):
        self.assertIn("throw ('.last_scene was modified by ", self.script)

    def test_timeout_waits_for_termination_before_touching_the_file(self):
        """Kill() returns immediately; reading the file mid-death races it."""
        timeout_block = self.script[self.script.index("$process.Kill()"):]
        wait_at = timeout_block.index("WaitForExit(30 * 1000)")
        assert_at = timeout_block.index("Assert-LastScene")
        self.assertLess(wait_at, assert_at,
                        "the .last_scene check must follow the wait, not the kill")

    def test_final_violation_fails_the_run_rather_than_warning(self):
        tail = self.script[self.script.rindex("} finally {"):]
        self.assertNotIn("Write-Warning", tail,
                         "a modified .last_scene must fail the run, not warn")
        self.assertIn("if ($lastSceneViolation) {", self.script)
        self.assertIn("throw $message", self.script)
        # The flag has to be raised where the violation is detected, not merely
        # tested afterwards: an assignment of $false there would leave the
        # throw below unreachable with every structural check still passing.
        tail = self.script[self.script.rindex("} finally {"):]
        self.assertIn("$lastSceneViolation = $true", tail)

    def test_capture_script_freezes_regions_and_records_their_hash(self):
        self.assertIn("$regionsFrozen = Join-Path $output 'r3_regions.json'", self.script)
        self.assertIn("regions-sha256.txt", self.script)

    def test_capture_script_passes_the_frozen_regions_to_the_validator(self):
        self.assertIn("--regions $regionsFrozen", self.script,
                      "the validator must be pointed at the frozen copy explicitly")

    def test_an_earlier_failure_message_is_preserved(self):
        self.assertIn("$captureError = $_", self.script)
        self.assertIn("$captureError.ToString() + [Environment]::NewLine + $message",
                      self.script)
        self.assertIn("if ($captureError) { throw $captureError }", self.script)


if __name__ == "__main__":
    unittest.main()
