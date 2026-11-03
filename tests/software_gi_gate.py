"""Validate screen-first distance-field GI independently of hardware sun shadows."""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import sys

import numpy as np
import trimesh
import ecs_gate
from lumen_gate import fixture, read_pfm


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--configuration', choices=('Debug', 'Release'), default='Debug')
    args = parser.parse_args()
    repo, out = Path(__file__).resolve().parents[1], args.output.resolve()
    assert not out.exists() or not any(out.iterdir()), 'Use fresh evidence directory'
    scenes = out/'scenes'; scenes.mkdir(parents=True)
    point = fixture(repo)
    dark = copy.deepcopy(point); dark['objects'][-1]['components']['light']['intensity'] = 0
    portal = copy.deepcopy(point)
    portal['authoredPortals'] = [{'hostWallObject':None, 'position':p, 'normal':n, 'up':[0,1,0],
        'halfWidth':1.1, 'halfHeight':1.5} for p,n in [([-4,1.7,3],[0,0,1]),([0,1.7,2],[0,0,-1])]]
    for name, doc in [('point', point), ('dark', dark), ('portal', portal), ('mirror', fixture(repo, True))]:
        (scenes/(name+'.json')).write_text(json.dumps(doc))
    ecs_gate.REPO, ecs_gate.OUT = repo, out
    common = {'MIRABILIS_QT_EDITOR':'0', 'MIRABILIS_TEST_SCENE':'point.json',
        'MIRABILIS_TEST_SCENE_DIRECTORY':str(scenes), 'MIRABILIS_TEST_WINDOW':'800x600',
        'MIRABILIS_TEST_CAMERA':'0 3.5 9 -0.3 0', 'MIRABILIS_TEST_HIDE_PANELS':'1',
        'MIRABILIS_LUMEN_HARDWARE':'0', 'MIRABILIS_LUMEN_LITE':'1',
        'MIRABILIS_SSGI_SCREEN_TRACE':'1', 'MIRABILIS_R4_UNCAP_FRAMES':'1',
        'MIRABILIS_BENCHMARK_SKIP_CPU_BVH_VALIDATION':'1',
        'MIRABILIS_R4_CONTRIBUTIONS':'env_diffuse,ray_screen_hit,ray_screen_hit_cache,ray_field_cache,ray_field_uncovered,ray_field_exit,ray_field_exhausted,ray_primary_cache_sky',
        'VK_INSTANCE_LAYERS':'VK_LAYER_KHRONOS_validation', 'VK_LAYER_VALIDATE_SYNC':'1',
        'VK_KHRONOS_VALIDATION_VALIDATE_SYNC':'true'}
    preflight = {**common, 'MIRABILIS_TEST_FRAMES':'2'}
    preflight.pop('MIRABILIS_SSGI_SCREEN_TRACE')
    logs, _ = ecs_gate.run('preflight', args.configuration, preflight)
    assert 'backend=software-distance-field screen-first=true' in logs
    ray_capable = 'hardware-sun=true' in logs
    default = {**preflight}; default.pop('MIRABILIS_LUMEN_HARDWARE')
    policy, _ = ecs_gate.run('default-policy', args.configuration, default)
    expected = 'backend=hardware screen-first=false' if ray_capable else 'backend=software-distance-field screen-first=true'
    assert expected in policy
    missing = set(re.findall(r'Scene SDF missing mesh: ([0-9a-f]+)', logs))
    if missing:
        sys.path.insert(0, str(repo/'scripts'))
        from bake_sdf import bake_world_grid, write_sdf
        for name in missing:
            mesh = trimesh.load(repo/'assets/sdf/queue'/(name+'.obj'), force='mesh', process=False)
            grid, lo, hi, _, _ = bake_world_grid(mesh)
            target = repo/'assets/sdf/cache'/(name+'.sdf')
            temporary = target.with_suffix('.software-test.partial')
            write_sdf(temporary, grid, lo, hi); os.replace(temporary, target)
    images, results = {}, {}
    for name, sun, scene, extra in [('point-sun-hardware','1','point',{}),
            ('point-sun-raster','0','point',{}),
            ('point-world','0','point',{'MIRABILIS_SSGI_SCREEN_TRACE':'0'}), ('dark','0','dark',{}),
            ('mirror','1','mirror',{'MIRABILIS_R4_CONTRIBUTIONS':'env_specular','MIRABILIS_TEST_CAMERA':'0 2.5 8 -0.6 0'}),
            ('portal','1','portal',{}), ('portal-offscreen','0','portal',
             {'MIRABILIS_TEST_PLAYER_PORTALS':'1','MIRABILIS_TEST_PORTAL_OFFSCREEN':'1'})]:
        if name=='mirror' and not ray_capable: continue
        logs, result = ecs_gate.run(name, args.configuration, {**common,
            'MIRABILIS_TEST_SCENE':scene+'.json',
            'MIRABILIS_IQ_RT_SHADOWS':sun, 'MIRABILIS_TEST_FRAMES':'240',
            'MIRABILIS_TEST_REQUIRE_LUMEN':'1', 'MIRABILIS_RASTER_CAPTURE':str(out/name), **extra})
        screen='false' if extra.get('MIRABILIS_SSGI_SCREEN_TRACE')=='0' else 'true'
        assert 'backend=software-distance-field screen-first='+screen in logs
        assert 'GI effective mode=lumen-lite' in logs
        fields = re.findall(r'GI world backend=distance-field instances=(\d+) ready=(\d+) missing-meshes=(\d+)', logs)
        assert fields and all(total==ready and missing=='0' for total, ready, missing in fields)
        assert 'mode=raster-emitters' in logs and 'mode=references' in logs
        if sun=='0': assert 'BLAS-builds=0 TLAS-builds=0' in logs
        if name=='mirror': assert 'reflections-active=true' in logs
        images[name], _ = read_pfm(out/(name+'.pfm')); assert np.isfinite(images[name]).all()
        results[name] = result
    difference = float(np.abs(images['point-sun-hardware']-images['point-sun-raster']).max())
    assert images['point-sun-raster'].mean()>1e-5, 'Software indirect lighting is absent'
    assert images['point-world'].mean()>1e-5, 'Distance-field fallback lighting is absent'
    if 'mirror' in images:
        assert images['mirror'].max()>.05, 'Hardware reflections cannot read software-GI lighting'
    assert np.abs(images['dark']).max()<1e-5, 'Software GI invents light'
    assert difference<.001, 'Sun backend changed software indirect lighting'
    (out/'summary.json').write_text(json.dumps({'runs':results, 'sunBackendMaxDifference':difference,
        'pointBounceMean':float(images['point-sun-raster'].mean())}, indent=2))
    print('SOFTWARE GI GPU GATE PASS', difference, flush=True)


if __name__ == '__main__':
    main()
