"""Rendered regression cases for software-GI sun and punctual visibility."""
import argparse
import copy
import json
from pathlib import Path
import re
import os
import sys
import numpy as np
import ecs_gate
from lumen_gate import fixture, read_pfm


def room(repo, source, roof):
    doc=fixture(repo)
    doc['objects']=[obj for obj in doc['objects'] if 'light' not in obj.get('components',{})]
    doc['lighting']['sunDirection']=[0,1,.6]
    doc['referenceLighting']['sunRadiance']=[2,2,2] if source=='sun' else [0,0,0]
    if roof:
        canopy=copy.deepcopy(doc['objects'][2]);canopy['id']=10
        canopy['components']['name']['value']='Opaque occluder'
        canopy['components']['transform'].update(position=[0,3,0],scale=[14,roof,14])
        doc['objects'].append(canopy)
    if source=='point':
        light=fixture(repo)['objects'][-1]
        light['id']=11;light['components']['transform']['position']=[0,5,-1]
        doc['objects'].append(light)
    return doc


def bake_missing(repo, logs):
    missing=set(re.findall(r'Scene SDF missing mesh: ([0-9a-f]+)',logs))
    if not missing:return
    import trimesh
    sys.path.insert(0,str(repo/'scripts'))
    from bake_sdf import bake_world_grid,write_sdf
    for name in missing:
        mesh=trimesh.load(repo/'assets/sdf/queue'/(name+'.obj'),force='mesh',process=False)
        grid,lo,hi,_,_=bake_world_grid(mesh)
        target=repo/'assets/sdf/cache'/(name+'.sdf');temporary=target.with_suffix('.visibility.partial')
        write_sdf(temporary,grid,lo,hi);os.replace(temporary,target)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--configuration',choices=['Debug','Release'],default='Debug')
    args=parser.parse_args();repo=Path(__file__).resolve().parents[1];out=args.output.resolve()
    assert not out.exists() or not any(out.iterdir()),'Use fresh evidence directory'
    scenes=out/'scenes';scenes.mkdir(parents=True)
    for source in ['sun','point']:
        for name,roof in [('open',0),('blocked',.4),('thin',.04)]:
            (scenes/(source+'-'+name+'.json')).write_text(json.dumps(room(repo,source,roof)))
    far=room(repo,'point',0)
    for obj in far['objects'][1:]:
        transform=obj['components']['transform']
        if 'light' in obj['components']:
            transform['position']=[0,10,-.1]
            obj['components']['light'].update(intensity=200,range=20)
        else:
            transform['position']=[x*.1 for x in transform['position']]
            transform['scale']=[x*.1 for x in transform['scale']]
    (scenes/'point-far.json').write_text(json.dumps(far))
    # Broad overlapping card bounds with a valid receiver inserted after >64 candidates.
    overflow=room(repo,'sun',0);floor=overflow['objects'][1]
    overflow['objects']=overflow['objects'][:1]
    for i in range(20):
        shell=copy.deepcopy(room(repo,'sun',0)['objects'][2]);shell['id']=i+20;shell['siblingIndex']=i
        shell['components']['name']['value']='Large bounds '+str(i)
        shell['components']['transform'].update(position=[0,0,0],scale=[40+i*.1]*3)
        shell['components']['material']['tint']=[.2,.2,.2,1]
        overflow['objects'].append(shell)
    floor['siblingIndex']=30;overflow['objects'].append(floor)
    for order in ['last','first']:
        variant=copy.deepcopy(overflow)
        if order=='first':
            for obj in variant['objects'][1:]:obj['siblingIndex']+=1
            variant['objects'][-1]['siblingIndex']=0
        (scenes/('overflow-'+order+'.json')).write_text(json.dumps(variant))
    ecs_gate.REPO,ecs_gate.OUT=repo,out
    common={'MIRABILIS_QT_EDITOR':'0','MIRABILIS_TEST_SCENE_DIRECTORY':str(scenes),
        'MIRABILIS_TEST_WINDOW':'800x600','MIRABILIS_TEST_CAMERA':'0 1.5 5 -0.15 0',
        'MIRABILIS_TEST_HIDE_PANELS':'1','MIRABILIS_LUMEN_LITE':'1','MIRABILIS_LUMEN_HARDWARE':'0',
        'MIRABILIS_SSGI_SCREEN_TRACE':'0','MIRABILIS_SURFACE_CACHE_RADIOSITY':'0',
        'MIRABILIS_R4_UNCAP_FRAMES':'1','MIRABILIS_BENCHMARK_SKIP_CPU_BVH_VALIDATION':'1',
        'VK_INSTANCE_LAYERS':'VK_LAYER_KHRONOS_validation','VK_LAYER_VALIDATE_SYNC':'1',
        'VK_KHRONOS_VALIDATION_VALIDATE_SYNC':'true'}
    logs,_=ecs_gate.run('preflight',args.configuration,{**common,'MIRABILIS_TEST_SCENE':'point-blocked.json','MIRABILIS_TEST_FRAMES':'2'})
    assert 'hardware-sun=true' in logs,'This regression requires an RT-capable device for the mixed backend'
    bake_missing(repo,logs)
    images={};results={}
    for source in ['sun','point']:
        for state in ['open','blocked','thin']:
            for backend in ['hardware','software']:
                name=source+'-'+state+'-'+backend
                flags={**common,'MIRABILIS_TEST_SCENE':source+'-'+state+'.json',
                    'MIRABILIS_TEST_FRAMES':'240','MIRABILIS_TEST_REQUIRE_LUMEN':'1',
                    'MIRABILIS_RASTER_CAPTURE':str(out/name)}
                if source=='sun':
                    flags.update(MIRABILIS_IQ_RT_SHADOWS='1' if backend=='hardware' else '0',
                        MIRABILIS_TEST_GI_UNCOVERED='1',MIRABILIS_R4_CONTRIBUTIONS='ray_field_uncovered')
                else:
                    flags.update(MIRABILIS_LUMEN_HARDWARE='1' if backend=='hardware' else '0',
                        MIRABILIS_RENDER_DEBUG_VIEW='29',MIRABILIS_SURFACE_CACHE_PAGE='8')
                logs,result=ecs_gate.run(name,args.configuration,flags)
                assert 'GI effective mode=lumen-lite' in logs
                fields=re.findall(r'backend=distance-field instances=(\d+) ready=(\d+) missing-meshes=(\d+)',logs)
                if flags['MIRABILIS_LUMEN_HARDWARE']=='0':
                    assert fields and all(a==b and c=='0' for a,b,c in fields),'Incomplete software field'
                image,_=read_pfm(out/(name+'.pfm'));assert np.isfinite(image).all()
                # PFM data view8: green is cache irradiance, independent of forward lighting.
                image=image[:,:,1] if source=='point' else image.mean(axis=2)
                images[name]=image;result['mean']=float(image.mean());results[name]=result
                (out/'summary.json').write_text(json.dumps(results,indent=2))
    far_means=[]
    for backend in ['hardware','software']:
        name='point-far-'+backend
        logs,result=ecs_gate.run(name,args.configuration,{**common,'MIRABILIS_TEST_SCENE':'point-far.json',
            'MIRABILIS_LUMEN_HARDWARE':'1' if backend=='hardware' else '0',
            'MIRABILIS_RENDER_DEBUG_VIEW':'29','MIRABILIS_SURFACE_CACHE_PAGE':'8',
            'MIRABILIS_TEST_FRAMES':'240','MIRABILIS_TEST_REQUIRE_LUMEN':'1','MIRABILIS_RASTER_CAPTURE':str(out/name),
            'MIRABILIS_TEST_CAMERA':'0 .15 .5 -0.15 0'})
        image,_=read_pfm(out/(name+'.pfm'));mean=float(image[:,:,1].mean())
        assert np.isfinite(image).all() and mean>1e-4,'Far unoccluded point light disappeared'
        far_means.append(mean);result['mean']=mean;results[name]=result
    assert abs(far_means[0]-far_means[1])/max(far_means)<.2,'Far-light backend disagreement'
    for order in ['last','first']:
        name='overflow-'+order
        logs,result=ecs_gate.run(name,args.configuration,{**common,'MIRABILIS_TEST_SCENE':name+'.json',
            'MIRABILIS_LUMEN_HARDWARE':'1','MIRABILIS_RENDER_DEBUG_VIEW':'29','MIRABILIS_SURFACE_CACHE_PAGE':'9',
            'MIRABILIS_SURFACE_CACHE_ATLAS_SIZE':'1024',
            'MIRABILIS_TEST_FRAMES':'240','MIRABILIS_TEST_REQUIRE_LUMEN':'1','MIRABILIS_RASTER_CAPTURE':str(out/name)})
        counts=re.findall(r'fullest cell (\d+) cards',logs)
        assert counts and max(map(int,counts))>64,'Overflow fixture did not exceed the old cap'
        image,_=read_pfm(out/(name+'.pfm'))
        receiver=image[:,:,2]>.8
        assert receiver.sum()>100,'Receiver not visible'
        assert image[:,:,0][receiver].mean()>.99,'Valid receiver card was dropped'
        assert np.abs(image[:,:,1][receiver]-image[:,:,2][receiver]).mean()<.02,'Wrong surface selected'
        result['coveredFraction']=float(image[:,:,0][receiver].mean());results[name]=result
    for source in ['sun','point']:
        for backend in ['hardware','software']:
            opened=results[source+'-open-'+backend]['mean']
            assert opened>1e-4,(source,backend,'unoccluded lighting missing',opened)
            for state in ['blocked','thin']:
                blocked=results[source+'-'+state+'-'+backend]['mean']
                assert blocked<opened*.08,(source,state,backend,'occlusion leaked',blocked,opened)
        a=results[source+'-open-hardware']['mean'];b=results[source+'-open-software']['mean']
        assert abs(a-b)/max(a,b)<.2,(source,'unoccluded backend disagreement',a,b)
    (out/'summary.json').write_text(json.dumps(results,indent=2))
    print('LIGHTING VISIBILITY GPU GATE PASS',flush=True)

if __name__=='__main__':main()
