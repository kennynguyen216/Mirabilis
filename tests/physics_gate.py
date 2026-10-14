"""Physics GPU gate: reuse the ECS runner and assert frozen telemetry contracts."""
import argparse
import copy
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import ecs_gate

STATIC_CACHES = ('sdf', 'surface', 'ray-shadow')
FIELDS = ('frame', 'bodies', 'sdf', 'surface', 'trace', 'ray-shadow',
          'eligible', 'dynamic-draws', 'history-reset', 'sdf-enabled',
          'surface-enabled', 'ray-shadow-enabled', 'trace-enabled')


def telemetry(logs):
    records = []
    for line in logs.splitlines():
        if not line.startswith('PHYSICS phase='):
            continue
        fields = dict(re.findall(r'([a-z][a-z-]*)=([^\s]+)', line))
        assert all(key in fields for key in ('phase', *FIELDS)), line
        records.append({'phase': fields['phase'], **{key: int(fields[key]) for key in FIELDS}})
    assert records, 'No physics telemetry was emitted'
    assert all(b['frame'] > a['frame'] for a, b in zip(records, records[1:])), 'Telemetry frame order'
    return records


def check_raster(records):
    phases = {phase: [record for record in records if record['phase'] == phase]
              for phase in ('awake', 'sleep', 'wake', 'teleport', 'restore',
                            'edit-restore', 'unchanged-restore', 'reopened', 'sandbox')}
    assert all(phases.values()), 'Missing lifecycle phase'
    awake, asleep = phases['awake'], phases['sleep']
    assert all(awake[-1][f'{cache}-enabled'] == 1 for cache in STATIC_CACHES), 'Required static cache capability inactive'
    assert len(awake) >= 4 and len(asleep) >= 2, 'Need repeated awake/sleep samples'
    assert all(item['dynamic-draws'] > 0 for item in awake), 'Awake dynamic geometry missing'
    for phase in (awake[2:], asleep):
        for cache in STATIC_CACHES:
            assert phase[-1][cache] == phase[0][cache], f'{cache} rebuilt every frame'
    for cache in STATIC_CACHES:
        assert 0 <= asleep[0][cache] - awake[-1][cache] <= 1, f'{cache} sleep rebuild batch'
        assert 0 <= phases['wake'][0][cache] - asleep[-1][cache] <= 1, f'{cache} wake rebuild batch'
        assert phases['unchanged-restore'][-1][cache] == phases['edit-restore'][-1][cache], f'{cache} unchanged restore rebuilt'
    assert asleep[-1]['eligible'] > awake[-1]['eligible'], 'Sleeping subtree never entered static caches'
    assert phases['wake'][0]['eligible'] < asleep[-1]['eligible'], 'Awake subtree stayed in static caches'
    assert phases['teleport'][0]['history-reset'] > 0, 'Teleport left previous-render history'
    assert phases['restore'][0]['history-reset'] > 0, 'Restore left previous-render history'
    assert phases['reopened'][-1]['bodies'] > 0 and phases['sandbox'][-1]['bodies'] > 0
    return phases


def child_variant(document):
    variant = copy.deepcopy(document)
    dynamic = [obj for obj in variant['objects']
               if obj.get('components', {}).get('rigidBody', {}).get('motion') == 'Dynamic']
    assert len(dynamic) == 1
    next_id = max(variant['nextPersistentId'], max(obj['id'] for obj in variant['objects']) + 1)
    variant['objects'].append({'id': next_id, 'parent': dynamic[0]['id'], 'siblingIndex': 0,
        'components': {'name': {'value': 'Physics gate visual glTF child'},
            'transform': {'position': [1, 0, 0], 'rotation': [0, .2, 0], 'scale': [.25, .25, .25]},
            'mesh': {'asset': 'gltf', 'modelPath': '../../assets/basicmesh.glb'},
            'visibility': {'visible': True, 'layer': 0},
            'collider': {'enabled': True, 'shape': 0, 'center': [0, 0, 0], 'halfExtents': [.5, .5, .5]},
            'rigidBody': {'motion': 'Static', 'mass': 1, 'friction': .5, 'restitution': 0, 'gravityFactor': 1}}})
    variant['nextPersistentId'] = next_id + 1
    return variant


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    repo, output = args.repo.resolve(), args.output.resolve()
    assert not output.exists() or not any(output.iterdir()), 'Use fresh evidence directory'
    output.mkdir(parents=True, exist_ok=True)
    ecs_gate.REPO, ecs_gate.OUT = repo, output
    scratch = output/'scratch-scenes'
    scratch.mkdir()
    for scene in (repo/'assets/scenes').glob('*.json'):
        shutil.copy2(scene, scratch/scene.name)
    original = json.loads((scratch/'physics_drop_demo.json').read_text())
    (scratch/'physics_child_demo.json').write_text(json.dumps(child_variant(original), indent=2))
    summary = {}
    for name, scene, trace in [('raster-demo', 'physics_drop_demo.json', False),
                               ('raster-child', 'physics_child_demo.json', False),
                               ('trace-child', 'physics_child_demo.json', True)]:
        captures = output/name/'captures'
        captures.mkdir(parents=True)
        flags = {'MIRABILIS_TEST_SCENE': scene, 'MIRABILIS_TEST_SCENE_DIRECTORY': str(scratch),
                 'MIRABILIS_TEST_PHYSICS_LIFECYCLE': '1', 'MIRABILIS_TEST_FRAMES': '200' if trace else '1080',
                 'MIRABILIS_TEST_WINDOW': '1600x900', 'MIRABILIS_TEST_HIDE_PANELS': '1',
                 'MIRABILIS_TEST_CAMERA': '0 2.5 12 -0.08 0',
                 'MIRABILIS_TEST_PHYSICS_CAPTURE_DIRECTORY': str(captures),
                 'MIRABILIS_LUMEN_LITE': '1', 'MIRABILIS_IQ_RT_SHADOWS': '1',
                 'VK_LAYER_VALIDATE_SYNC': '1', 'VK_KHRONOS_VALIDATION_VALIDATE_SYNC': 'true'}
        if trace:
            flags.update(MIRABILIS_TEST_TRACE='1', MIRABILIS_CAPTURE=str(captures/'trace'))
        logs, result = ecs_gate.run(name, 'Debug', flags, seconds=900)
        records = telemetry(logs)
        if trace:
            assert 'renderer=software' in logs and 'PHYSICS trace PASS' in logs
            awake = [item for item in records if item['phase'] == 'awake']
            assert len(awake) >= 2 and all(item['dynamic-draws'] > 0 for item in awake)
            assert awake[-1]['trace-enabled'] == 1
            assert awake[-1]['trace'] > awake[0]['trace'], 'Trace geometry did not update while body moved'
            assert (captures/'trace.pfm').is_file()
        else:
            assert 'PHYSICS lifecycle PASS' in logs and 'renderer=raster' in logs
            check_raster(records)
            for phase in ('awake', 'sleep'):
                assert (captures/f'{phase}.pfm').is_file(), f'Missing {phase} same-camera capture'
        for capture in captures.glob('*.pfm'):
            subprocess.run([sys.executable, str(repo/'scripts/pfm_to_png.py'), str(capture)], check=True)
        summary[name] = {**result, 'telemetry': records}
        (output/'physics-summary.json').write_text(json.dumps(summary, indent=2))
    assert summary['raster-child']['telemetry'][0]['bodies'] == summary['raster-demo']['telemetry'][0]['bodies'], 'Visual child created independent body'
    demo_awake = [item for item in summary['raster-demo']['telemetry'] if item['phase'] == 'awake']
    child_awake = [item for item in summary['raster-child']['telemetry'] if item['phase'] == 'awake']
    assert child_awake[-1]['dynamic-draws'] > demo_awake[-1]['dynamic-draws'], 'glTF child surfaces missing inherited dynamic metadata'
    print('PHYSICS GPU gate PASS', flush=True)


if __name__ == '__main__':
    main()
