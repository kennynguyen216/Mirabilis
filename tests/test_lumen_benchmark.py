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


class CompletedGpuTimingValidation(unittest.TestCase):
    def test_completed_submission_ids_and_static_window(self):
        import tempfile
        import csv
        from lumen_benchmark import read_gpu_metrics
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'gpu.csv'
            base={'gpu_written_mask':3,'gpu_frame_ms':10,'gpu_forward_ms':8,'cpu_fence_ms':2,
                  'heap_usage':100,'heap_budget':200,**{k:0 for k in
                    ('sdf_rebuilds','card_rebuilds','trace_rebuilds','blas_builds','tlas_builds','albedo_rebuilds')}}
            def write(frames,change=None):
                rows=[dict(base,frame=n) for n in frames]
                if change:rows[-1].update(change)
                with path.open('w',newline='') as stream:
                    writer=csv.DictWriter(stream,fieldnames=rows[0]);writer.writeheader();writer.writerows(rows)
            # Out-of-order final-slot draining is valid; readback host IDs are irrelevant.
            write([1,2,4,3]);result=read_gpu_metrics(path,2,2)
            self.assertEqual(result['samples'],2)
            self.assertEqual(result['passCoverageFraction'],.8)
            self.assertEqual(result['unscopedGpuMs'],2)
            for frames,change in [([1,2,3],None),([3,3],None),([3,4],{'gpu_written_mask':0}),
                    ([3,4],{'gpu_frame_ms':'nan'}),([3,4],{'gpu_forward_ms':-1}),
                    ([3,4],{'albedo_rebuilds':1})]:
                with self.subTest(frames=frames,change=change):
                    write(frames,change)
                    with self.assertRaises(AssertionError):read_gpu_metrics(path,2,2)
            write([3,4],{'albedo_rebuilds':1})
            self.assertEqual(read_gpu_metrics(path,2,2,allow_rebuilds=True)['rebuilds']['albedo_rebuilds'],1)
