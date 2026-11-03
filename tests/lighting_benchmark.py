"""Matched HQ Sponza+banners baseline/candidate runs with completed GPU attribution."""
import argparse
import csv
import json
from pathlib import Path
import re
import statistics
import subprocess
import hashlib
import numpy as np
import ecs_gate
from lumen_benchmark import parse_metrics,read_gpu_metrics
from benchmark_comparison import comparison
from lumen_gate import read_pfm


def shader_hash(repo):
    digest=hashlib.sha256()
    for path in sorted((repo/'shaders').glob('*.spv')):
        digest.update(path.name.encode());digest.update(path.read_bytes())
    return digest.hexdigest()


def image_difference(before,after):
    assert before.shape==after.shape and np.isfinite(before).all() and np.isfinite(after).all()
    delta=np.abs(before-after);scale=max(float(np.abs(before).mean()),1e-4)
    return {'maxAbsolute':float(delta.max()),'p99Absolute':float(np.percentile(delta,99)),
        'meanRelative':float(delta.mean()/scale),'signedMeanRelative':float((after-before).mean()/scale)}


def check_repeat_image(before,after,repeated):
    difference=image_difference(before,after);noise=image_difference(before,repeated)
    limits={'meanRelative':min(.005,max(3*noise['meanRelative'],1e-5)),
        'p99Absolute':max(3*noise['p99Absolute'],1e-6),'maxAbsolute':max(3*noise['maxAbsolute'],1e-4)}
    assert all(difference[key]<=limit for key,limit in limits.items()),(difference,noise,limits)
    return {**difference,'referenceRepeatDifference':noise,'acceptanceLimits':limits}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--runs',type=int,default=3)
    parser.add_argument('--warmup',type=int,default=900)
    parser.add_argument('--profiles',nargs='+',choices=['plain','hardware','software'],default=['plain','hardware','software'])
    parser.add_argument('--scene',choices=['sponza_hq_preview.json','sponza_hq_full_4k.json'],default='sponza_hq_preview.json')
    parser.add_argument('--texture-cache',choices=['on','off'],default='on')
    parser.add_argument('--reflections',action='store_true',help='Include scene reflections in both revisions')
    parser.add_argument('--screen-trace',choices=['policy','on','off'],default='policy',
        help='Use on/off for an equal-policy backend comparison; policy keeps each backend default')
    parser.add_argument('--probes',action='store_true',help='Use screen probes in both cached-GI profiles')
    parser.add_argument('--baseline-gpu-scopes',action='store_true',help='Baseline supports completed-submission GPU CSVs')
    parser.add_argument('--reference-controls',action='store_true',
        help='Same-binary baseline with mesh-box/exhaustive lookup and prepass rejection disabled')
    args=parser.parse_args();assert args.runs>=3 and args.warmup>=900
    repo=Path(__file__).resolve().parents[1];out=args.output.resolve()
    assert not out.exists() or not any(out.iterdir()),'Use fresh evidence directory'
    out.mkdir(parents=True);ecs_gate.OUT=out
    roots={'baseline':args.baseline.resolve(),'candidate':repo}
    profiles={'plain':{'MIRABILIS_LUMEN_LITE':'0'},
        'hardware':{'MIRABILIS_LUMEN_LITE':'1','MIRABILIS_LUMEN_HARDWARE':'1','MIRABILIS_SSGI_SCREEN_TRACE':'0'},
        'software':{'MIRABILIS_LUMEN_LITE':'1','MIRABILIS_LUMEN_HARDWARE':'0','MIRABILIS_SSGI_SCREEN_TRACE':'1'}}
    profiles={mode:profiles[mode] for mode in dict.fromkeys(args.profiles)}
    if args.screen_trace!='policy':
        for mode in profiles:
            if mode!='plain':profiles[mode]['MIRABILIS_SSGI_SCREEN_TRACE']='1' if args.screen_trace=='on' else '0'
    hashes={k:{'engine':ecs_gate.digest(p/'bin/Release/engine.exe'),'shaders':shader_hash(p)} for k,p in roots.items()}
    if args.reference_controls:assert hashes['baseline']==hashes['candidate'],'Reference controls require identical binaries/shaders'
    selected={k:ecs_gate.digest(p/'assets/scenes/.last_scene') for k,p in roots.items()}
    doc=repo/'assets/scenes'/args.scene
    assert all(ecs_gate.digest(p/'assets/scenes'/doc.name)==ecs_gate.digest(doc) for p in roots.values())
    common={'MIRABILIS_QT_EDITOR':'0','MIRABILIS_TEST_SCENE':doc.name,'MIRABILIS_TEST_CAMERA':'0 2.5 0 -0.05 1.5708',
        'MIRABILIS_TEST_WINDOW':'1600x900','MIRABILIS_TEST_HIDE_PANELS':'1','MIRABILIS_SSGI_PRESET':'2',
        'MIRABILIS_SSGI_BENCHMARK':'1','MIRABILIS_R4_UNCAP_FRAMES':'1','MIRABILIS_BENCHMARK_SKIP_CPU_BVH_VALIDATION':'1',
        'MIRABILIS_SURFACE_CACHE_ATLAS_SIZE':'2048','MIRABILIS_IQ5_CACHE_SKY_SAMPLES':'32',
        'MIRABILIS_IQ_RT_SHADOWS':'1','MIRABILIS_SCENE_REFLECTIONS':'1' if args.reflections else '0','MIRABILIS_SSGI_PROBES':'1' if args.probes else '0',
        'MIRABILIS_TEXTURE_CACHE':'1' if args.texture_cache=='on' else '0',
        'MIRABILIS_TEST_FRAMES':str(args.warmup+300),'MIRABILIS_BENCHMARK_WARMUP_FRAMES':str(args.warmup)}
    summary={'scene':doc.name,'sceneSha256':ecs_gate.digest(doc),'camera':common['MIRABILIS_TEST_CAMERA'],
        'window':[1600,900],'render':[1200,675],'gi':[600,338],'warmup':args.warmup,'samples':300,
        'hashes':hashes,'configuration':common,'profileConfiguration':profiles,
        'referenceControls':args.reference_controls,'textureCache':args.texture_cache,'profiles':{},'excludedRuns':[],
        'revision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),
        'workingTree':subprocess.check_output(['git','status','--short'],cwd=repo,text=True),
        'gpu':subprocess.check_output(['nvidia-smi','--query-gpu=name,driver_version,temperature.gpu','--format=csv'],text=True),
        'power':subprocess.check_output(['powercfg','/getactivescheme'],text=True),
        'notes':[f'Screen tracing policy={args.screen_trace}, screen probes={args.probes}. Compare before/after within each profile.',
                 'Cache sky is fixed at32 samples for both revisions. Both keep hardware sun. These are cached-GI profiles, not pure RT GI.',
                 'Visibility and card-coverage fixes can change images; FPS changes are not equal-image optimization claims.',
                 f'Completed-submission GPU CSV: candidate=yes, baseline={args.baseline_gpu_scopes or args.reference_controls}; final slots are drained.']}
    if args.reference_controls:
        summary['notes'].append('Extra reference repeats calibrate HDR variation and are excluded from paired FPS statistics; mean-relative image error is capped at 0.5%.')
    def save():(out/'summary.json').write_text(json.dumps(summary,indent=2))
    save()
    for repeat in range(args.runs):
        modes=list(profiles);rotation=repeat%len(modes);modes=modes[rotation:]+modes[:rotation]
        for mode in modes:
            for variant in (list(roots) if repeat%2==0 else list(reversed(roots))):
                ecs_gate.REPO=roots[variant];label=f'{mode}-{variant}-{repeat+1}'
                flags={**common,**profiles[mode],'MIRABILIS_R4_FRAME_TIME_LOG':str(out/(label+'.csv'))}
                if args.reference_controls:
                    flags.update(MIRABILIS_SURFACE_CACHE_LINEAR_LOOKUP='1' if variant=='baseline' else '0',
                        MIRABILIS_SURFACE_CACHE_BOX_LOOKUP='1' if variant=='baseline' else '0',
                        MIRABILIS_PREPASS_REJECT='0' if variant=='baseline' else '1')
                if mode!='plain':flags['MIRABILIS_TEST_REQUIRE_LUMEN']='1'
                gpu_csv=variant=='candidate' or args.baseline_gpu_scopes or args.reference_controls
                if gpu_csv:flags['MIRABILIS_GPU_FRAME_LOG']=str(out/(label+'-gpu.csv'))
                if repeat==0:
                    flags['MIRABILIS_RASTER_CAPTURE']=str(out/(label+'-hdr'))
                    flags['MIRABILIS_TEST_COLOR_CAPTURE']=str(out/(label+'-color'))
                logs,result=ecs_gate.run(label,'Release',flags,seconds=900)
                if args.reflections:assert 'reflections-active=true' in logs
                result.update(parse_metrics(logs,300,mode!='plain',args.warmup))
                assert re.search(r'preset=2 extent=600x338\b',logs),'Unexpected GI resolution/preset'
                assert result['engineSha256']==hashes[variant]['engine']
                assert shader_hash(roots[variant])==hashes[variant]['shaders'],'Shaders changed during benchmark'
                if mode=='software':
                    fields=re.findall(r'backend=distance-field instances=(\d+) ready=(\d+) missing-meshes=(\d+)',logs)
                    assert fields and all(a==b and c=='0' for a,b,c in fields),'Incomplete software field'
                for key in ['trace','radiosity','exposure']:
                    match=re.search(r'average-'+key+r'-gpu-ms=([\d.]+)',logs)
                    if match:result[key+'GpuMs']=float(match[1])
                if gpu_csv:
                    result['gpu']=read_gpu_metrics(out/(label+'-gpu.csv'),args.warmup,300)
                    result['gpu']['memoryBudgetSource']='driver' if 'device-local=true budget-extension=true' in logs else 'VMA estimate'
                emitters=re.findall(r'mode=raster-emitters triangles=(\d+)',logs)
                cells=re.findall(r'fullest cell (\d+) cards',logs)
                result['emitterTriangles']=max(map(int,emitters),default=0)
                result['lookupFullestCell']=max(map(int,cells),default=0)
                textures=re.findall(r'GLTF texture cache: BC7=(\d+) BC5=(\d+) RGBA=(\d+) compressed-payload-bytes=(\d+)',logs)
                result['textureUploads']={key:sum(int(row[i]) for row in textures)
                    for i,key in enumerate(['bc7','bc5','rgba','compressedBytes'])} if textures else None
                with (out/(label+'.csv')).open() as stream:
                    rows=[r for r in csv.DictReader(stream) if int(r['frame'])>args.warmup]
                assert len(rows)==300
                result.update(p95Ms=float(np.percentile([float(r['ms']) for r in rows],95)),
                    p99Ms=float(np.percentile([float(r['ms']) for r in rows],99)))
                summary['profiles'].setdefault(mode,{}).setdefault(variant,[]).append(result);save()
                print(label,result['frameMs'],result.get('gpu',{}).get('meanMs',{}),flush=True)
            if args.reference_controls and repeat==0:
                # Complex Sponza runs vary even without changing the executable or controls.
                ecs_gate.REPO=roots['baseline'];label=mode+'-reference-repeat'
                flags={**common,**profiles[mode],'MIRABILIS_SURFACE_CACHE_LINEAR_LOOKUP':'1',
                    'MIRABILIS_SURFACE_CACHE_BOX_LOOKUP':'1','MIRABILIS_PREPASS_REJECT':'0',
                    'MIRABILIS_RASTER_CAPTURE':str(out/(label+'-hdr'))}
                if mode!='plain':flags['MIRABILIS_TEST_REQUIRE_LUMEN']='1'
                logs,result=ecs_gate.run(label,'Release',flags,seconds=900)
                if args.reflections:assert 'reflections-active=true' in logs
                result.update(parse_metrics(logs,300,mode!='plain',args.warmup))
                assert result['engineSha256']==hashes['baseline']['engine']
                assert shader_hash(roots['baseline'])==hashes['baseline']['shaders']
                summary.setdefault('referenceImageRepeats',{})[mode]=result;save()
    summary['comparisons']={}
    for mode,runs in summary['profiles'].items():
        result=comparison([r['frameMs'] for r in runs['baseline']],[r['frameMs'] for r in runs['candidate']])
        before,_=read_pfm(out/(mode+'-baseline-1-hdr.pfm'));after,_=read_pfm(out/(mode+'-candidate-1-hdr.pfm'))
        assert before.shape==(675,1200,3) and after.shape==before.shape,'Unexpected rendered extent'
        result['imageDifference']=image_difference(before,after)
        if args.reference_controls:
            repeated,_=read_pfm(out/(mode+'-reference-repeat-hdr.pfm'))
            result['imageDifference']=check_repeat_image(before,after,repeated)
        summary['comparisons'][mode]=result
    assert all(ecs_gate.digest(p/'assets/scenes/.last_scene')==selected[k] for k,p in roots.items())
    save();print('LIGHTING SPONZA BENCHMARK COMPLETE',flush=True)

if __name__=='__main__':main()
