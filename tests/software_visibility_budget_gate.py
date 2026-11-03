"""A software visibility march must not treat an exhausted ray as unoccluded."""
import argparse
import copy
import json
from pathlib import Path
import numpy as np
import ecs_gate
from lumen_gate import fixture,read_pfm
from lighting_visibility_gate import bake_missing


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--configuration',choices=['Debug','Release'],default='Debug')
    args=parser.parse_args();repo=Path(__file__).resolve().parents[1];out=args.output.resolve()
    assert not out.exists() or not any(out.iterdir()),'Use fresh evidence directory'
    scenes=out/'scenes';scenes.mkdir(parents=True)
    opened=fixture(repo,True);opened['rendering']['sceneReflections']=False
    opened['objects'][1]['components']['transform'].update(position=[0,0,0],scale=[12,1,120])
    opened['objects'][2]['components']['transform'].update(position=[0,3,-50],scale=[6,6,.2])
    blocked=copy.deepcopy(opened)
    wall=copy.deepcopy(fixture(repo)['objects'][2]);wall['id']=3;wall['siblingIndex']=2
    wall['components']['transform'].update(position=[0,10,-25],scale=[64,20,.5])
    blocked['objects'].append(wall)
    dark=copy.deepcopy(opened);dark['objects'][2]['components']['material']['emissionStrength']=0
    for name,doc in [('open',opened),('blocked',blocked),('dark',dark)]:
        (scenes/(name+'.json')).write_text(json.dumps(doc))
    ecs_gate.REPO,ecs_gate.OUT=repo,out
    common={'MIRABILIS_QT_EDITOR':'0','MIRABILIS_TEST_SCENE_DIRECTORY':str(scenes),
        'MIRABILIS_TEST_WINDOW':'800x600','MIRABILIS_TEST_CAMERA':'0 3 50 -0.25 0',
        'MIRABILIS_TEST_HIDE_PANELS':'1','MIRABILIS_LUMEN_LITE':'1','MIRABILIS_LUMEN_HARDWARE':'0',
        'MIRABILIS_SURFACE_CACHE_ATLAS_SIZE':'1024','MIRABILIS_SURFACE_CACHE_RADIOSITY':'0',
        'MIRABILIS_SSGI_SCREEN_TRACE':'0','MIRABILIS_R4_CACHE_SOURCES':'emitter',
        'MIRABILIS_RENDER_DEBUG_VIEW':'29','MIRABILIS_SURFACE_CACHE_PAGE':'8',
        'MIRABILIS_R4_UNCAP_FRAMES':'1','MIRABILIS_BENCHMARK_SKIP_CPU_BVH_VALIDATION':'1',
        'VK_INSTANCE_LAYERS':'VK_LAYER_KHRONOS_validation','VK_LAYER_VALIDATE_SYNC':'1',
        'VK_KHRONOS_VALIDATION_VALIDATE_SYNC':'true'}
    preflight={**common,'MIRABILIS_TEST_SCENE':'blocked.json','MIRABILIS_TEST_FRAMES':'2'}
    logs,_=ecs_gate.run('preflight',args.configuration,preflight);bake_missing(repo,logs)
    results={}
    for name,scene,distance in [('open','open',8),('blocked-control','blocked',8),
        ('blocked-exhausted','blocked',2),('dark','dark',8)]:
        logs,result=ecs_gate.run(name,args.configuration,{**common,
            'MIRABILIS_TEST_SCENE':scene+'.json','MIRABILIS_SDF_MAX_DISTANCE_VOXELS':str(distance),
            'MIRABILIS_TEST_FRAMES':'360','MIRABILIS_TEST_REQUIRE_LUMEN':'1',
            'MIRABILIS_RASTER_CAPTURE':str(out/name)})
        assert 'pending-texels=0' in logs
        image,_=read_pfm(out/(name+'.pfm'));assert np.isfinite(image).all()
        result['receiverMean']=float(image[image.shape[0]//2:,:,1].mean());results[name]=result
        (out/'summary.json').write_text(json.dumps(results,indent=2))
    opened=results['open']['receiverMean'];assert opened>1e-5,('Open control is not lit',opened)
    for name in ['blocked-control','blocked-exhausted','dark']:
        assert results[name]['receiverMean']<opened*.05,(name,'Light leaked through occluder',results[name]['receiverMean'],opened)
    print('SOFTWARE VISIBILITY BUDGET GPU GATE PASS',flush=True)


if __name__=='__main__':main()
