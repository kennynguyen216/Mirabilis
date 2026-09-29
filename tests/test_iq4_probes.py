"""IQ4 screen probes: the Q1 analyzer's rejections and the uncovered gather's clamp.

The analyzer checks run on synthetic readouts written in the engine's PFM
convention.  The clamp check reads the shader source (nothing here needs a
GPU): an uncovered pixel must take both coefficient sets summed before the
nonnegative clamp, as its own rays would, never each set clamped apart.

Run: python -m unittest discover -s tests
"""
import pathlib
import re
import sys
import tempfile
import unittest

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parent.parent
SSGI = ROOT / "shaders" / "ssgi_body.glsl"
sys.path.insert(0, str(ROOT / "scripts"))
import iq4_q1  # noqa: E402


def write_pfm(path, rgb):
    # Bottom row first with a negative scale, as capture_ssgi writes.
    h, w, _ = rgb.shape
    with open(path, "wb") as f:
        f.write(f"PF\n{w} {h}\n-1.0\n".encode())
        f.write(np.ascontiguousarray(rgb[::-1], dtype="<f4").tobytes())


def readout(g, served=True):
    probe = np.zeros((4, 4, 3), np.float32)
    probe[..., 0] = 1.0 if served else 0.0
    probe[..., 1] = g
    return probe


class Q1Analyzer(unittest.TestCase):
    def test_all_ones_pass(self):
        self.assertTrue(iq4_q1.q1(readout(1.0))[0])

    def test_nonfinite_served_value_fails(self):
        for bad in (np.nan, np.inf):
            probe = readout(1.0)
            probe[2, 1, 1] = bad
            passed, stats = iq4_q1.q1(probe)
            self.assertFalse(passed)
            self.assertEqual(stats["nonfinite"], 1)

    def test_empty_served_set_fails(self):
        passed, stats = iq4_q1.q1(readout(1.0, served=False))
        self.assertFalse(passed)
        self.assertEqual(stats["served"], 0)

    def test_nan_survives_the_file_round_trip(self):
        with tempfile.TemporaryDirectory() as d:
            nan = readout(1.0)
            nan[0, 0, 1] = np.nan
            write_pfm(pathlib.Path(d) / "cornell-L0-full-f300-ssgi.probe.pfm", nan)
            write_pfm(pathlib.Path(d) / "living-room-L0-full-f300-ssgi.probe.pfm", readout(1.0))
            results = iq4_q1.analyse(d)
        self.assertFalse(results["cornell"][0])
        self.assertTrue(results["living-room"][0])


    def test_every_violation_is_listed(self):
        probe = readout(1.0)
        probe[1, 2, 1] = 0.97
        probe[3, 0, 1] = np.nan
        probe[0, 0, 0] = 0.0          # not served: never a violation
        probe[0, 0, 1] = 5.0
        self.assertEqual([(y, x) for y, x, _ in iq4_q1.violations(probe)], [(1, 2), (3, 0)])

    def test_fallback_count_comes_from_the_run_log(self):
        log = "x\nSSGI screen trace: 4160 pixels traced, hit rate 0.3516, mean steps 4.88\n"
        self.assertEqual(iq4_q1.fallback_pixels(log), 4160)
        with self.assertRaises(ValueError):
            iq4_q1.fallback_pixels("no trace line")


class ProbeSampling(unittest.TestCase):
    """Step 2c: uniform stratified rays and the normal-compatibility test."""

    def setUp(self):
        self.text = SSGI.read_text(encoding="utf-8")

    def test_uniform_rays_weight_two_pi(self):
        self.assertIn("vec3 weighted = incident * (2.0 * Pi);", self.text)
        self.assertIn("float cosTheta = (float(lane) + randomFloat(state)) / 64.0;", self.text)
        self.assertIn("bitfieldReverse(lane) >> 26", self.text)
        self.assertNotIn("Pi / cosTheta", self.text)

    def test_gather_drops_probes_below_the_normal_threshold(self):
        self.assertRegex(self.text, r"const float ProbeNormalCos = 0\.98;")
        self.assertRegex(self.text, r"if \(facing < ProbeNormalCos\) continue;")
        # (1 + cos beta) / 2 at the threshold loses at most 1% of Q1's 2%.
        self.assertAlmostEqual((1 + 0.98) / 2, 0.99)


class UncoveredGatherClamp(unittest.TestCase):
    def setUp(self):
        self.text = SSGI.read_text(encoding="utf-8")

    def test_probe_incident_clamps_after_summing_sets(self):
        body = re.search(r"vec3 probe_incident\(.*?\n}\n", self.text, re.S).group(0)
        self.assertLess(body.index("if (set != 0) c +="), body.index("return max(e, vec3(0.0));"))

    def test_uncovered_pixel_takes_the_combined_set(self):
        self.assertRegex(self.text, r"gatheredAll \+= probe_incident\(probe, o\.worldNormal, 2\)")
        self.assertRegex(self.text,
            r"primarySkyCached\s*\?\s*gathered / weightSum \+ r4_ray_value\(RayPrimaryCacheSky, "
            r"primarySky\.sky\)\s*:\s*gatheredAll / weightSum;")
        # No separately clamped exit set may be added anywhere.
        self.assertNotIn("probe_incident(probe, o.worldNormal, 1)", self.text)


if __name__ == "__main__":
    unittest.main()
