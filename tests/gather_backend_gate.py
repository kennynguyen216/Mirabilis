"""Compare specialized GI gathers against their generic shader control on Vulkan."""
import argparse
import json
from pathlib import Path
import numpy as np
import ecs_gate
from lighting_visibility_gate import bake_missing
from lumen_gate import fixture,read_pfm


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--configuration',choices=['Debug','Release'],default='Debug')
    args=parser.parse_args();repo=Path(__file__).resolve().parents[1];out=args.output.resolve()
    assert not out.exists() or not any(out.iterdir()),'Use a fresh evidence directory'
    scenes=out/'scenes';scenes.mkdir(parents=True)
    (scenes/'point.json').write_text(json.dumps(fixture(repo)))
    ecs_gate.REPO,ecs_gate.OUT=repo,out
    common={'MIRABILIS_QT_EDITOR':'0','MIRABILIS_TEST_SCENE_DIRECTORY':str(scenes),
        'MIRABILIS_TEST_SCENE':'point.json','MIRABILIS_TEST_WINDOW':'640x480',
        'MIRABILIS_TEST_CAMERA':'0 3.5 9 -0.3 0','MIRABILIS_TEST_HIDE_PANELS':'1',
        'MIRABILIS_LUMEN_LITE':'1','MIRABILIS_R4_UNCAP_FRAMES':'1',
        'MIRABILIS_IQ_RT_SHADOWS':'1',
        'MIRABILIS_SURFACE_CACHE_ATLAS_SIZE':'1024','MIRABILIS_IQ5_CACHE_SKY_SAMPLES':'32',
        'MIRABILIS_AUTO_EXPOSURE':'0','MIRABILIS_BENCHMARK_SKIP_CPU_BVH_VALIDATION':'1',
        'VK_INSTANCE_LAYERS':'VK_LAYER_KHRONOS_validation','VK_LAYER_VALIDATE_SYNC':'1',
        'VK_KHRONOS_VALIDATION_VALIDATE_SYNC':'true'}
    logs,_=ecs_gate.run('preflight',args.configuration,{**common,'MIRABILIS_LUMEN_HARDWARE':'0','MIRABILIS_TEST_FRAMES':'3'})
    bake_missing(repo,logs)
    summary={}
    for profile,hardware,screen,probes in [('software-world',False,False,False),
            ('software-screen',False,True,False),('hardware-world',True,False,False),
            ('hardware-probes',True,True,True)]:
        images={};results={}
        for generic in [True,False]:
            variant='generic' if generic else 'specialized';name=profile+'-'+variant
            flags={**common,'MIRABILIS_LUMEN_HARDWARE':str(int(hardware)),
                'MIRABILIS_SSGI_SCREEN_TRACE':str(int(screen)),'MIRABILIS_SSGI_PROBES':str(int(probes)),
                'MIRABILIS_TEST_REQUIRE_LUMEN':'1','MIRABILIS_TEST_FRAMES':'240',
                'MIRABILIS_RASTER_CAPTURE':str(out/name)}
            if generic:flags['MIRABILIS_GI_GENERIC_WORLD']='1'
            logs,result=ecs_gate.run(name,args.configuration,flags)
            expected='generic' if generic else 'hardware' if hardware else 'software-static'
            assert 'GI gather pipeline='+expected in logs,'Requested gather pipeline not exercised'
            images[variant]=read_pfm(out/(name+'.pfm'))[0];results[variant]=result
        before,after=images['generic'],images['specialized']
        assert before.shape==after.shape and np.isfinite(before).all() and np.isfinite(after).all()
        delta=np.abs(before-after)
        results['difference']={'maxAbsolute':float(delta.max()),'meanAbsolute':float(delta.mean())}
        summary[profile]=results;(out/'summary.json').write_text(json.dumps(summary,indent=2))
        assert np.array_equal(before,after),(profile,results['difference'])
    print('GATHER BACKEND GPU GATE PASS',flush=True)


if __name__=='__main__':main()
