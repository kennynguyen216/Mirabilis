"""Rendered moving-field, stale-occlusion, relighting and temporal-noise checks."""
import argparse
import copy
import json
from pathlib import Path
import numpy as np
import ecs_gate
from lumen_gate import fixture, read_pfm
from lighting_visibility_gate import bake_missing


def occupied_points(path, metadata):
    dims=np.array(metadata['dimensions'])
    field=np.fromfile(path,dtype='<f2').reshape((*dims[::-1],4)).astype(np.float32)
    assert np.isfinite(field).all()
    indices=np.argwhere(field[:,:,:,3]<0)[:,::-1]
    assert len(indices)>100,'Moving mesh did not reach the GPU field'
    lo=np.array(metadata['minimum'][:3]);voxel=metadata['minimum'][3]
    return field,lo+(indices+.5)*voxel


def distances(field, metadata, points):
    lo=np.array(metadata['minimum'][:3]);hi=np.array(metadata['maximum'][:3])
    clamped=np.clip(points,lo,hi)
    indices=np.clip(((clamped-lo)/metadata['minimum'][3]).astype(int),0,np.array(metadata['dimensions'])-1)
    stored=field[indices[:,2],indices[:,1],indices[:,0],3]
    outside=np.linalg.norm(points-clamped,axis=1)
    return np.where(outside>0,np.hypot(outside,np.maximum(stored,0)),stored)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--configuration',choices=['Debug','Release'],default='Debug')
    args=parser.parse_args();repo=Path(__file__).resolve().parents[1];out=args.output.resolve()
    assert not out.exists() or not any(out.iterdir()),'Use a fresh evidence directory'
    scenes=out/'scenes';scenes.mkdir(parents=True)
    moving=fixture(repo)
    mover=copy.deepcopy(moving['objects'][2]);mover['id']=12;mover['siblingIndex']=10
    mover['components']['name']['value']='Moving GI occluder'
    mover['components']['transform'].update(position=[-2,2.5,0],scale=[.6,.12,3])
    mover['components']['collider']={'enabled':True,'shape':0,'center':[0,0,0],'halfExtents':[.5,.5,.5]}
    mover['components']['rigidBody']={'motion':'Dynamic','mass':1,'friction':0,'restitution':0,'gravityFactor':0}
    mover['components']['material']['tint']=[.05,.8,.1,1]
    moving['objects'].append(mover)
    moving['objects'][-2]['components']['transform']['position']=[0,4,0]
    for name,doc in [('moving',moving),('point',fixture(repo))]:
        (scenes/(name+'.json')).write_text(json.dumps(doc))
    gap=fixture(repo);gap['objects']=[gap['objects'][0],gap['objects'][1],gap['objects'][-1]]
    gap['objects'][-1]['components']['transform']['position']=[0,5,0]
    roof=copy.deepcopy(mover);roof['id']=13
    roof['components'].pop('rigidBody');roof['components'].pop('collider')
    roof['components']['transform'].update(position=[0,.06,0],scale=[14,.02,14])
    (scenes/'gap-open.json').write_text(json.dumps(gap))
    gap['objects'].append(roof)
    (scenes/'gap-closed.json').write_text(json.dumps(gap))
    ecs_gate.REPO,ecs_gate.OUT=repo,out
    common={'MIRABILIS_QT_EDITOR':'0','MIRABILIS_TEST_SCENE_DIRECTORY':str(scenes),
        'MIRABILIS_TEST_WINDOW':'640x480','MIRABILIS_TEST_CAMERA':'0 3.5 9 -0.3 0',
        'MIRABILIS_TEST_HIDE_PANELS':'1','MIRABILIS_LUMEN_LITE':'1','MIRABILIS_LUMEN_HARDWARE':'0',
        'MIRABILIS_SSGI_SCREEN_TRACE':'0','MIRABILIS_R4_UNCAP_FRAMES':'1',
        'MIRABILIS_SURFACE_CACHE_ATLAS_SIZE':'1024','MIRABILIS_IQ5_CACHE_SKY_SAMPLES':'32',
        'MIRABILIS_BENCHMARK_SKIP_CPU_BVH_VALIDATION':'1',
        'VK_INSTANCE_LAYERS':'VK_LAYER_KHRONOS_validation','VK_LAYER_VALIDATE_SYNC':'1',
        'VK_KHRONOS_VALIDATION_VALIDATE_SYNC':'true'}
    logs,_=ecs_gate.run('preflight',args.configuration,{**common,'MIRABILIS_TEST_SCENE':'moving.json',
        'MIRABILIS_TEST_DYNAMIC_GI':'1','MIRABILIS_TEST_FRAMES':'3'})
    bake_missing(repo,logs)
    results={};sequences={}
    gap_means={}
    for state in ['open','closed']:
        name='gap-'+state
        logs,result=ecs_gate.run(name,args.configuration,{**common,'MIRABILIS_TEST_SCENE':name+'.json',
            'MIRABILIS_TEST_FRAMES':'180','MIRABILIS_TEST_CAMERA':'0 .03 0 0 0',
            'MIRABILIS_RENDER_DEBUG_VIEW':'29','MIRABILIS_SURFACE_CACHE_PAGE':'8',
            'MIRABILIS_SURFACE_CACHE_RADIOSITY':'0','MIRABILIS_RASTER_CAPTURE':str(out/name)})
        image,_=read_pfm(out/(name+'.pfm'));assert np.isfinite(image).all()
        roi=image[int(image.shape[0]*.6):int(image.shape[0]*.9),:,:]
        covered=roi[:,:,0]>.99;assert covered.sum()>100,'Close-gap receiver not covered'
        gap_means[state]=float(roi[:,:,1][covered].mean());result['floorCacheMean']=gap_means[state]
        results[name]=result
    assert gap_means['open']>1e-4 and gap_means['closed']<gap_means['open']*.08,('Origin offset crossed close wall',gap_means)
    for name,extra in {
        'moving':{'MIRABILIS_TEST_SCENE':'moving.json','MIRABILIS_TEST_DYNAMIC_GI':'1'},
        'moving-point-only':{'MIRABILIS_TEST_SCENE':'moving.json','MIRABILIS_TEST_DYNAMIC_GI':'1','MIRABILIS_R4_CACHE_SOURCES':'punctual'},
        'moving-screen':{'MIRABILIS_TEST_SCENE':'moving.json','MIRABILIS_TEST_DYNAMIC_GI':'1','MIRABILIS_SSGI_SCREEN_TRACE':'1'},
        'moving-disabled':{'MIRABILIS_TEST_SCENE':'moving.json','MIRABILIS_TEST_DYNAMIC_GI':'1','MIRABILIS_DYNAMIC_GI':'0'},
        'noise':{'MIRABILIS_TEST_SCENE':'point.json','MIRABILIS_LUMEN_HARDWARE':'1'},
        'noise-random':{'MIRABILIS_TEST_SCENE':'point.json','MIRABILIS_LUMEN_HARDWARE':'1','MIRABILIS_GI_RANDOM_SAMPLING':'1'},
        'noise-long-run':{'MIRABILIS_TEST_SCENE':'point.json','MIRABILIS_LUMEN_HARDWARE':'1','MIRABILIS_IQ9_SSGI_SEED':'2000000'},
        'relighting':{'MIRABILIS_TEST_SCENE':'point.json','MIRABILIS_TEST_LIGHT_DRAG':'1'},
        'lights-off':{'MIRABILIS_TEST_SCENE':'point.json','MIRABILIS_TEST_LIGHT_OFF':'1'},
    }.items():
        moving_case=name.startswith('moving');relight=name=='relighting'
        start,count=(80,64) if moving_case else ((100,32) if relight else ((150,32) if name=='lights-off' else (256,32)))
        directory=out/(name+'-frames')
        flags={**common,**extra,'MIRABILIS_TEST_FRAMES':str(start+count),
            'MIRABILIS_TEST_REQUIRE_LUMEN':'1','MIRABILIS_TEST_GI_SEQUENCE':str(directory),
            'MIRABILIS_TEST_GI_CAPTURE_RANGE':f'{start} {count}',
            'MIRABILIS_SURFACE_CACHE_RADIOSITY':'0',
            'MIRABILIS_R4_CONTRIBUTIONS':'env_diffuse,ray_field_cache,ray_field_uncovered,ray_field_exit,ray_primary_cache_sky'}
        logs,result=ecs_gate.run(name,args.configuration,flags,seconds=400)
        paths=sorted(directory.glob('*.pfm'));assert len(paths)==count
        images=np.stack([read_pfm(p)[0] for p in paths]);assert np.isfinite(images).all()
        sequences[name]=images
        first=json.loads((directory/f'frame-{start:04}.json').read_text())
        last=json.loads((directory/f'frame-{start+count-1:04}.json').read_text())
        if moving_case:assert first['dynamicPixels']>20 and last['dynamicPixels']>20,'Moving pixels lost their cache ownership'
        else:assert first['dynamicPixels']==last['dynamicPixels']==0,'Static pixels lost their cache ownership'
        if name=='moving':
            assert first['active'] and last['active'] and first['missing']==last['missing']==0
            assert last['updates']>first['updates']+40,'Moving field stopped updating'
            assert first['staticRebuilds']==last['staticRebuilds'] and first['surfaceRebuilds']==last['surfaceRebuilds'], 'Motion rebuilt static geometry'
            a,points=occupied_points(directory/f'frame-{start:04}.field',first)
            b,later=occupied_points(directory/f'frame-{start+count-1:04}.field',last)
            delta=np.array(last['bodies'][0])-np.array(first['bodies'][0])
            assert delta[0]>.3 and np.linalg.norm(later.mean(axis=0)-points.mean(axis=0)-delta)<.06
            cleared=float(np.mean(distances(b,last,points)>last['minimum'][3]*2))
            assert cleared>.7,('Old occluder remained in the moving field',cleared)
            result['oldSurfaceClearedFraction']=cleared
        elif name=='moving-disabled':assert not first['active'] and last['updates']==0
        elif relight:
            assert first['historyAcceptedPixels']>100 and last['historyAcceptedPixels']>100,'Relighting discarded all temporal history'
            assert last['relights']>first['relights'] or last['pendingTexels']>0
        elif name=='lights-off':
            assert images[:8].mean()>1e-5 and np.abs(images[-8:]).max()<1e-5,'Old light survived the lights-off update'
        result.update(first=first,last=last,mean=float(images.mean()),temporalVariance=float(np.var(images,axis=0).mean()))
        results[name]=result;(out/'summary.json').write_text(json.dumps(results,indent=2))
    changed=np.abs(sequences['moving']-sequences['moving-disabled'])
    assert np.mean(changed)>1e-5,'Moving field had no lighting effect'
    assert np.abs(sequences['moving']-sequences['moving-point-only']).max()<1e-5,'Moving hit lighting ignored source ownership'
    ratio=results['noise']['temporalVariance']/max(results['noise-random']['temporalVariance'],1e-20)
    assert results['noise-random']['temporalVariance']>1e-10,'Noise control has no measurable variation'
    assert ratio<1,('Low-discrepancy sampling did not reduce temporal noise',ratio)
    energy=abs(results['noise']['mean']-results['noise-random']['mean'])/max(results['noise-random']['mean'],1e-6)
    assert energy<.05,('Noise reduction changed mean energy',energy)
    long_energy=abs(results['noise-long-run']['mean']-results['noise']['mean'])/max(results['noise']['mean'],1e-6)
    assert long_energy<.05 and results['noise-long-run']['temporalVariance']>1e-10,('Long-running sampling lost energy or variation',long_energy)
    results['comparison']={'temporalVarianceRatio':ratio,'noiseMeanRelativeDifference':energy,
        'longRunMeanRelativeDifference':long_energy,'dynamicLightingMeanDifference':float(changed.mean())}
    (out/'summary.json').write_text(json.dumps(results,indent=2))
    print('DYNAMIC GI GPU GATE PASS',results['comparison'],flush=True)


if __name__=='__main__':main()
