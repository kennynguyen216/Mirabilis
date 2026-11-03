"""Trace different emissive materials in one mirrored, nonuniformly scaled mesh BLAS."""
import argparse
import base64
import json
from pathlib import Path
import struct

import numpy as np
import ecs_gate
from lumen_gate import fixture, read_pfm


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--configuration',choices=('Debug','Release'),default='Debug')
    args=parser.parse_args()
    repo,out=Path(__file__).resolve().parents[1],args.output.resolve()
    assert not out.exists() or not any(out.iterdir()), 'Use fresh evidence directory'
    scenes=out/'scenes';scenes.mkdir(parents=True)
    points=[(x,y,-4) for lo,hi in [(-2,-.2),(.2,2)] for x,y in [(lo,4),(hi,4),(hi,6),(lo,6)]]
    positions=struct.pack('<24f',*(v for p in points for v in p))
    normals=struct.pack('<24f',*([0,0,1]*8))
    indices=struct.pack('<12H',0,1,2,0,2,3,4,5,6,4,6,7)
    data=positions+normals+indices
    model={'asset':{'version':'2.0'},'buffers':[{'byteLength':len(data),'uri':'data:application/octet-stream;base64,'+base64.b64encode(data).decode()}],
        'bufferViews':[{'buffer':0,'byteOffset':0,'byteLength':len(positions)},
            {'buffer':0,'byteOffset':len(positions),'byteLength':len(normals)},
            {'buffer':0,'byteOffset':len(positions)+len(normals),'byteLength':len(indices)}],
        'accessors':[{'bufferView':0,'componentType':5126,'count':8,'type':'VEC3','min':[-2,4,-4],'max':[2,6,-4]},
            {'bufferView':1,'componentType':5126,'count':8,'type':'VEC3'},
            {'bufferView':2,'componentType':5123,'count':6,'type':'SCALAR'},
            {'bufferView':2,'byteOffset':12,'componentType':5123,'count':6,'type':'SCALAR'}],
        'materials':[{'emissiveFactor':color} for color in ([1,0,0],[0,1,0])],
        'meshes':[{'primitives':[{'attributes':{'POSITION':0,'NORMAL':1},'indices':2+i,'material':i} for i in range(2)]}],
        'nodes':[{'mesh':0}],'scenes':[{'nodes':[0]}],'scene':0}
    model_path=out/'two-materials.gltf';model_path.write_text(json.dumps(model))
    scene=fixture(repo,True);obj=scene['objects'][2]['components'];obj.pop('material')
    obj['mesh']={'asset':'gltf','modelPath':str(model_path)}
    obj['transform']={'position':[0,0,0],'rotation':[0,0,0],'scale':[-1.1,.9,1.2]}
    (scenes/'multi.json').write_text(json.dumps(scene));ecs_gate.REPO,ecs_gate.OUT=repo,out
    logs,result=ecs_gate.run('multi-material',args.configuration,{'MIRABILIS_QT_EDITOR':'0','MIRABILIS_TEST_SCENE':'multi.json',
        'MIRABILIS_TEST_SCENE_DIRECTORY':str(scenes),'MIRABILIS_TEST_WINDOW':'800x600','MIRABILIS_TEST_CAMERA':'0 2.5 8 -0.6 0',
        'MIRABILIS_TEST_HIDE_PANELS':'1','MIRABILIS_TEST_FRAMES':'180','MIRABILIS_LUMEN_LITE':'1','MIRABILIS_TEST_REQUIRE_LUMEN':'1',
        'MIRABILIS_BENCHMARK_SKIP_CPU_BVH_VALIDATION':'1',
        'MIRABILIS_R4_UNCAP_FRAMES':'1','MIRABILIS_R4_CONTRIBUTIONS':'env_specular','MIRABILIS_RASTER_CAPTURE':str(out/'hdr'),
        'VK_INSTANCE_LAYERS':'VK_LAYER_KHRONOS_validation','VK_LAYER_VALIDATE_SYNC':'1','VK_KHRONOS_VALIDATION_VALIDATE_SYNC':'true'})
    assert 'Ray scene: 2 instances (3 material ranges)' in logs
    frame,_=read_pfm(out/'hdr.pfm');assert np.isfinite(frame).all()
    red=int(((frame[...,0]>.05)&(frame[...,1]<1e-5)).sum())
    green=int(((frame[...,1]>.05)&(frame[...,0]<1e-5)).sum())
    assert red>10 and green>10, 'Geometry indices lost one of the reflected materials'
    (out/'summary.json').write_text(json.dumps({**result,'redPixels':red,'greenPixels':green},indent=2))
    print('RAY GEOMETRY GPU GATE PASS',red,green,flush=True)


if __name__=='__main__':
    main()
