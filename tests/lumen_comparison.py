"""Alternate frozen/current binaries and shader packs with identical GI settings and HDR checks."""
import argparse
import csv
import json
from pathlib import Path
import shutil
import subprocess

import numpy as np
import ecs_gate
from lumen_benchmark import parse_metrics
from benchmark_comparison import comparison
from lumen_gate import read_pfm


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--baseline-engine',required=True)
    parser.add_argument('--baseline-shaders',type=Path,required=True)
    parser.add_argument('--scene',default='sponza_hq_preview.json')
    parser.add_argument('--camera',default='0 2.5 0 -0.05 1.5708')
    parser.add_argument('--warmup',type=int,default=900)
    parser.add_argument('--atlas',type=int,choices=(512,1024,2048,4096),default=2048)
    parser.add_argument('--profiles',nargs='+',choices=('plain','lumen','reflections'),default=('plain','lumen'))
    args=parser.parse_args()
    assert args.warmup>=60
    repo,out=Path(__file__).resolve().parents[1],args.output.resolve()
    assert not out.exists() or not any(out.iterdir()), 'Use fresh evidence directory'
    out.mkdir(parents=True);ecs_gate.REPO,ecs_gate.OUT=repo,out
    candidate=out/'candidate-shaders';candidate.mkdir()
    for path in (repo/'shaders').glob('*.spv'):shutil.copy2(path,candidate/path.name)
    packs={'baseline':args.baseline_shaders.resolve(),'candidate':candidate}
    manifests={name:{path.name:ecs_gate.digest(path) for path in sorted(pack.glob('*.spv'))} for name,pack in packs.items()}
    assert manifests['baseline'].keys()==manifests['candidate'].keys(), 'Shader pack is incomplete'
    executable=repo/'bin/Release/engine.exe'
    shutil.copy2(executable,out/'candidate-engine.exe')
    engines={'baseline':repo/'bin/Release'/args.baseline_engine,'candidate':out/'candidate-engine.exe'}
    hashes={name:ecs_gate.digest(exe) for name,exe in engines.items()}
    authored=ecs_gate.digest(repo/'assets/scenes'/args.scene)
    summary={'scene':args.scene,'camera':args.camera,'warmup':args.warmup,'samples':300,'atlasLimit':args.atlas,
        'revision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),
        'workingTreeStatus':subprocess.check_output(['git','status','--short'],cwd=repo,text=True),
        'engineSha256':hashes,'shaderSha256':manifests,'profiles':{},'comparisons':{},'captureDifferences':{},
        'gpu':subprocess.check_output(['nvidia-smi','--query-gpu=name,driver_version,temperature.gpu','--format=csv'],text=True),
        'power':subprocess.check_output(['powercfg','/GetActiveScheme'],text=True)}

    def install(name):
        for path in packs[name].glob('*.spv'):shutil.copy2(path,repo/'shaders'/path.name)
        shutil.copy2(engines[name],executable)

    try:
        for repeat in range(3):
            for mode in args.profiles:
                for variant in (('baseline','candidate') if repeat%2==0 else ('candidate','baseline')):
                    install(variant);label=f'{mode}-{variant}-{repeat+1}'
                    flags={'MIRABILIS_QT_EDITOR':'0','MIRABILIS_TEST_SCENE':args.scene,'MIRABILIS_TEST_CAMERA':args.camera,
                        'MIRABILIS_TEST_WINDOW':'1600x900','MIRABILIS_TEST_HIDE_PANELS':'1','MIRABILIS_SSGI_PRESET':'2',
                        'MIRABILIS_SSGI_BENCHMARK':'1','MIRABILIS_R4_UNCAP_FRAMES':'1',
                        'MIRABILIS_LUMEN_LITE':'0' if mode=='plain' else '1',
                        'MIRABILIS_SCENE_REFLECTIONS':'1' if mode=='reflections' else '0',
                        'MIRABILIS_SURFACE_CACHE_ATLAS_SIZE':str(args.atlas),'MIRABILIS_TEST_FRAMES':str(args.warmup+300),
                        'MIRABILIS_BENCHMARK_WARMUP_FRAMES':str(args.warmup),'MIRABILIS_BENCHMARK_SKIP_CPU_BVH_VALIDATION':'1',
                        'MIRABILIS_R4_FRAME_TIME_LOG':str(out/(label+'.csv'))}
                    if mode!='plain':flags['MIRABILIS_TEST_REQUIRE_LUMEN']='1'
                    if repeat==0:flags['MIRABILIS_RASTER_CAPTURE']=str(out/f'{mode}-{variant}-hdr')
                    logs,result=ecs_gate.run(label,'Release',flags,seconds=900)
                    result.update(parse_metrics(logs,300,mode!='plain',args.warmup))
                    assert result['engineSha256']==hashes[variant], 'Binary changed during comparison'
                    assert {p.name:ecs_gate.digest(p) for p in (repo/'shaders').glob('*.spv')}==manifests[variant], 'Shaders changed during run'
                    with (out/(label+'.csv')).open() as file:
                        frames=[float(row['ms']) for row in csv.DictReader(file) if int(row['frame'])>args.warmup]
                    assert len(frames)==300
                    result['p95Ms']=float(np.percentile(frames,95))
                    summary['profiles'].setdefault(mode,{}).setdefault(variant,[]).append(result)
                    (out/'summary.json').write_text(json.dumps(summary,indent=2));print(label,result,flush=True)
                if repeat==0:
                    before,_=read_pfm(out/f'{mode}-baseline-hdr.pfm');after,_=read_pfm(out/f'{mode}-candidate-hdr.pfm')
                    assert np.isfinite(before).all() and np.isfinite(after).all()
                    error=np.abs(before-after)
                    quality={'maxAbsolute':float(error.max()),'p99Absolute':float(np.percentile(error,99)),
                        'meanRelative':float(error.mean()/max(float(np.abs(before).mean()),1e-4))}
                    summary['captureDifferences'][mode]=quality
                    assert quality['p99Absolute']<=.001 and quality['meanRelative']<=.001, f'Output changed: {quality}'
        for mode,variants in summary['profiles'].items():
            summary['comparisons'][mode]=comparison([r['frameMs'] for r in variants['baseline']],[r['frameMs'] for r in variants['candidate']])
        (out/'summary.json').write_text(json.dumps(summary,indent=2))
    finally:
        install('candidate')
        assert ecs_gate.digest(repo/'assets/scenes'/args.scene)==authored
    print('MATCHED GI BENCHMARK COMPLETE',json.dumps(summary['comparisons']),flush=True)


if __name__=='__main__':
    main()
