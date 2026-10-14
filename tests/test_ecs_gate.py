"""Headless checks for the measurement arithmetic and recorded collision fixtures."""
import hashlib
import json
from pathlib import Path
import unittest
import ecs_gate

ROOT = Path(__file__).resolve().parent.parent

class PerformanceProtocol(unittest.TestCase):
    def test_three_mean_median_and_spread(self):
        result = ecs_gate.timing_statistics([1.954,1.925,1.869])
        self.assertEqual(result['medianMeanFrameMs'],1.925)
        self.assertAlmostEqual(result['spreadPercent'],4.415584415584414)
        self.assertEqual(result['allowedSlowdownPercent'],5)

    def test_noisy_baseline_uses_twice_its_spread(self):
        result = ecs_gate.timing_statistics([4.439,3.720,4.258])
        self.assertEqual(result['medianMeanFrameMs'],4.258)
        self.assertAlmostEqual(result['allowedSlowdownPercent'],33.77172381399717)
        limit = result['medianMeanFrameMs']*(1+result['allowedSlowdownPercent']/100)
        self.assertAlmostEqual(limit,5.696)

    def test_fixture_and_original_scene_hashes_match_recorded_engine_capture(self):
        manifest = json.loads((ROOT/'tests/fixtures/ecs_collision_manifest.json').read_text())
        self.assertEqual(len(manifest),23)
        for name, record in manifest.items():
            with self.subTest(scene=name):
                fixture = ROOT/'tests/fixtures/ecs_collision'/name
                self.assertEqual(hashlib.sha256(fixture.read_bytes().replace(b'\r\n',b'\n')).hexdigest(),record['normalizedFixtureSha256'])
                scene = ROOT/'assets/scenes'/name
                self.assertEqual(hashlib.sha256(scene.read_bytes().replace(b'\r\n',b'\n')).hexdigest(),record['normalizedSceneSha256'])
                data = json.loads(fixture.read_text())
                self.assertEqual({k:len(v) for k,v in data.items()},record['counts'])

if __name__ == '__main__':
    unittest.main()
