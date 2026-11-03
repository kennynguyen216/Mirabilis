"""Compare CPU albedo volumes from shared source images and packed tracer textures."""
import argparse
import json
from pathlib import Path
import re

import ecs_gate


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--configuration',choices=('Debug','Release'),default='Debug')
    args=parser.parse_args()
    repo,out=Path(__file__).resolve().parents[1],args.output.resolve()
    assert not out.exists() or not any(out.iterdir()), 'Use fresh evidence directory'
    out.mkdir(parents=True);ecs_gate.REPO,ecs_gate.OUT=repo,out
    results={}
    for mode in ('shared','packed'):
        flags={'MIRABILIS_QT_EDITOR':'0','MIRABILIS_TEST_SCENE':'living_room_showcase.json',
            'MIRABILIS_TEST_WINDOW':'800x600','MIRABILIS_TEST_CAMERA':'4.740 1.700 -2.947 -0.133 -14.670',
            'MIRABILIS_TEST_HIDE_PANELS':'1','MIRABILIS_TEST_FRAMES':'180',
            'MIRABILIS_LUMEN_LITE':'1','MIRABILIS_TEST_REQUIRE_LUMEN':'1',
            'MIRABILIS_LUMEN_HARDWARE':'1','MIRABILIS_SSGI_SCREEN_TRACE':'0',
            'MIRABILIS_TEST_ALBEDO_CHECKSUM':'1','MIRABILIS_R4_UNCAP_FRAMES':'1',
            'MIRABILIS_BENCHMARK_SKIP_CPU_BVH_VALIDATION':'1',
            'VK_INSTANCE_LAYERS':'VK_LAYER_KHRONOS_validation',
            'VK_LAYER_VALIDATE_SYNC':'1','VK_KHRONOS_VALIDATION_VALIDATE_SYNC':'true'}
        if mode=='packed':flags['MIRABILIS_TEST_FULL_TRACE_UPLOAD']='1'
        logs,result=ecs_gate.run(mode,args.configuration,flags)
        checksum=re.findall(r'GI albedo checksum=(\d+) texels=(\d+) colored=(\d+)',logs)[-1]
        packed=int(re.findall(r'GI GPU trace payload:.*CPU-texels=(\d+)',logs)[-1])
        assert int(checksum[2])>0, 'Albedo volume is empty/black'
        assert packed>1 if mode=='packed' else packed==0
        results[mode]={**result,'albedo':checksum,'packedTexels':packed}
    assert results['shared']['albedo']==results['packed']['albedo'], 'Shared textures changed CPU albedo sampling'
    (out/'summary.json').write_text(json.dumps(results,indent=2))
    print('TRACE TEXTURE GPU GATE PASS',flush=True)


if __name__=='__main__':
    main()
