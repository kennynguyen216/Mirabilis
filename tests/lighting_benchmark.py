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


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--runs',type=int,default=3)
    parser.add_argument('--warmup',type=int,default=900)
    args=parser.parse_args();assert args.runs>=3 and args.warmup>=900
    repo=Path(__file__).resolve().parents[1];out=args.output.resolve()
    assert not out.exists() or not any(out.iterdir()),'Use fresh evidence directory'
    out.mkdir(parents=True);ecs_gate.OUT=out
    roots={'baseline':args.baseline.resolve(),'candidate':repo}
    profiles={'plain':{'MIRABILIS_LUMEN_LITE':'0'},
        'hardware':{'MIRABILIS_LUMEN_LITE':'1','MIRABILIS_LUMEN_HARDWARE':'1','MIRABILIS_SSGI_SCREEN_TRACE':'0'},
        'software':{'MIRABILIS_LUMEN_LITE':'1','MIRABILIS_LUMEN_HARDWARE':'0','MIRABILIS_SSGI_SCREEN_TRACE':'1'}}
    hashes={k:{'engine':ecs_gate.digest(p/'bin/Release/engine.exe'),'shaders':shader_hash(p)} for k,p in roots.items()}
    selected={k:ecs_gate.digest(p/'assets/scenes/.last_scene') for k,p in roots.items()}
    doc=repo/'assets/scenes/sponza_hq_preview.json'
    assert all(ecs_gate.digest(p/'assets/scenes'/doc.name)==ecs_gate.digest(doc) for p in roots.values())
    common={'MIRABILIS_QT_EDITOR':'0','MIRABILIS_TEST_SCENE':doc.name,'MIRABILIS_TEST_CAMERA':'0 2.5 0 -0.05 1.5708',
        'MIRABILIS_TEST_WINDOW':'1600x900','MIRABILIS_TEST_HIDE_PANELS':'1','MIRABILIS_SSGI_PRESET':'2',
        'MIRABILIS_SSGI_BENCHMARK':'1','MIRABILIS_R4_UNCAP_FRAMES':'1','MIRABILIS_BENCHMARK_SKIP_CPU_BVH_VALIDATION':'1',
        'MIRABILIS_SURFACE_CACHE_ATLAS_SIZE':'2048','MIRABILIS_IQ5_CACHE_SKY_SAMPLES':'32',
        'MIRABILIS_IQ_RT_SHADOWS':'1','MIRABILIS_SCENE_REFLECTIONS':'0','MIRABILIS_SSGI_PROBES':'0',
        'MIRABILIS_TEST_FRAMES':str(args.warmup+300),'MIRABILIS_BENCHMARK_WARMUP_FRAMES':str(args.warmup)}
    summary={'scene':doc.name,'sceneSha256':ecs_gate.digest(doc),'camera':common['MIRABILIS_TEST_CAMERA'],
        'window':[1600,900],'render':[1200,675],'gi':[600,338],'warmup':args.warmup,'samples':300,
        'hashes':hashes,'configuration':common,'profiles':{},'excludedRuns':[],
        'revision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),
        'workingTree':subprocess.check_output(['git','status','--short'],cwd=repo,text=True),
        'gpu':subprocess.check_output(['nvidia-smi','--query-gpu=name,driver_version,temperature.gpu','--format=csv'],text=True),
        'power':subprocess.check_output(['powercfg','/getactivescheme'],text=True),
        'notes':['Hardware uses direct-world default; software uses screen-first default. Compare before/after within each profile.',
                 'Cache sky is fixed at32 samples for both revisions. Both keep hardware sun. These are cached-GI profiles, not pure RT GI.',
                 'Visibility and card-coverage fixes can change images; FPS changes are not equal-image optimization claims.',
                 'Baseline lacks complete GPU scopes; candidate rows are keyed by completed submission, with final slots drained.']}
    def save():(out/'summary.json').write_text(json.dumps(summary,indent=2))
    save()
    for repeat in range(args.runs):
        modes=list(profiles);modes=modes[repeat%3:]+modes[:repeat%3]
        for mode in modes:
            for variant in (list(roots) if repeat%2==0 else list(reversed(roots))):
                ecs_gate.REPO=roots[variant];label=f'{mode}-{variant}-{repeat+1}'
                flags={**common,**profiles[mode],'MIRABILIS_R4_FRAME_TIME_LOG':str(out/(label+'.csv'))}
                if mode!='plain':flags['MIRABILIS_TEST_REQUIRE_LUMEN']='1'
                if variant=='candidate':flags['MIRABILIS_GPU_FRAME_LOG']=str(out/(label+'-gpu.csv'))
                if repeat==0:flags['MIRABILIS_RASTER_CAPTURE']=str(out/(label+'-hdr'))
                logs,result=ecs_gate.run(label,'Release',flags,seconds=900)
                result.update(parse_metrics(logs,300,mode!='plain',args.warmup))
                assert result['engineSha256']==hashes[variant]['engine']
                assert shader_hash(roots[variant])==hashes[variant]['shaders'],'Shaders changed during benchmark'
                if mode=='software':
                    fields=re.findall(r'backend=distance-field instances=(\d+) ready=(\d+) missing-meshes=(\d+)',logs)
                    assert fields and all(a==b and c=='0' for a,b,c in fields),'Incomplete software field'
                for key in ['trace','radiosity','exposure']:
                    match=re.search(r'average-'+key+r'-gpu-ms=([\d.]+)',logs)
                    if match:result[key+'GpuMs']=float(match[1])
                if variant=='candidate':
                    result['gpu']=read_gpu_metrics(out/(label+'-gpu.csv'),args.warmup,300)
                    result['gpu']['memoryBudgetSource']='driver' if 'device-local=true budget-extension=true' in logs else 'VMA estimate'
                emitters=re.findall(r'mode=raster-emitters triangles=(\d+)',logs)
                cells=re.findall(r'fullest cell (\d+) cards',logs)
                result['emitterTriangles']=max(map(int,emitters),default=0)
                result['lookupFullestCell']=max(map(int,cells),default=0)
                with (out/(label+'.csv')).open() as stream:
                    rows=[r for r in csv.DictReader(stream) if int(r['frame'])>args.warmup]
                assert len(rows)==300
                result.update(p95Ms=float(np.percentile([float(r['ms']) for r in rows],95)),
                    p99Ms=float(np.percentile([float(r['ms']) for r in rows],99)))
                summary['profiles'].setdefault(mode,{}).setdefault(variant,[]).append(result);save()
                print(label,result['frameMs'],result.get('gpu',{}).get('meanMs',{}),flush=True)
    summary['comparisons']={}
    for mode,runs in summary['profiles'].items():
        result=comparison([r['frameMs'] for r in runs['baseline']],[r['frameMs'] for r in runs['candidate']])
        before,_=read_pfm(out/(mode+'-baseline-1-hdr.pfm'));after,_=read_pfm(out/(mode+'-candidate-1-hdr.pfm'))
        assert np.isfinite(before).all() and np.isfinite(after).all();delta=np.abs(before-after)
        result['imageDifference']={'maxAbsolute':float(delta.max()),'p99Absolute':float(np.percentile(delta,99)),
            'meanRelative':float(delta.mean()/max(float(np.abs(before).mean()),1e-4))}
        summary['comparisons'][mode]=result
    assert all(ecs_gate.digest(p/'assets/scenes/.last_scene')==selected[k] for k,p in roots.items())
    save();print('LIGHTING SPONZA BENCHMARK COMPLETE',flush=True)

if __name__=='__main__':main()
