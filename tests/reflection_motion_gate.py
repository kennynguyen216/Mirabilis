"""Measure actual per-pixel reflection reuse under camera motion and invalidation."""
import argparse
import json
from pathlib import Path
import numpy as np
import ecs_gate
from lumen_gate import fixture,read_pfm


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--configuration',choices=['Debug','Release'],default='Debug')
    args=parser.parse_args();repo=Path(__file__).resolve().parents[1];out=args.output.resolve()
    assert not out.exists() or not any(out.iterdir()),'Use fresh evidence directory'
    scenes=out/'scenes';scenes.mkdir(parents=True)
    doc=fixture(repo,True);doc['objects'].append(fixture(repo)['objects'][-1])
    (scenes/'mirror.json').write_text(json.dumps(doc))
    ecs_gate.REPO,ecs_gate.OUT=repo,out
    common={'MIRABILIS_QT_EDITOR':'0','MIRABILIS_TEST_SCENE':'mirror.json',
        'MIRABILIS_TEST_SCENE_DIRECTORY':str(scenes),'MIRABILIS_TEST_WINDOW':'800x600',
        'MIRABILIS_TEST_CAMERA':'0 2.5 8 -0.6 0','MIRABILIS_TEST_HIDE_PANELS':'1',
        'MIRABILIS_TEST_FRAMES':'160','MIRABILIS_LUMEN_LITE':'1','MIRABILIS_LUMEN_HARDWARE':'1',
        'MIRABILIS_SSGI_SCREEN_TRACE':'0','MIRABILIS_TEST_REQUIRE_LUMEN':'1',
        'MIRABILIS_SURFACE_CACHE_ATLAS_SIZE':'1024','MIRABILIS_IQ5_CACHE_SKY_SAMPLES':'32',
        'MIRABILIS_R4_UNCAP_FRAMES':'1','MIRABILIS_R4_CONTRIBUTIONS':'env_specular',
        'VK_INSTANCE_LAYERS':'VK_LAYER_KHRONOS_validation','VK_LAYER_VALIDATE_SYNC':'1',
        'VK_KHRONOS_VALIDATION_VALIDATE_SYNC':'true'}
    cases={'static':{},'moving':{'MIRABILIS_TEST_REFLECTION_MOTION':'1'},
        'moving-furnace':{'MIRABILIS_TEST_REFLECTION_MOTION':'1','MIRABILIS_TEST_REFLECTION_FURNACE':'1'},
        'moving-furnace-reference':{'MIRABILIS_TEST_REFLECTION_MOTION':'1','MIRABILIS_TEST_REFLECTION_FURNACE':'1','MIRABILIS_REFLECTION_HISTORY':'0'},
        'cut':{'MIRABILIS_TEST_REFLECTION_CUT':'1'},
        'relighting':{'MIRABILIS_TEST_LIGHT_DRAG':'1','MIRABILIS_IQ13_SUN_ONLY_RELIGHT':'0','MIRABILIS_TEST_FRAMES':'120'}}
    results={};images={}
    for name,extra in cases.items():
        logs,result=ecs_gate.run(name,args.configuration,{**common,**extra,
            'MIRABILIS_TEST_REFLECTION_HISTORY':str(out/(name+'.json')),
            'MIRABILIS_RASTER_CAPTURE':str(out/name)})
        stats=json.loads((out/(name+'.json')).read_text());assert stats['ownedPixels']>100
        if name.startswith('moving'):assert stats['cameraPosition'][0]>.1 and stats['cameraPosition'][2]<7.5
        if name=='cut':assert stats['cameraPosition'][0]>2
        assert 1<=stats['maximumSamples']<=16 and stats['meanSamples']>=1
        if name in ['cut','relighting','moving-furnace-reference']:
            assert stats['reusedPixels']==0,(name,'Stale reflection history reused',stats)
        else:
            assert stats['reusedPixels']>stats['ownedPixels']*.05,(name,'History never survives',stats)
        if name=='relighting':assert 'reflection-history=false' in logs
        image,_=read_pfm(out/(name+'.pfm'));assert np.isfinite(image).all()
        images[name]=image;result['history']=stats;results[name]=result
        (out/'summary.json').write_text(json.dumps(results,indent=2))
    before=images['moving-furnace-reference'];delta=np.abs(images['moving-furnace']-before)
    # A uniform incident field has no changing reflected detail to justify temporal trails.
    relative=float(delta.mean()/max(float(np.abs(before).mean()),1e-5))
    assert relative<.01,('Furnace camera-motion bias',relative)
    results['furnaceComparison']={'meanRelativeError':relative,'maximumAbsoluteError':float(delta.max())}
    (out/'summary.json').write_text(json.dumps(results,indent=2))
    print('REFLECTION MOTION GPU GATE PASS',flush=True)


if __name__=='__main__':main()
