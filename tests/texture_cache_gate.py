"""Exercise BC7 color, BC5 normal decoding, and corrupt-cache RGBA fallback on Vulkan."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import numpy as np
from PIL import Image
import ecs_gate
from graphics_gate import occlusion_fixture
from lumen_gate import read_pfm


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--configuration',choices=['Debug','Release'],default='Debug')
    parser.add_argument('--texconv',type=Path,required=True)
    parser.add_argument('--gpu',type=int,default=0)
    args=parser.parse_args();repo=Path(__file__).resolve().parents[1];out=args.output.resolve()
    assert not out.exists() or not any(out.iterdir()),'Use fresh evidence directory'
    scenes=out/'scenes';scenes.mkdir(parents=True)
    original=json.loads((repo/'assets/scenes/graphics_showcase.json').read_text())
    scene=occlusion_fixture(scenes,original);scene['objects']=scene['objects'][:-1]
    model=scenes/'grid.gltf';gltf=json.loads(model.read_text());binary=scenes/'grid.bin'
    uv=np.array([(x/4,y/2) for y in range(33) for x in range(65)],dtype='<f4')
    data=binary.read_bytes();binary.write_bytes(data+uv.tobytes())
    gltf['buffers'][0]['byteLength']=len(data)+uv.nbytes
    gltf['bufferViews'].append({'buffer':0,'byteOffset':len(data),'byteLength':uv.nbytes})
    gltf['accessors'].append({'bufferView':2,'componentType':5126,'count':len(uv),'type':'VEC2'})
    gltf['meshes'][0]['primitives'][0]['attributes']['TEXCOORD_0']=2
    gltf['images']=[{'uri':'color.png'},{'uri':'normal.png'}]
    gltf['textures']=[{'source':0},{'source':1}]
    gltf['materials'][0]['pbrMetallicRoughness'].update(baseColorFactor=[1,1,1,1],
        baseColorTexture={'index':0},roughnessFactor=.4)
    gltf['materials'][0]['normalTexture']={'index':1,'scale':.7}
    model.write_text(json.dumps(gltf));(scenes/'scene.json').write_text(json.dumps(scene))
    y,x=np.indices((64,64));pixels=np.full((64,64,4),255,dtype=np.uint8)
    pixels[:,:,0]=80+x*2;pixels[:,:,1]=100+y;pixels[:,:,2]=140
    pixels[:,:,3]=np.where((x//8+y//8)%2,255,0)
    Image.fromarray(pixels).save(scenes/'color.png')
    Image.fromarray(np.full((64,64,4),[153,115,252,255],dtype=np.uint8)).save(scenes/'normal.png')
    ecs_gate.REPO,ecs_gate.OUT=repo,out
    common={'MIRABILIS_QT_EDITOR':'0','MIRABILIS_TEST_SCENE':'scene.json',
        'MIRABILIS_TEST_SCENE_DIRECTORY':str(scenes),'MIRABILIS_TEST_FRAMES':'48',
        'MIRABILIS_TEST_WINDOW':'960x600','MIRABILIS_TEST_CAMERA':'0 3.5 10 -0.12 0',
        'MIRABILIS_TEST_HIDE_PANELS':'1','MIRABILIS_SSGI_DISABLE':'1',
        'MIRABILIS_AUTO_EXPOSURE':'0','MIRABILIS_R4_UNCAP_FRAMES':'1',
        'VK_INSTANCE_LAYERS':'VK_LAYER_KHRONOS_validation','VK_LAYER_VALIDATE_SYNC':'1',
        'VK_KHRONOS_VALIDATION_VALIDATE_SYNC':'true'}
    images={};results={}
    for name in ['rgba','compressed','masked-reference','masked-cache','corrupt-fallback']:
        if name=='compressed':
            subprocess.run([sys.executable,str(repo/'scripts/cache_textures.py'),str(model),
                '--asset-root',str(scenes),'--texconv',str(args.texconv.resolve()),'--gpu',str(args.gpu),
                '--report',str(out/'quality.json')],check=True,cwd=repo)
            quality=json.loads((out/'quality.json').read_text());assert len(quality)==2
            assert all(r['accepted'] for r in quality.values()),quality
        if name=='corrupt-fallback':
            for cache in scenes.glob('*.mbc'):
                data=bytearray(cache.read_bytes());data[-1]^=1;cache.write_bytes(data)
        if name=='masked-reference':
            gltf['materials'][0].update(alphaMode='MASK',alphaCutoff=.5)
            del gltf['materials'][0]['normalTexture']
            model.write_text(json.dumps(gltf))
        if name=='corrupt-fallback':
            gltf['materials'][0].pop('alphaMode');gltf['materials'][0].pop('alphaCutoff')
            gltf['materials'][0]['normalTexture']={'index':1,'scale':.7}
            model.write_text(json.dumps(gltf))
        logs,result=ecs_gate.run(name,args.configuration,{**common,
            'MIRABILIS_TEXTURE_CACHE':'0' if name in ['rgba','masked-reference'] else '1',
            'MIRABILIS_RASTER_CAPTURE':str(out/name)})
        expected='BC7=1 BC5=1 RGBA=0' if name=='compressed' else 'BC7=0 BC5=0 RGBA='+('1' if name.startswith('masked') else '2')
        assert expected in logs
        image,_=read_pfm(out/(name+'.pfm'));assert np.isfinite(image).all()
        images[name]=image;results[name]=result
    delta=np.abs(images['compressed']-images['rgba'])
    results['quality']={'meanRelativeError':float(delta.mean()/max(np.abs(images['rgba']).mean(),1e-5)),
        'maximumAbsoluteError':float(delta.max())}
    assert results['quality']['meanRelativeError']<.02,results['quality']
    assert np.array_equal(images['rgba'],images['corrupt-fallback']),'Corrupt cache changed original-image fallback'
    assert np.array_equal(images['masked-reference'],images['masked-cache']),'Cache changed masked coverage'
    (out/'summary.json').write_text(json.dumps(results,indent=2))
    print('TEXTURE CACHE GPU GATE PASS',results['quality'],flush=True)


if __name__=='__main__':main()
