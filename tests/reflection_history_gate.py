"""Reject reflection history produced while a continuous light edit is still relighting."""
import argparse
import json
from pathlib import Path
import re

import ecs_gate
from lumen_gate import fixture


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--configuration',choices=('Debug','Release'),default='Debug')
    args=parser.parse_args()
    repo,out=Path(__file__).resolve().parents[1],args.output.resolve()
    assert not out.exists() or not any(out.iterdir()), 'Use fresh evidence directory'
    scenes=out/'scenes';scenes.mkdir(parents=True)
    scene=fixture(repo,True)
    scene['objects'].append(fixture(repo)['objects'][-1])
    (scenes/'relight.json').write_text(json.dumps(scene))
    ecs_gate.REPO,ecs_gate.OUT=repo,out
    logs,result=ecs_gate.run('unfinished-relight',args.configuration,{
        'MIRABILIS_QT_EDITOR':'0','MIRABILIS_TEST_SCENE':'relight.json',
        'MIRABILIS_TEST_SCENE_DIRECTORY':str(scenes),'MIRABILIS_TEST_WINDOW':'800x600',
        'MIRABILIS_TEST_CAMERA':'0 2.5 8 -0.6 0','MIRABILIS_TEST_HIDE_PANELS':'1',
        'MIRABILIS_TEST_FRAMES':'120','MIRABILIS_TEST_LIGHT_DRAG':'1',
        'MIRABILIS_IQ13_SUN_ONLY_RELIGHT':'0','MIRABILIS_LUMEN_LITE':'1',
        'MIRABILIS_LUMEN_HARDWARE':'1','MIRABILIS_SSGI_SCREEN_TRACE':'0',
        'MIRABILIS_TEST_REQUIRE_LUMEN':'1','MIRABILIS_R4_UNCAP_FRAMES':'1',
        'VK_INSTANCE_LAYERS':'VK_LAYER_KHRONOS_validation',
        'VK_LAYER_VALIDATE_SYNC':'1','VK_KHRONOS_VALIDATION_VALIDATE_SYNC':'true'})
    status=re.search(r'GI updates:.*pending-texels=(\d+).*reflections-active=true reflection-history=false',logs)
    assert status and int(status[1])>0, 'Unfinished relighting seeded reflection history'
    (out/'summary.json').write_text(json.dumps(result,indent=2))
    print('REFLECTION HISTORY GPU GATE PASS',flush=True)


if __name__=='__main__':
    main()
