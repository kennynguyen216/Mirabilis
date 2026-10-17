import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import ecs_gate

STATES=('sandbox-baseline','reopened-play','floor-contact','reset','edit-restored','second-play','sandbox-replaced')
def check(logs):
    records={}
    for line in logs.splitlines():
        match=re.search(r'SCRIPT state=([\w-]+) frame=(\d+) epoch=(\d+) bodies=(\d+) scripts=(\d+) x=([-+\deE.]+) y=([-+\deE.]+) force=(.*)',line)
        if match:
            state,frame,epoch,bodies,scripts,x,y,force=match.groups()
            assert state not in records, f'duplicate phase {state}'
            records[state]={'frame':int(frame),'epoch':int(epoch),'bodies':int(bodies),'scripts':int(scripts),'x':float(x),'y':float(y),'force':None if force=='none' else json.loads(force)}
    assert tuple(records)==STATES, f'phases missing/out of order: {tuple(records)}'
    for state in ('reopened-play','floor-contact','reset','second-play'):
        assert records[state]['scripts']==1 and records[state]['bodies']==3, state
    assert records['edit-restored']['scripts']==0 and records['edit-restored']['bodies']==3
    for state in ('reopened-play','reset','edit-restored'):
        force=records[state]['force']
        assert force==[4,0,0], f'{state}: authored Force not retained: {force}'
    floor=records['floor-contact']
    assert floor['x']>.1 and .3 <= floor['y'] <= 1.2, 'force/floor behavior not exercised'
    base,end=records['sandbox-baseline'],records['sandbox-replaced']
    assert base['bodies']==end['bodies'] and base['scripts']==end['scripts']==0
    assert records['reset']['epoch']>records['floor-contact']['epoch']
    assert records['edit-restored']['epoch']>records['reset']['epoch']
    assert end['epoch']>records['second-play']['epoch']
    assert 'SCRIPT lifecycle PASS: scratch edit/save/reopen/run, force/floor, reset, leave, sandbox bodies=' in logs
    return records

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--repo',type=Path,default=ecs_gate.REPO)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();repo=args.repo.resolve();out=args.output.resolve()
    assert not out.exists() or not any(out.iterdir()), 'Use fresh evidence output'
    out.mkdir(parents=True,exist_ok=True)
    digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    paths=[*sorted((repo/'assets/scenes').glob('*.json')),repo/'assets/scenes/.last_scene',repo/'bin/Debug/engine.exe']
    before={str(p.relative_to(repo)):digest(p) for p in paths}
    (out/'input-before.json').write_text(json.dumps(before,indent=2))
    scratch=out/'scratch-scenes';scratch.mkdir()
    for path in (repo/'assets/scenes').glob('*.json'):shutil.copy2(path,scratch/path.name)
    ecs_gate.REPO=repo;ecs_gate.OUT=out
    logs,result=ecs_gate.run('script-lifecycle','Debug',{'MIRABILIS_TEST_SCENE':'scripted_physics_demo.json',
        'MIRABILIS_TEST_SCENE_DIRECTORY':str(scratch),'MIRABILIS_TEST_SCRIPT_LIFECYCLE':'1','MIRABILIS_TEST_FRAMES':'280',
        'MIRABILIS_TEST_WINDOW':'1600x900','MIRABILIS_TEST_HIDE_PANELS':'1','MIRABILIS_TEST_CAMERA':'0 2.5 12 -0.08 0',
        'VK_LAYER_VALIDATE_SYNC':'1','VK_KHRONOS_VALIDATION_VALIDATE_SYNC':'true'})
    assert 'GI bounded run complete: frames=280' in logs
    (out/'script-telemetry.json').write_text(json.dumps(check(logs),indent=2))
    after={str(p.relative_to(repo)):digest(p) for p in paths};assert before==after, 'Authored scenes/selection/binary changed'
    (out/'input-after.json').write_text(json.dumps(after,indent=2))
    print('SCRIPT GPU GATE PASS',flush=True)

if __name__=='__main__':main()
