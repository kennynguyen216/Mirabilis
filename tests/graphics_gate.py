"""Render the graphics showcase and verify feature changes and conservative GPU visibility."""
import argparse
import json
from pathlib import Path
import struct
import sys

import numpy as np
import ecs_gate

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from validate_r3_capture import read_pfm


def occlusion_fixture(root, document):
    # A dense imported grid behind a smaller opaque wall leaves both hidden
    # clusters and visible edge clusters, including mirrored/scaled instances.
    vertices, indices = [], []
    width, height = 64, 32
    for y in range(height + 1):
        for x in range(width + 1):
            vertices.extend((x / width * 10 - 5, y / height * 6, -2))
    for y in range(height):
        for x in range(width):
            a = y * (width + 1) + x
            indices.extend((a, a + 1, a + width + 1, a + 1, a + width + 2, a + width + 1))
    positions = struct.pack('<' + 'f' * len(vertices), *vertices)
    index_bytes = struct.pack('<' + 'I' * len(indices), *indices)
    (root / 'grid.bin').write_bytes(positions + index_bytes)
    gltf = {'asset': {'version': '2.0'}, 'buffers': [{'uri': 'grid.bin', 'byteLength': len(positions + index_bytes)}],
            'bufferViews': [{'buffer': 0, 'byteLength': len(positions)}, {'buffer': 0, 'byteOffset': len(positions), 'byteLength': len(index_bytes)}],
            'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': len(vertices) // 3, 'type': 'VEC3', 'min': [-5, 0, -2], 'max': [5, 6, -2]},
                          {'bufferView': 1, 'componentType': 5125, 'count': len(indices), 'type': 'SCALAR'}],
            'materials': [{'pbrMetallicRoughness': {'baseColorFactor': [0.4, 0.7, 0.3, 1], 'metallicFactor': 0, 'roughnessFactor': 1}, 'doubleSided': True}],
            'meshes': [{'primitives': [{'attributes': {'POSITION': 0}, 'indices': 1, 'material': 0}]}],
            'nodes': [{'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0}
    model = root / 'grid.gltf'
    model.write_text(json.dumps(gltf), encoding='utf-8')
    document = json.loads(json.dumps(document))
    document['rendering'] = {'bloom': False}
    document['objects'] = document['objects'][:2]
    for i, (position, scale, asset, path) in enumerate([
        ([0, 0, 0], [1, 1, 1], 'gltf', str(model)),
        ([0.3, 0.1, -1], [-0.8, 0.7, 1], 'gltf', str(model)),
        ([0, 2, 0], [6, 4, 0.3], 'cube', '')], 2):
        document['objects'].append({'id': i, 'parent': 0, 'siblingIndex': i - 1, 'components': {
            'name': {'value': 'Occlusion fixture ' + str(i)},
            'transform': {'position': position, 'rotation': [0, 0, 0], 'scale': scale},
            'mesh': {'asset': asset, 'modelPath': path}}})
    document['nextPersistentId'] = 10
    return document


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--configuration', choices=['Debug', 'Release'], default='Debug')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    assert not output.exists() or not any(output.iterdir()), 'Use a fresh output directory'
    output.mkdir(parents=True, exist_ok=True)
    scenes = output / 'scenes'
    scenes.mkdir()
    ecs_gate.REPO, ecs_gate.OUT = repo, output
    original = json.loads((repo / 'assets/scenes/graphics_showcase.json').read_text(encoding='utf-8'))
    variants = {}
    for name in ['pbr', 'unused-lights', 'cel', 'material-cel', 'no-bloom', 'no-shadows', 'no-glass']:
        doc = json.loads(json.dumps(original))
        if name == 'unused-lights':
            inactive = []
            for i in range(24):
                inactive.append({'id': 20+i, 'parent': 0, 'siblingIndex': 7+i, 'components': {
                    'name': {'value': 'Inactive light '+str(i)},
                    'light': {'type':'Point','intensity':0 if i<23 else 100,'color':[1,1,1] if i<23 else [0,0,0]}}})
            doc['objects'] = doc['objects'][:-2]+inactive+doc['objects'][-2:]
            doc['nextPersistentId'] = 100
        elif name == 'cel':
            doc['rendering']['celShading'] = True
        elif name == 'material-cel':
            for obj in doc['objects']:
                if obj['components']['name']['value'] in ['Warm ceramic', 'Refractive glass']:
                    obj['components']['material']['celShading'] = True
        elif name == 'no-bloom':
            doc['rendering']['bloom'] = False
        elif name == 'no-shadows':
            for obj in doc['objects']:
                if 'light' in obj['components']:
                    obj['components']['light']['castShadows'] = False
        elif name == 'no-glass':
            next(obj for obj in doc['objects'] if obj['components']['name']['value'] == 'Refractive glass')['components']['visibility'] = {'visible': False}
        variants[name] = doc
    portal = json.loads(json.dumps(original))
    portal['authoredPortals'] = [{'hostWallObject': None, 'position': position, 'normal': normal, 'up': [0,1,0], 'halfWidth': 1.1, 'halfHeight': 1.5}
                                 for position, normal in [([-4,1.7,3],[0,0,1]),([0,1.7,2],[0,0,-1])]]
    variants['portal'] = portal
    instances=json.loads(json.dumps(original))
    instances['objects']=instances['objects'][:2]
    instances['rendering']={'bloom':False}
    for i in range(8):
        instances['objects'].append({'id':20+i,'parent':0,'siblingIndex':1+i,'components':{
            'name':{'value':'Instance '+str(i)},
            'transform':{'position':[(i%4)*2-3,1,(i//4)*-3-1],'rotation':[0,i*0.12,0],
                         'scale':[-0.8 if i%2 else 0.8,1.2+i*0.1,0.7]},
            'mesh':{'asset':'cube','modelPath':''}}})
    instances['nextPersistentId']=100
    variants['instances']=instances
    variants['occlusion'] = occlusion_fixture(scenes, original)
    for name, doc in variants.items():
        (scenes / (name + '.json')).write_text(json.dumps(doc), encoding='utf-8')
    images, results = {}, {}
    for name in ['pbr', 'unused-lights', 'cel', 'material-cel', 'no-bloom', 'no-shadows', 'no-glass', 'occlusion-off', 'occlusion-on', 'portal', 'portal-offscreen', 'instances-on', 'instances-off']:
        scene = 'occlusion' if name.startswith('occlusion') else 'portal' if name.startswith('portal') else 'instances' if name.startswith('instances') else name
        capture = output / (name + '-hdr')
        logs, result = ecs_gate.run(name, args.configuration, {
            'MIRABILIS_QT_EDITOR': '0', 'MIRABILIS_TEST_SCENE': scene + '.json',
            'MIRABILIS_TEST_SCENE_DIRECTORY': str(scenes), 'MIRABILIS_TEST_FRAMES': '48',
            'MIRABILIS_TEST_WINDOW': '1280x800', 'MIRABILIS_TEST_CAMERA': '0 3.5 10 -0.12 0',
            'MIRABILIS_TEST_HIDE_PANELS': '1', 'MIRABILIS_R4_UNCAP_FRAMES': '1',
            'MIRABILIS_SSGI_DISABLE': '1', 'MIRABILIS_TEST_CLUSTER_COUNTS': '1', 'MIRABILIS_MESH_CULLING': '1' if name == 'occlusion-on' else '0',
            'MIRABILIS_RASTER_CAPTURE': str(capture), 'MIRABILIS_TEST_COLOR_CAPTURE': str(output / (name + '-color')),
            'VK_INSTANCE_LAYERS': 'VK_LAYER_KHRONOS_validation', 'VK_LAYER_VALIDATE_SYNC': '1',
            'VK_KHRONOS_VALIDATION_VALIDATE_SYNC': 'true',
            **({'MIRABILIS_TEST_PLAYER_PORTALS':'1','MIRABILIS_TEST_PORTAL_OFFSCREEN':'1'} if name=='portal-offscreen' else {}),
            **({'MIRABILIS_PROFILE_TRACE':str(output/(name+'-trace.json'))} if name.startswith('instances') else {}),
            **({'MIRABILIS_TEST_DISABLE_INSTANCING':'1'} if name=='instances-off' else {})}, seconds=180)
        images[name], _ = read_pfm(capture.with_suffix('.pfm'))
        assert np.isfinite(images[name]).all(), name + ' produced non-finite pixels'
        results[name] = result
        if name == 'occlusion-on':
            import re
            match = re.search(r'GPU cluster culling: (\d+) / (\d+)', logs)
            assert match and int(match[1]) > 0 and int(match[1]) < int(match[2]), 'Expected both hidden and visible GPU clusters'
    # HDR captures are taken before post-processing, so cel and bloom must be
    # compared in the actual display output rather than the linear radiance.
    from PIL import Image
    display = {name: np.asarray(Image.open(output / (name + '-color/frame_0000.ppm')), dtype=np.float32)
               for name in ['pbr', 'cel', 'material-cel', 'no-bloom']}
    differences = {}
    for name in ['cel', 'material-cel', 'no-bloom']:
        differences[name] = float(np.abs(display[name] - display['pbr']).mean())
        assert differences[name] > 0.02, name + ' had no visible effect'
    for name in ['no-shadows', 'no-glass']:
        differences[name] = float(np.abs(images[name] - images['pbr']).mean())
        assert differences[name] > 0.0001, name + ' had no radiance effect'
    unused_delta = np.abs(images['unused-lights'] - images['pbr'])
    differences['unusedLightsMaxAbsolute'] = float(unused_delta.max())
    assert unused_delta.max() <= 0.002, 'Inactive lights consumed real-light or shadow slots'
    instance_delta=np.abs(images['instances-on']-images['instances-off'])
    differences['instancingMaxAbsolute']=float(instance_delta.max())
    assert instance_delta.max()<=0.002, 'Instanced transforms/normals differ from ordinary draws'
    draw_counts={}
    for name in ['instances-on','instances-off']:
        trace=json.loads((output/(name+'-trace.json')).read_text(encoding='utf-8'))
        draw_counts[name]=[e['args']['value'] for e in trace['traceEvents'] if e['ph']=='C' and e['name']=='Draw calls'][-1]
    differences['instanceDrawCalls']=draw_counts
    assert draw_counts['instances-on']<draw_counts['instances-off'], 'Compatible objects were not batched'
    delta = np.abs(images['occlusion-on'] - images['occlusion-off'])
    differences['cullingMaxAbsolute'] = float(delta.max())
    assert delta.max() <= 0.002, 'Conservative visibility changed visible geometry'
    (output / 'summary.json').write_text(json.dumps({'runs': results, 'differences': differences}, indent=2), encoding='utf-8')
    print('GRAPHICS GPU GATE PASS', json.dumps(differences), flush=True)


if __name__ == '__main__':
    main()
