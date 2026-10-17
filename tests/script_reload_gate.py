"""Exercise a real source save, background compilation and in-Play reload."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import ecs_gate


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--repo', type=Path, default=ecs_gate.REPO)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--configuration', choices=['Debug', 'Release'], default='Debug')
    args = parser.parse_args()
    repo, out = args.repo.resolve(), args.output.resolve()
    assert not out.exists(), 'Use fresh evidence output'
    out.mkdir(parents=True)
    source = repo / 'scripting/Mirabilis.SampleScripts/ReloadValidation.cs'
    assert not source.exists(), 'Never overwrite a user source file'
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    authored = [*sorted((repo/'assets/scenes').glob('*.json')), repo/'assets/scenes/.last_scene']
    before = {str(p.relative_to(repo)): digest(p) for p in authored}
    scratch = out/'scenes'; scratch.mkdir()
    for p in (repo/'assets/scenes').glob('*.json'): shutil.copy2(p, scratch/p.name)
    original = subprocess.check_output(['git', 'show', '3d33fc3:assets/scenes/scripted_physics_demo.json'], cwd=repo)
    scene = json.loads(original)
    actor = next(o for o in scene['objects'] if o['id'] == 3)
    actor['components']['rigidBody']['gravityFactor'] = 0
    actor['components']['script']['type'] = 'Mirabilis.SampleScripts.ReloadValidation'
    actor['components']['script']['fields'] = {}
    (scratch/'script_reload_demo.json').write_text(json.dumps(scene))
    code = '''using Mirabilis.ScriptCore;
namespace Mirabilis.SampleScripts;
public sealed class ReloadValidation : Script {
    public int Creates, Updates, PrivateObserved;
    private int counter = 40;
    public override void OnCreate() { ++Creates; Entity.RigidBody.SetVelocity(new Vec3(.25f,0,0)); }
    public override void OnUpdate(float dt) { Updates += STEP; PrivateObserved = ++counter; }
}
'''
    deploy = repo/'bin'/args.configuration
    names = ['Mirabilis.SampleScripts.dll', 'Mirabilis.SampleScripts.pdb', 'Mirabilis.SampleScripts.deps.json']
    saved = {name: (deploy/name).read_bytes() for name in names}
    process = None
    env = {k:v for k,v in os.environ.items() if not k.startswith(('MIRABILIS_', 'VK_LAYER', 'VK_KHRONOS_VALIDATION_'))}
    flags = dict(MIRABILIS_TEST_SCENE='script_reload_demo.json', MIRABILIS_TEST_SCENE_DIRECTORY=str(scratch),
               MIRABILIS_TEST_SCRIPT_RELOAD='1', MIRABILIS_TEST_FRAMES='6000', MIRABILIS_TEST_HIDE_PANELS='1',
               MIRABILIS_TEST_WINDOW='1600x900', MIRABILIS_TEST_CAMERA='0 2.5 12 -0.08 0',
               VK_LAYER_VALIDATE_SYNC='1', VK_KHRONOS_VALIDATION_VALIDATE_SYNC='true')
    env.update(flags)
    (out/'environment.json').write_text(json.dumps(flags, indent=2))
    try:
        source.write_text(code.replace('STEP', '1'))
        with (out/'initial-build.log').open('w') as log:
            result = subprocess.run(['dotnet', 'build', str(repo/'scripting/Mirabilis.SampleScripts/Mirabilis.SampleScripts.csproj'),
                '-c', args.configuration, '--artifacts-path', str(out/'dotnet'), '--output', str(out/'managed'),
                '--ignore-failed-sources', '-p:NuGetAudit=false'], cwd=repo, stdout=log, stderr=subprocess.STDOUT)
        assert result.returncode == 0, 'Initial probe build failed'
        for name in names: shutil.copy2(out/'managed'/name, deploy/name)
        with (out/'stdout.log').open('w') as stdout, (out/'stderr.log').open('w') as stderr:
            process = subprocess.Popen([str(deploy/'engine.exe')], cwd=deploy, env=env, stdout=stdout, stderr=stderr,
                                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            deadline = time.monotonic()+180
            while time.monotonic() < deadline:
                assert process.poll() is None, 'Engine exited before reload readiness'
                if 'SCRIPT reload READY' in (out/'stdout.log').read_text(errors='replace'): break
                time.sleep(.1)
            else: raise AssertionError('Engine readiness timeout')
            source.write_text(code.replace('STEP', '10'))
            (out/'source-after.cs').write_text(source.read_text())
            assert process.wait(timeout=180) == 0, 'Engine reload failed'
        logs = (out/'stdout.log').read_text(errors='replace')+(out/'stderr.log').read_text(errors='replace')
        assert 'SCRIPT reload PRESERVED:' in logs and 'SCRIPT reload PASS:' in logs
        assert 'Script reload committed: 1 instances' in logs
        assert not ecs_gate.FATAL.search(logs), 'Renderer validation failure'
    finally:
        if process is not None and process.poll() is None:
            process.terminate(); process.wait(timeout=30)
        if source.exists(): source.unlink()
        for name, data in saved.items(): (deploy/name).write_bytes(data)
        after = {str(p.relative_to(repo)): digest(p) for p in authored}
        (out/'preservation.json').write_text(json.dumps({'before':before, 'after':after}, indent=2))
        assert before == after, 'Authored scene or selection changed'
    (out/'result.json').write_text(json.dumps({'result':'PASS', 'configuration':args.configuration,
        'engineSha256':digest(deploy/'engine.exe'), 'sourceSavedAndRecompiled':True,
        'worldAndPrivateStatePreserved':True, 'authoredFilesUnchanged':True}, indent=2))
    print('SCRIPT LIVE RELOAD GPU PASS', flush=True)


if __name__ == '__main__': main()
