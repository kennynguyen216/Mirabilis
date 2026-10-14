import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import time
import platform
import shutil

REPO = Path(__file__).resolve().parent.parent
OUT = None
BASELINE = None
FATAL = re.compile(r'GI TEST FAIL|Validation Error|\[ERROR:|VUID-|Device lost|Unhandled renderer exception|Detected Vulkan error')

def digest(path):
    with path.open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()

def timing_statistics(means):
    median = statistics.median(means)
    spread = (max(means)-min(means))/median*100
    return {'meanFrameMs':means,'medianMeanFrameMs':median,'spreadPercent':spread,
            'allowedSlowdownPercent':2*spread if spread>5 else 5}

def run(name, configuration, flags, seconds=400):
    directory = OUT / name
    directory.mkdir(parents=True, exist_ok=True)
    env = {k:v for k,v in os.environ.items() if not (k.startswith('MIRABILIS_') or k.startswith('VK_LAYER') or k.startswith('VK_KHRONOS_VALIDATION_') or k in ('VK_INSTANCE_LAYERS','VK_LOADER_LAYERS_ENABLE'))}
    env.update(flags)
    (directory / 'environment.json').write_text(json.dumps(flags, indent=2))
    engine = REPO / 'bin' / configuration / 'engine.exe'
    before = digest(REPO / 'assets/scenes/.last_scene')
    started = time.monotonic()
    with (directory / 'stdout.log').open('w') as stdout, (directory / 'stderr.log').open('w') as stderr:
        try:
            p = subprocess.run([str(engine)], cwd=engine.parent, env=env, stdout=stdout, stderr=stderr,
                timeout=seconds, creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            code = p.returncode
        except subprocess.TimeoutExpired:
            code = 'timeout'
    logs = (directory / 'stdout.log').read_text(errors='replace') + (directory / 'stderr.log').read_text(errors='replace')
    result = {'exit':code, 'elapsedSeconds':time.monotonic()-started, 'engineSha256':digest(engine), 'fatalLines':[line for line in logs.splitlines() if FATAL.search(line)], 'lastSceneUnchanged':before == digest(REPO / 'assets/scenes/.last_scene')}
    (directory / 'result.json').write_text(json.dumps(result, indent=2))
    print(name, json.dumps(result), flush=True)
    assert code == 0 and not result['fatalLines'] and result['lastSceneUnchanged'], f'{name} failed; see {directory}'
    assert 'Loaded editor scene:' in logs and flags['MIRABILIS_TEST_SCENE'] in logs, f'{name} did not load requested scene'
    return logs, result

def manifest():
    paths = sorted([*REPO.glob('src/*'), *REPO.glob('shaders/*.spv'), *REPO.glob('assets/scenes/*.json')])
    paths = [p for p in paths if p.is_file()]
    state = {'revision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=REPO,text=True).strip(), 'status':subprocess.check_output(['git','status','--short'],cwd=REPO,text=True), 'inputSha256':{str(p.relative_to(REPO)):digest(p) for p in paths}, 'protocol':{'window':'1600x900','warmup':120,'measured':600,'runs':3,'validation':'Release off; Debug on','timing':'steady_clock loop start-to-start wallDeltaTime including previous present/wait','frameCap':'disabled','presentRequest':'MAILBOX, FIFO fallback','collisionTolerance':{'absolute':1e-5,'relative':1e-6}}, 'environment':{'platform':platform.platform(),'python':platform.python_version(),'VulkanSDK':os.environ.get('VULKAN_SDK')}}
    if os.name == 'nt':
        env = {k:v for k,v in os.environ.items() if k != 'PSModulePath'}
        state['environment']['videoControllers'] = json.loads(subprocess.check_output(['powershell','-NoProfile','-Command','Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion | ConvertTo-Json -Compress'],env=env,text=True))
        state['environment']['powerScheme'] = subprocess.check_output(['powercfg','/getactivescheme'],text=True).strip()
    (OUT/'manifest.json').write_text(json.dumps(state,indent=2))

def collisions():
    directory = OUT/'collisions'
    directory.mkdir(exist_ok=True)
    summary = {}
    for scene in sorted((REPO/'assets/scenes').glob('*.json')):
        path = directory / scene.name
        logs, result = run('collision-'+scene.stem,'Release',{'MIRABILIS_TEST_SCENE':scene.name,'MIRABILIS_TEST_FRAMES':'1','MIRABILIS_TEST_COLLISION_DUMP':str(path),'MIRABILIS_TEST_COLLISION_DUMP_ONLY':'1','MIRABILIS_TEST_WINDOW':'1600x900'})
        assert 'ECS collision dump complete:' in logs
        data = json.loads(path.read_text())
        assert set(data) == {'walls','grounds','ramps'}
        summary[scene.name] = {'counts':{k:len(v) for k,v in data.items()},'sha256':digest(path),'sceneSha256':digest(scene)}
        (OUT/'collision-summary.json').write_text(json.dumps(summary,indent=2))

def timings():
    summary = {}
    for scene, camera in [('sandbox.json','0 2.5 12 -0.08 0'),('sponza_showcase.json','0 2.5 12 0 0')]:
        means = []
        for index in range(1,4):
            logs, result = run(f'timing-{Path(scene).stem}-{index}','Release',{'MIRABILIS_TEST_SCENE':scene,'MIRABILIS_TEST_CAMERA':camera,'MIRABILIS_TEST_WINDOW':'1600x900','MIRABILIS_TEST_HIDE_PANELS':'1','MIRABILIS_TEST_FRAMES':'720','MIRABILIS_BENCHMARK_WARMUP_FRAMES':'120','MIRABILIS_SSGI_BENCHMARK':'1','MIRABILIS_R4_UNCAP_FRAMES':'1'})
            match = re.search(r'average-frame-ms=([\d.]+).*samples=(\d+)',logs)
            assert match and int(match[2]) == 600 and 'GI benchmark focused samples: 600/600' in logs, 'Invalid measurement count/focus'
            assert 'GI bounded run complete: frames=720 renderer=raster' in logs
            means.append(float(match[1]))
            summary[scene] = {'camera':camera,**timing_statistics(means)}
            (OUT/'timing-summary.json').write_text(json.dumps(summary,indent=2))
        if BASELINE:
            baseline = json.loads((BASELINE/'timing-summary.json').read_text())[scene]
            limit = baseline['medianMeanFrameMs']*(1+baseline['allowedSlowdownPercent']/100)
            summary[scene]['allowedSlowdownPercent'] = baseline['allowedSlowdownPercent']
            summary[scene]['baselineMedianMeanFrameMs'] = baseline['medianMeanFrameMs']
            summary[scene]['maximumGateMeanFrameMs'] = limit
            summary[scene]['pass'] = summary[scene]['medianMeanFrameMs'] <= limit
            (OUT/'timing-summary.json').write_text(json.dumps(summary,indent=2))
    assert all(item.get('pass',True) for item in summary.values()), 'Performance gate failed; see timing-summary.json'

def validation(gate=False):
    cases = [
        ('cycle','shadow_showcase.json',20,{'MIRABILIS_TEST_CYCLE':'1'},'GI lifecycle: minimized and restore requested'),
        ('scene-switch','lighting_calibration.json',100,{'MIRABILIS_TEST_SCENE_SWITCH':'sponza_showcase.json'},'GI scene switch loaded: sponza_showcase.json'),
        ('portal-traversal','gi_portal_pair.json',200,{'MIRABILIS_TEST_PORTAL_TRAVERSAL':'1'},'crossed=true'),
        ('portal-placement','sandbox.json',180,{'MIRABILIS_TEST_PORTAL_PLACEMENT':'1'},'Portal placement frame='),
    ]
    if gate:
        cases.append(('ecs-lifecycle','sandbox.json',8,{'MIRABILIS_TEST_ECS_LIFECYCLE':'1'},'ECS lifecycle PASS'))
        scratch = OUT/'scratch-scenes'
        scratch.mkdir(exist_ok=True)
        for scene in (REPO/'assets/scenes').glob('*.json'):
            shutil.copy2(scene,scratch/scene.name)
        shutil.copy2(REPO/'tests/fixtures/ecs_input/invalid_transform.json',scratch/'invalid_transform.json')
    summary = {}
    for name, scene, frames, extra, marker in cases:
        flags = {'MIRABILIS_TEST_SCENE':scene,'MIRABILIS_TEST_FRAMES':str(frames),'MIRABILIS_TEST_WINDOW':'1600x900','VK_LAYER_VALIDATE_SYNC':'1','VK_KHRONOS_VALIDATION_VALIDATE_SYNC':'true',**extra}
        if gate: flags['MIRABILIS_TEST_SCENE_DIRECTORY'] = str(scratch)
        logs,result = run('validation-'+name,'Debug',flags)
        assert f'GI bounded run complete: frames={frames}' in logs and marker in logs, f'{name} path was not exercised'
        if gate: assert 'ECS replacement maps cleared' in logs, f'{name} did not exercise cache clearing'
        summary[name] = result
        (OUT/'validation-summary.json').write_text(json.dumps(summary,indent=2))

if __name__ == '__main__':
    assert (statistics.median([1,3,2]) == 2) and FATAL.search('VUID-example') and not FATAL.search('GI bounded run complete')
    assert FATAL.search('[ERROR: Validation]') and not FATAL.search('[WARNING: General]')
    parser=argparse.ArgumentParser()
    parser.add_argument('mode',choices=['manifest','collisions','timings','validation','gate-validation'])
    parser.add_argument('--repo',type=Path,default=REPO)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--baseline',type=Path)
    args=parser.parse_args()
    REPO=args.repo.resolve(); OUT=args.output.resolve(); BASELINE=args.baseline.resolve() if args.baseline else None
    assert not OUT.exists() or not any(OUT.iterdir()), 'Use a fresh output directory to preserve prior evidence'
    OUT.mkdir(parents=True,exist_ok=True)
    if args.mode == 'gate-validation': validation(gate=True)
    else: globals()[args.mode]()
