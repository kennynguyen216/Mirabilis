import unittest
import json
from pathlib import Path
from lumen_benchmark import parse_metrics, light_edit_scene


class LumenTimingValidation(unittest.TestCase):
    def test_legacy_living_room_edit_fixture(self):
        doc = json.loads((Path(__file__).resolve().parents[1]/'assets/scenes/living_room_showcase.json').read_text())
        room = next(obj for obj in doc['objects'] if obj['asset']=='gltf')
        expected = (room['position'],room['modelPath'])
        edited = light_edit_scene(doc)
        self.assertEqual(edited['version'],2)
        self.assertIsNone(edited['objects'][0]['parent'])
        self.assertEqual((room['components']['transform']['position'],room['components']['mesh']['modelPath']),expected)
        self.assertEqual(edited['objects'][-1]['components']['light']['intensity'],20)
        self.assertGreater(edited['nextPersistentId'],edited['objects'][-1]['id'])

    def log(self):
        return ('GI effective mode=lumen-lite\nSurface cache lit: 20 cards over 15 frames\n'
                'GI benchmark focused samples: 300/300\npending-texels=0\n'
                'average-frame-ms=5.0 average-ssgi-gpu-ms=2.0 samples=300')

    def test_real_ready_samples(self):
        self.assertEqual(parse_metrics(self.log(),300,True,60)['fps'],200)

    def test_rejects_fallback_incomplete_cache_focus_and_late_warmup(self):
        for bad in [self.log().replace('mode=lumen-lite','mode=plain-ssgi'),
                    self.log().replace('pending-texels=0','pending-texels=100'),
                    self.log().replace('300/300','299/300'),
                    self.log().replace('over 15 frames','over 65 frames')]:
            with self.subTest(log=bad):
                with self.assertRaises(AssertionError):parse_metrics(bad,300,True,60)

    def test_plain_mode_does_not_require_a_world_cache(self):
        self.assertEqual(parse_metrics(self.log().replace('mode=lumen-lite','mode=plain-ssgi'),300,False,60)['giGpuMs'],2)
