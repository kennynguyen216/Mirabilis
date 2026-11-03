"""Focused, uncapped GI timings; reject fallback, incomplete warm-up and shader changes."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import statistics
import subprocess

import numpy as np
import ecs_gate


def light_edit_scene(doc):
    if doc['version'] == 1:
        # The living-room benchmark is still authored in the legacy format.
        # Keep its geometry/visibility; punctual lights require version 2.
        for obj in doc['objects']:
            assert not obj.get('materialEnabled'), 'Use a version-2 scene for authored material edits'
            obj['components'] = {
                'name': {'value': obj['name']},
                'transform': {key: obj[key] for key in ('position','rotation','scale')},
                'mesh': {key: obj[key] for key in ('asset','modelPath')},
                'visibility': {'visible': obj['visible'], 'layer': obj['layer']}}
            obj['parent'] = None if obj['parent'] < 0 else obj['parent']
            obj['siblingIndex'] = obj['id']
        doc['version'] = 2
    ident = max(doc.get('nextPersistentId',0),max(obj['id'] for obj in doc['objects'])+1)
    doc['objects'].append({'id':ident,'parent':0,'siblingIndex':1000,'components':{
        'name':{'value':'Benchmark punctual source'},
        'transform':{'position':[3,2,0],'rotation':[0,0,0],'scale':[1,1,1]},
        'light':{'type':'Point','intensity':20,'range':12,'color':[1,.6,.3],'castShadows':True}}})
    doc['nextPersistentId']=ident+1
    return doc


def parse_metrics(logs, samples, lumen, warmup):
    match = re.search(r'average-frame-ms=([\d.]+).*average-ssgi-gpu-ms=([\d.]+).*samples=(\d+)', logs)
    assert match and int(match[3]) == samples, 'Missing or incorrect sample count'
    assert f'GI benchmark focused samples: {samples}/{samples}' in logs, 'Foreground focus lost'
    if lumen:
        assert 'GI effective mode=lumen-lite' in logs, 'Requested mode fell back'
        assert 'pending-texels=0' in logs, 'Cache did not converge'
        first = re.search(r'Surface cache lit: .*? over (\d+) frames', logs)
        assert first and int(first[1]) < warmup, 'Increase warm-up; initial cache sweep overlaps measurement'
    frame_ms = float(match[1])
    assert np.isfinite(frame_ms) and frame_ms > 0
    return {'frameMs': frame_ms, 'fps': 1000/frame_ms, 'giGpuMs': float(match[2])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--scene', default='living_room_showcase.json')
    parser.add_argument('--camera', default='4.740 1.700 -2.947 -0.133 -14.670')
    parser.add_argument('--warmup', type=int, default=300)
    parser.add_argument('--atlas', type=int, choices=[512,1024,2048,4096], default=4096)
    parser.add_argument('--hitches', action='store_true')
    parser.add_argument('--compare-backends', action='store_true',
                        help='Compare screen-first software/hardware GI with equal 32-sample cache sky budgets')
    parser.add_argument('--profiles', nargs='+', choices=('plain','lumen','probes','reflections'),
                        default=('plain','lumen','probes','reflections'))
    args = parser.parse_args()
    assert args.warmup >= 60
    repo, out = Path(__file__).resolve().parents[1], args.output.resolve()
    assert not out.exists() or not any(out.iterdir()), 'Use fresh evidence directory'
    out.mkdir(parents=True, exist_ok=True)
    ecs_gate.REPO, ecs_gate.OUT = repo, out

    def shader_hash():
        digest = hashlib.sha256()
        for path in sorted((repo/'shaders').glob('*.spv')):
            digest.update(path.name.encode()); digest.update(path.read_bytes())
        return digest.hexdigest()

    profiles = {'plain': {'MIRABILIS_LUMEN_LITE':'0'}, 'lumen': {'MIRABILIS_LUMEN_LITE':'1'},
                'probes': {'MIRABILIS_LUMEN_LITE':'1','MIRABILIS_SSGI_PROBES':'1'},
                'reflections': {'MIRABILIS_LUMEN_LITE':'1','MIRABILIS_SCENE_REFLECTIONS':'1'}}
    profiles={name:profiles[name] for name in args.profiles}
    measured, warmup, scene = 300, args.warmup, args.scene
    common = {'MIRABILIS_QT_EDITOR':'0','MIRABILIS_TEST_WINDOW':'1600x900','MIRABILIS_TEST_HIDE_PANELS':'1',
              'MIRABILIS_TEST_CAMERA':args.camera,'MIRABILIS_SSGI_PRESET':'2','MIRABILIS_SSGI_BENCHMARK':'1',
              'MIRABILIS_R4_UNCAP_FRAMES':'1','MIRABILIS_BENCHMARK_SKIP_CPU_BVH_VALIDATION':'1',
              'MIRABILIS_SURFACE_CACHE_ATLAS_SIZE':str(args.atlas)}
    if args.hitches:
        doc = light_edit_scene(json.loads((repo/'assets/scenes'/args.scene).read_text()))
        scenes=out/'scenes';scenes.mkdir();scene='living_edit.json'
        (scenes/scene).write_text(json.dumps(doc))
        common.update(MIRABILIS_TEST_SCENE_DIRECTORY=str(scenes),MIRABILIS_TEST_LIGHT_DRAG='1',MIRABILIS_LUMEN_LITE='1')
        profiles={'full-blocking-control':{'MIRABILIS_TEST_BLOCKING_RELIGHT':'1','MIRABILIS_IQ13_SUN_ONLY_RELIGHT':'0'},
                  'budgeted-source-reuse':{'MIRABILIS_IQ13_SUN_ONLY_RELIGHT':'1'}}
        measured,warmup=180,60
    if args.compare_backends:
        assert not args.hitches, 'Backend and relight comparisons are separate workloads'
        profiles={'software':{'MIRABILIS_LUMEN_LITE':'1','MIRABILIS_LUMEN_HARDWARE':'0'},
                  'hardware':{'MIRABILIS_LUMEN_LITE':'1','MIRABILIS_LUMEN_HARDWARE':'1'}}
        common.update(MIRABILIS_SSGI_SCREEN_TRACE='1',MIRABILIS_IQ_RT_SHADOWS='1',
                      MIRABILIS_IQ5_CACHE_SKY_SAMPLES='32',MIRABILIS_SCENE_REFLECTIONS='0')
    initial_hash=shader_hash()
    initial_engine=ecs_gate.digest(repo/'bin/Release/engine.exe')
    summary={'scene':scene,'camera':args.camera,'window':[1600,900],'render':[1200,675],
             'gi':[600,338],'atlasLimit':args.atlas,'warmup':warmup,'samples':measured,
             'revision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),
             'workingTree':subprocess.check_output(['git','status','--short'],cwd=repo,text=True),
             'engineSha256':initial_engine,'backendComparison':args.compare_backends,
             'shaderSha256':initial_hash,'gpu':subprocess.check_output(['nvidia-smi','--query-gpu=name,driver_version,temperature.gpu','--format=csv'],text=True),
             'power':subprocess.check_output(['powercfg','/GetActiveScheme'],text=True),'profiles':{}}
    for repeat in range(3):
        order=list(profiles)
        if repeat%2:order.reverse()
        for mode in order:
            label=f'{mode}-{repeat+1}'
            flags={**common,'MIRABILIS_TEST_SCENE':scene,'MIRABILIS_TEST_FRAMES':str(warmup+measured),
                   'MIRABILIS_BENCHMARK_WARMUP_FRAMES':str(warmup),
                   'MIRABILIS_R4_FRAME_TIME_LOG':str(out/(label+'.csv')),**profiles[mode]}
            lumen=flags.get('MIRABILIS_LUMEN_LITE')=='1'
            if lumen:flags['MIRABILIS_TEST_REQUIRE_LUMEN']='1'
            if args.compare_backends and repeat==0:flags['MIRABILIS_RASTER_CAPTURE']=str(out/(mode+'-hdr'))
            logs,result=ecs_gate.run(label,'Release',flags,seconds=900)
            result.update(parse_metrics(logs,measured,lumen,warmup))
            assert result['engineSha256']==initial_engine, 'Executable changed during measurement'
            if args.compare_backends:
                backend='software-distance-field' if mode=='software' else 'hardware'
                assert 'GI tracing configuration: backend='+backend+' screen-first=true' in logs
                if mode=='software':
                    placed=re.findall(r'GI world backend=distance-field instances=(\d+) ready=(\d+) missing-meshes=(\d+)',logs)
                    assert placed and all(total==ready and missing=='0' for total,ready,missing in placed)
            with (out/(label+'.csv')).open() as file:
                rows=list(csv.DictReader(file))
            frames=[float(row['ms']) for row in rows if int(row['frame'])>warmup]
            result.update(p95Ms=float(np.percentile(frames,95)),p99Ms=float(np.percentile(frames,99)))
            if args.hitches:
                edit=[float(row['ms']) for row in rows if 95<=int(row['frame'])<180]
                result.update(editP95Ms=float(np.percentile(edit,95)),editMaxMs=max(edit))
            profile=summary['profiles'].setdefault(mode,{'runs':[]});profile['runs'].append(result)
            profile.update(medianFps=statistics.median(run['fps'] for run in profile['runs']),
                           fpsRange=[min(run['fps'] for run in profile['runs']),max(run['fps'] for run in profile['runs'])])
            if args.hitches:profile['medianEditP95Ms']=statistics.median(run['editP95Ms'] for run in profile['runs'])
            (out/'summary.json').write_text(json.dumps(summary,indent=2))
            print(label,result,flush=True)
    assert initial_hash==shader_hash(),'Shaders changed during measurement'
    if args.compare_backends:
        from lumen_gate import read_pfm
        software,_=read_pfm(out/'software-hdr.pfm');hardware,_=read_pfm(out/'hardware-hdr.pfm')
        assert np.isfinite(software).all() and np.isfinite(hardware).all()
        difference=np.abs(software-hardware)
        summary['captureDifferences']={'maxAbsolute':float(difference.max()),
            'p99Absolute':float(np.percentile(difference,99)),
            'meanRelative':float(difference.mean()/max(float(np.abs(hardware).mean()),1e-4))}
        summary['backendSettings']={'screenFirst':True,'cacheSkySamples':32,'hardwareSun':True}
        (out/'summary.json').write_text(json.dumps(summary,indent=2))
    print('LUMEN BENCHMARK COMPLETE',flush=True)


if __name__ == '__main__':
    main()
