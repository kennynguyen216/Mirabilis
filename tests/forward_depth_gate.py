"""Verify prepass fragment rejection preserves masks, instances and portal views."""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image
import ecs_gate
from graphics_gate import occlusion_fixture
from lumen_gate import read_pfm


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--configuration',choices=['Debug','Release'],default='Debug')
    args=parser.parse_args();repo=Path(__file__).resolve().parents[1];out=args.output.resolve()
    assert not out.exists() or not any(out.iterdir()),'Use fresh evidence directory'
    scenes=out/'scenes';scenes.mkdir(parents=True)
    original=json.loads((repo/'assets/scenes/graphics_showcase.json').read_text())
    masked=occlusion_fixture(scenes,original)
    model=scenes/'grid.gltf';gltf=json.loads(model.read_text());binary=scenes/'grid.bin'
    uv=np.array([(x/64,y/32) for y in range(33) for x in range(65)],dtype='<f4')
    data=binary.read_bytes();binary.write_bytes(data+uv.tobytes())
    gltf['buffers'][0]['byteLength']=len(data)+uv.nbytes
    gltf['bufferViews'].append({'buffer':0,'byteOffset':len(data),'byteLength':uv.nbytes})
    gltf['accessors'].append({'bufferView':2,'componentType':5126,'count':len(uv),'type':'VEC2'})
    gltf['meshes'][0]['primitives'][0]['attributes']['TEXCOORD_0']=2
    gltf['images']=[{'uri':'alpha.png'}];gltf['textures']=[{'source':0}]
    gltf['materials'][0].update(alphaMode='MASK',alphaCutoff=.5)
    gltf['materials'][0]['pbrMetallicRoughness']['baseColorTexture']={'index':0}
    model.write_text(json.dumps(gltf))
    pixels=np.full((16,16,4),255,dtype=np.uint8)
    y,x=np.indices((16,16));pixels[:,:,3]=np.where((x//2+y//2)%2,255,0)
    Image.fromarray(pixels).save(scenes/'alpha.png')
    (scenes/'masked.json').write_text(json.dumps(masked))
    ecs_gate.REPO,ecs_gate.OUT=repo,out
    common={'MIRABILIS_QT_EDITOR':'0','MIRABILIS_TEST_WINDOW':'960x600',
        'MIRABILIS_TEST_CAMERA':'0 3.5 10 -0.12 0','MIRABILIS_TEST_HIDE_PANELS':'1',
        'MIRABILIS_SSGI_DISABLE':'1','MIRABILIS_AUTO_EXPOSURE':'0','MIRABILIS_R4_UNCAP_FRAMES':'1',
        'VK_INSTANCE_LAYERS':'VK_LAYER_KHRONOS_validation','VK_LAYER_VALIDATE_SYNC':'1',
        'VK_KHRONOS_VALIDATION_VALIDATE_SYNC':'true'}
    results={}
    for name,scene,extra in [('graphics','graphics_showcase.json',{}),
        ('masked','masked.json',{'MIRABILIS_TEST_SCENE_DIRECTORY':str(scenes)}),
        ('masked-unbatched','masked.json',{'MIRABILIS_TEST_SCENE_DIRECTORY':str(scenes),'MIRABILIS_TEST_DISABLE_INSTANCING':'1'}),
        ('portal-traversal','gi_portal_pair.json',{'MIRABILIS_TEST_PORTAL_TRAVERSAL':'1'})]:
        images=[]
        for reject in [False,True]:
            label=name+('-reject' if reject else '-reference')
            logs,result=ecs_gate.run(label,args.configuration,{**common,**extra,
                'MIRABILIS_TEST_SCENE':scene,'MIRABILIS_TEST_FRAMES':'200' if name=='portal-traversal' else '48',
                'MIRABILIS_PREPASS_REJECT':'1' if reject else '0','MIRABILIS_RASTER_CAPTURE':str(out/label)})
            if name=='portal-traversal': assert 'crossed=true' in logs
            assert ('Forward prepass rejection: '+str(reject).lower()) in logs
            image,_=read_pfm(out/(label+'.pfm'));assert np.isfinite(image).all()
            images.append(image);results[label]=result
        delta=np.abs(images[0]-images[1]);maximum=float(delta.max())
        assert maximum==0,(name,'Prepass rejection changed HDR output',maximum)
        results[name+'-comparison']={'maximumAbsoluteError':maximum}
        (out/'summary.json').write_text(json.dumps(results,indent=2))
    print('FORWARD DEPTH GPU GATE PASS',flush=True)


if __name__=='__main__':main()
