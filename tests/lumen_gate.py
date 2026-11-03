"""GPU checks for punctual bounce lighting, offscreen reflections and compact uploads."""
import argparse
import copy
import json
from pathlib import Path
import re
import sys

import numpy as np
import ecs_gate

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from validate_r3_capture import read_pfm


def fixture(repo, mirror=False):
    doc = json.loads((repo / 'assets/scenes/graphics_showcase.json').read_text())
    doc['objects'] = doc['objects'][:1]
    doc['referenceLighting'] = {'sunRadiance': [0, 0, 0], 'environmentIntensity': 0,
                                'blackEnvironment': True}
    doc['rendering'] = {'lumenGI': True, 'bloom': False, 'sceneReflections': mirror}

    def box(name, position, scale, color, metallic=0, roughness=.8, emission=None):
        obj = {'id': len(doc['objects']), 'parent': 0, 'siblingIndex': len(doc['objects'])-1,
               'components': {'name': {'value': name},
                   'transform': {'position': position, 'rotation': [0, 0, 0], 'scale': scale},
                   'mesh': {'asset': 'cube', 'modelPath': ''},
                   'material': {'enabled': True, 'tint': color, 'metallic': metallic,
                                'roughness': roughness}}}
        if emission:
            obj['components']['material'].update(emissionColor=emission, emissionStrength=8)
        doc['objects'].append(obj)

    box('Receiver', [0, -.1, 0], [12, .2, 12], [.85, .85, .85, 1],
        metallic=1 if mirror else 0, roughness=.045 if mirror else .8)
    doc['objects'][1]['components']['mesh']['asset']='floor'
    doc['objects'][1]['components']['transform'].update(position=[0,0,0],scale=[12,1,12])
    if mirror:
        # Above the primary frustum; visible from the floor's reflected camera.
        box('Offscreen emitter', [0, 5, -4], [2, 2, .2], [1, 1, 1, 1], emission=[1, .2, .03])
    else:
        box('Bounce wall', [0, 2.5, -4], [10, 5, .2], [.75, .1, .05, 1])
        doc['objects'].append({'id': 3, 'parent': 0, 'siblingIndex': 2, 'components': {
            'name': {'value': 'Punctual source'},
            'transform': {'position': [0, 3, -1], 'rotation': [0, 0, 0], 'scale': [1, 1, 1]},
            'light': {'type': 'Point', 'intensity': 40, 'range': 15, 'color': [1, .8, .6],
                      'castShadows': True}}})
    doc['nextPersistentId'] = 100
    return doc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--configuration', choices=['Debug', 'Release'], default='Debug')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    assert not out.exists() or not any(out.iterdir()), 'Use fresh evidence directory'
    out.mkdir(parents=True, exist_ok=True)
    scenes = out / 'scenes'
    scenes.mkdir()
    ecs_gate.REPO, ecs_gate.OUT = repo, out
    light, mirror = fixture(repo), fixture(repo, True)
    variants = {'point': light, 'mirror': mirror}
    variants['dark'] = copy.deepcopy(light)
    variants['dark']['objects'][-1]['components']['light']['intensity'] = 0
    variants['spot'] = copy.deepcopy(light)
    variants['spot']['objects'][-1]['components']['light'].update(type='Spot', innerCone=.2, outerCone=.6)
    variants['rough'] = copy.deepcopy(mirror)
    variants['rough']['objects'][1]['components']['material']['roughness'] = .3
    variants['threshold']=copy.deepcopy(variants['rough'])
    variants['threshold']['rendering']['reflectionRoughness']=.3
    variants['portal'] = copy.deepcopy(mirror)
    variants['portal']['authoredPortals'] = [
        {'hostWallObject': None, 'position': position, 'normal': normal, 'up': [0,1,0],
         'halfWidth': 1.1, 'halfHeight': 1.5}
        for position, normal in [([-4,1.7,3],[0,0,1]), ([0,1.7,2],[0,0,-1])]]
    for name, doc in variants.items():
        (scenes / (name+'.json')).write_text(json.dumps(doc))
    images, results = {}, {}
    cases = [('point','point',{}), ('dark','dark',{}), ('spot','spot',{}),
             ('mirror-off','mirror',{'MIRABILIS_SCENE_REFLECTIONS':'0'}),
             ('mirror-on','mirror',{'MIRABILIS_SCENE_REFLECTIONS':'1'}),
             ('emission-primary','mirror',{'MIRABILIS_SCENE_REFLECTIONS':'0','MIRABILIS_R4_CONTRIBUTIONS':'emission'}),
             ('rough','rough',{}), ('threshold','threshold',{}), ('furnace','rough',{'MIRABILIS_TEST_REFLECTION_FURNACE':'1'}), ('emitter-full-upload','mirror',{'MIRABILIS_TEST_FULL_TRACE_UPLOAD':'1'}),
             ('portal','portal',{}),
             ('portal-offscreen','portal',{'MIRABILIS_TEST_PLAYER_PORTALS':'1','MIRABILIS_TEST_PORTAL_OFFSCREEN':'1'})]
    for name, scene, extra in cases:
        capture = out / (name+'-hdr')
        flags = {'MIRABILIS_QT_EDITOR':'0', 'MIRABILIS_TEST_SCENE':scene+'.json',
            'MIRABILIS_TEST_SCENE_DIRECTORY':str(scenes), 'MIRABILIS_TEST_WINDOW':'800x600',
            'MIRABILIS_TEST_CAMERA':'0 2.5 8 -0.6 0' if scene in ('mirror','rough','threshold','portal') else '0 3.5 9 -0.3 0',
            'MIRABILIS_TEST_HIDE_PANELS':'1', 'MIRABILIS_TEST_FRAMES':'180',
            'MIRABILIS_LUMEN_LITE':'1', 'MIRABILIS_TEST_REQUIRE_LUMEN':'1',
            'MIRABILIS_LUMEN_HARDWARE':'1', 'MIRABILIS_SSGI_SCREEN_TRACE':'0',
            'MIRABILIS_BENCHMARK_SKIP_CPU_BVH_VALIDATION':'1',
            'MIRABILIS_R4_UNCAP_FRAMES':'1', 'MIRABILIS_RASTER_CAPTURE':str(capture),
            'MIRABILIS_R4_CONTRIBUTIONS':'env_diffuse,ray_field_cache,ray_field_uncovered,ray_field_exit,ray_field_exhausted,ray_primary_cache_sky' if scene in ('point','dark','spot') else 'env_specular',
            'VK_INSTANCE_LAYERS':'VK_LAYER_KHRONOS_validation', 'VK_LAYER_VALIDATE_SYNC':'1',
            'VK_KHRONOS_VALIDATION_VALIDATE_SYNC':'true', **extra}
        logs, result = ecs_gate.run(name, args.configuration, flags, seconds=240)
        assert 'GI effective mode=lumen-lite' in logs and 'no blocking relight submissions' in logs
        assert 'distance-field-bakes-required=false' in logs
        assert 'GI CPU geometry: mode='+('full' if name=='emitter-full-upload' else 'references') in logs
        if name in ('mirror-on','rough','portal','portal-offscreen'):
            assert 'reflections-active=true' in logs, 'Reflection path did not run'
        images[name], _ = read_pfm(capture.with_suffix('.pfm'))
        assert np.isfinite(images[name]).all()
        results[name] = result
    diff = {'pointBounceMean':float(images['point'].mean()), 'spotBounceMean':float(images['spot'].mean()),
            'darkMax':float(np.abs(images['dark']).max()),
            'offscreenEmissionPrimaryMax':float(np.abs(images['emission-primary']).max()),
            'mirrorOnMax':float(images['mirror-on'].max()), 'mirrorOffMax':float(images['mirror-off'].max()),
            'thresholdDifference':float(np.abs(images['rough']-images['threshold']).max()), 'roughMax':float(images['rough'].max()), 'furnaceMax':float(images['furnace'].max()), 'furnaceMean':float(images['furnace'].mean()),
            'compactUploadMaxDifference':float(np.abs(images['mirror-on']-images['emitter-full-upload']).max())}
    (out/'summary.json').write_text(json.dumps({'runs':results,'differences':diff},indent=2))
    assert diff['pointBounceMean']>1e-4 and diff['spotBounceMean']>1e-4
    assert diff['darkMax']<1e-5
    assert diff['offscreenEmissionPrimaryMax']<1e-5, 'Emitter is visible directly'
    assert diff['mirrorOnMax']>0.1 and diff['mirrorOffMax']<1e-5
    assert diff['thresholdDifference']<1e-4
    assert diff['roughMax']>0.01
    assert 0.1<diff['furnaceMean']<1.001 and diff['furnaceMax']<=1.001
    assert diff['compactUploadMaxDifference']<1e-4
    print('LUMEN GPU GATE PASS',json.dumps(diff),flush=True)


if __name__ == '__main__':
    main()
