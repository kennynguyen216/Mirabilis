"""Measure HQ Sponza under continuous body motion and punctual-light edits."""
import argparse
import csv
import json
from pathlib import Path
import re
import statistics
import subprocess
import numpy as np
import ecs_gate
from lighting_benchmark import shader_hash
from lumen_benchmark import light_edit_scene,parse_metrics,read_gpu_metrics
from lumen_gate import read_pfm


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--scene',choices=['sponza_hq_preview.json','sponza_hq_full_4k.json'],default='sponza_hq_full_4k.json')
    parser.add_argument('--runs',type=int,default=3)
    args=parser.parse_args();assert args.runs>=3
    repo=Path(__file__).resolve().parents[1];out=args.output.resolve()
    assert not out.exists() or not any(out.iterdir()),'Use a fresh evidence directory'
    scenes=out/'scenes';scenes.mkdir(parents=True)
    source=repo/'assets/scenes'/args.scene
    doc=light_edit_scene(json.loads(source.read_text()))
    light=doc['objects'][-1]['components']
    light['transform']['position']=[-3,3,0]
    light['light'].update(intensity=40,range=12)
    ident=doc['nextPersistentId'];doc['nextPersistentId']=ident+1
    doc['objects'].append({'id':ident,'parent':0,'siblingIndex':1001,'components':{
        'name':{'value':'Benchmark moving opaque body'},
        'transform':{'position':[-3,1.5,0],'rotation':[0,0,0],'scale':[.75,.75,.75]},
        'mesh':{'asset':'cube','modelPath':''},
        'material':{'enabled':True,'tint':[.1,.7,.15,1],'roughness':.7,'metallic':0},
        'visibility':{'visible':True,'layer':0},
        'collider':{'enabled':True,'shape':0,'center':[0,0,0],'halfExtents':[.5,.5,.5]},
        'rigidBody':{'motion':'Dynamic','mass':1,'friction':0,'restitution':0,'gravityFactor':0}}})
    (scenes/'dynamic-sponza.json').write_text(json.dumps(doc,indent=2))
    ecs_gate.REPO,ecs_gate.OUT=repo,out
    engine=ecs_gate.digest(repo/'bin/Release/engine.exe');shaders=shader_hash(repo)
    common={'MIRABILIS_QT_EDITOR':'0','MIRABILIS_TEST_SCENE':'dynamic-sponza.json',
        'MIRABILIS_TEST_SCENE_DIRECTORY':str(scenes),'MIRABILIS_TEST_CAMERA':'0 2.5 0 -0.05 1.5708',
        'MIRABILIS_TEST_WINDOW':'1600x900','MIRABILIS_TEST_HIDE_PANELS':'1','MIRABILIS_SSGI_PRESET':'2',
        'MIRABILIS_SSGI_BENCHMARK':'1','MIRABILIS_R4_UNCAP_FRAMES':'1',
        'MIRABILIS_BENCHMARK_SKIP_CPU_BVH_VALIDATION':'1','MIRABILIS_SURFACE_CACHE_ATLAS_SIZE':'2048',
        'MIRABILIS_IQ5_CACHE_SKY_SAMPLES':'32','MIRABILIS_IQ_RT_SHADOWS':'1','MIRABILIS_SCENE_REFLECTIONS':'1',
        'MIRABILIS_SSGI_PROBES':'0','MIRABILIS_TEXTURE_CACHE':'1','MIRABILIS_TEST_FRAMES':'1200',
        'MIRABILIS_BENCHMARK_WARMUP_FRAMES':'900','MIRABILIS_LUMEN_LITE':'1','MIRABILIS_TEST_REQUIRE_LUMEN':'1',
        'MIRABILIS_TEST_DYNAMIC_GI':'1','MIRABILIS_TEST_DYNAMIC_GI_OSCILLATE':'1',
        'MIRABILIS_TEST_LIGHT_DRAG':'1','MIRABILIS_TEST_LIGHT_DRAG_CONTINUOUS':'1'}
    summary={'revision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),
        'sourceSceneSha256':ecs_gate.digest(source),'engineSha256':engine,'shaderSha256':shaders,
        'window':[1600,900],'render':[1200,675],'gi':[600,338],'warmup':900,'samples':300,
        'workload':'One oscillating opaque physics body and one continuously edited punctual source; scene reflections enabled',
        'environment':common,'profiles':{}}
    def save():(out/'summary.json').write_text(json.dumps(summary,indent=2))
    save()
    for repeat in range(args.runs):
        for mode in (['hardware','software'] if repeat%2==0 else ['software','hardware']):
            name=f'{mode}-{repeat+1}';flags={**common,
                'MIRABILIS_LUMEN_HARDWARE':'1' if mode=='hardware' else '0',
                'MIRABILIS_SSGI_SCREEN_TRACE':'0' if mode=='hardware' else '1',
                'MIRABILIS_GPU_FRAME_LOG':str(out/(name+'-gpu.csv')),
                'MIRABILIS_R4_FRAME_TIME_LOG':str(out/(name+'-frame.csv'))}
            if repeat==0:
                flags['MIRABILIS_RASTER_CAPTURE']=str(out/(name+'-hdr'))
                flags['MIRABILIS_TEST_COLOR_CAPTURE']=str(out/(name+'-color'))
            logs,result=ecs_gate.run(name,'Release',flags,seconds=1200)
            assert 'GI effective mode=lumen-lite' in logs and 'reflections-active=true' in logs
            assert result['engineSha256']==engine and shader_hash(repo)==shaders,'Binary changed during benchmark'
            assert re.search(r'preset=2 extent=600x338\b',logs)
            # Continuous changes intentionally keep the relight queue active.
            result.update(parse_metrics(logs,300,False,900))
            result['gpu']=read_gpu_metrics(out/(name+'-gpu.csv'),900,300,allow_rebuilds=True)
            rebuilds=result['gpu']['rebuilds']
            assert all(rebuilds[key]==0 for key in ['sdf_rebuilds','card_rebuilds','trace_rebuilds','albedo_rebuilds','blas_builds']),rebuilds
            assert rebuilds['tlas_builds']>=290,'Body did not keep moving during measurement'
            if mode=='software':
                updates=re.findall(r'Dynamic GI field: updates=(\d+) instances=(\d+) missing=(\d+)',logs)
                assert updates and int(updates[-1][0])>=1190 and all(int(n)>0 and m=='0' for _,n,m in updates)
                assert result['gpu']['meanMs']['gpu_dynamic_field_ms']>0
            else:assert result['gpu']['meanMs']['gpu_dynamic_field_ms']<.1
            with (out/(name+'-frame.csv')).open() as stream:
                frames=[float(r['ms']) for r in csv.DictReader(stream) if int(r['frame'])>900]
            assert len(frames)==300
            result.update(p95Ms=float(np.percentile(frames,95)),p99Ms=float(np.percentile(frames,99)))
            if repeat==0:
                image,_=read_pfm(out/(name+'-hdr.pfm'));assert image.shape==(675,1200,3) and np.isfinite(image).all()
            summary['profiles'].setdefault(mode,[]).append(result);save()
            print(name,result['frameMs'],result['gpu'],flush=True)
    summary['medians']={mode:{'frameMs':statistics.median(r['frameMs'] for r in runs),
        'fps':1000/statistics.median(r['frameMs'] for r in runs),
        'p95Ms':statistics.median(r['p95Ms'] for r in runs),'p99Ms':statistics.median(r['p99Ms'] for r in runs)}
        for mode,runs in summary['profiles'].items()}
    save();print('DYNAMIC SPONZA BENCHMARK COMPLETE',flush=True)


if __name__=='__main__':main()
