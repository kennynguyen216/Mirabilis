"""Compare accelerated cache lookup with its exhaustive shader reference."""
import argparse
import copy
import json
from pathlib import Path
import re
import numpy as np
import ecs_gate
from lumen_gate import fixture, read_pfm
from lighting_visibility_gate import bake_missing


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--configuration',choices=['Debug','Release'],default='Debug')
    args=parser.parse_args();repo=Path(__file__).resolve().parents[1];out=args.output.resolve()
    assert not out.exists() or not any(out.iterdir()),'Use fresh evidence directory'
    scenes=out/'scenes';scenes.mkdir(parents=True)
    variants={'point':fixture(repo),'mirror':fixture(repo,True)}
    rotated=copy.deepcopy(variants['point'])
    rotated['objects'][2]['components']['transform'].update(rotation=[13,37,8],scale=[-7,4,.3])
    variants['rotated']=rotated
    overflow=copy.deepcopy(rotated)
    for index in range(20):
        shell=copy.deepcopy(rotated['objects'][2]);shell['id']=index+20;shell['siblingIndex']=index+4
        shell['components']['transform'].update(position=[0,0,0],rotation=[0,0,0],scale=[40+index*.1]*3)
        overflow['objects'].append(shell)
    overflow['nextPersistentId']=100
    variants['overflow']=overflow
    for name,doc in variants.items(): (scenes/(name+'.json')).write_text(json.dumps(doc))
    ecs_gate.REPO,ecs_gate.OUT=repo,out
    common={'MIRABILIS_QT_EDITOR':'0','MIRABILIS_TEST_SCENE_DIRECTORY':str(scenes),
        'MIRABILIS_TEST_WINDOW':'800x600','MIRABILIS_TEST_CAMERA':'0 3.5 9 -0.3 0',
        'MIRABILIS_TEST_HIDE_PANELS':'1','MIRABILIS_LUMEN_LITE':'1',
        'MIRABILIS_SURFACE_CACHE_ATLAS_SIZE':'1024','MIRABILIS_IQ5_CACHE_SKY_SAMPLES':'32',
        'MIRABILIS_R4_UNCAP_FRAMES':'1','MIRABILIS_BENCHMARK_SKIP_CPU_BVH_VALIDATION':'1',
        'VK_INSTANCE_LAYERS':'VK_LAYER_KHRONOS_validation','VK_LAYER_VALIDATE_SYNC':'1',
        'VK_KHRONOS_VALIDATION_VALIDATE_SYNC':'true'}
    results={}
    for backend in ['software','hardware']:
        for scene in variants:
            flags={**common,'MIRABILIS_TEST_SCENE':scene+'.json',
                'MIRABILIS_LUMEN_HARDWARE':'1' if backend=='hardware' else '0',
                'MIRABILIS_SSGI_SCREEN_TRACE':'1','MIRABILIS_TEST_FRAMES':'360',
                'MIRABILIS_TEST_REQUIRE_LUMEN':'1'}
            if scene=='mirror':
                flags.update(MIRABILIS_SCENE_REFLECTIONS='1',MIRABILIS_TEST_CAMERA='0 2.5 8 -0.6 0')
            if backend=='software':
                preflight={**flags,'MIRABILIS_TEST_FRAMES':'2'}
                preflight.pop('MIRABILIS_TEST_REQUIRE_LUMEN')
                logs,_=ecs_gate.run(scene+'-preflight',args.configuration,
                    preflight)
                bake_missing(repo,logs)
            captures=[]
            for linear in [True,False]:
                label=scene+'-'+backend+('-linear' if linear else '-grouped')
                logs,result=ecs_gate.run(label,args.configuration,{**flags,
                    'MIRABILIS_SURFACE_CACHE_LINEAR_LOOKUP':'1' if linear else '0',
                    'MIRABILIS_SURFACE_CACHE_BOX_LOOKUP':'1' if linear else '0',
                    'MIRABILIS_PREPASS_REJECT':'0' if linear else '1',
                    'MIRABILIS_RASTER_CAPTURE':str(out/label)})
                assert 'GI effective mode=lumen-lite' in logs
                assert ('linear reference' if linear else 'facing groups: enabled') in logs
                assert ('Surface cache occupancy index: '+('box reference' if linear else 'enabled')) in logs
                assert ('Forward prepass rejection: '+str(not linear).lower()) in logs
                assert 'pending-texels=0' in logs,'Cache did not converge'
                if backend=='software':
                    fields=re.findall(r'backend=distance-field instances=(\d+) ready=(\d+) missing-meshes=(\d+)',logs)
                    assert fields and all(a==b and c=='0' for a,b,c in fields)
                if scene=='overflow' and linear:
                    assert max(map(int,re.findall(r'fullest cell (\d+) cards',logs)))>64
                image,_=read_pfm(out/(label+'.pfm'));assert np.isfinite(image).all()
                captures.append(image);results[label]=result
            delta=np.abs(captures[0]-captures[1])
            assert delta.max()==0,(scene,backend,'Acceleration changed HDR output',float(delta.max()))
            results[scene+'-'+backend+'-comparison']={'maximumAbsoluteError':float(delta.max())}
            (out/'summary.json').write_text(json.dumps(results,indent=2))
    print('CACHE LOOKUP GPU GATE PASS',flush=True)


if __name__=='__main__':main()
