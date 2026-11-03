"""Compile referenced glTF textures to full-resolution, quality-checked BC5/BC7 caches."""
import argparse
import io
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
from urllib.parse import unquote,urlsplit
import zlib
import numpy as np
from PIL import Image

MAGIC=b'MBTCv001'


def image_roles(document):
    roles={}
    def mark(info,role,kind):
        if not info:return
        texture=document['textures'][info['index']]
        source=texture.get('source')
        if source is not None:roles.setdefault((source,role),set()).add(kind)
    for material in document.get('materials',[]):
        pbr=material.get('pbrMetallicRoughness',{})
        mark(pbr.get('baseColorTexture'),'srgb','color')
        mark(pbr.get('metallicRoughnessTexture'),'linear','data')
        mark(material.get('normalTexture'),'linear','normal')
        mark(material.get('occlusionTexture'),'linear','data')
        mark(material.get('extensions',{}).get('KHR_materials_specular',{}).get('specularTexture'),'linear','data')
    return roles


def referenced_images(models,asset_root):
    selected={};asset_root=asset_root.resolve()
    for model in models:
        model=model.resolve();document=json.loads(model.read_text(encoding='utf-8-sig'))
        for (index,role),kinds in image_roles(document).items():
            uri=document['images'][index].get('uri')
            if not uri or urlsplit(uri).scheme:continue
            source=(model.parent/unquote(uri)).resolve()
            if not source.is_relative_to(asset_root):raise ValueError(f'Image escapes asset root: {source}')
            if source.suffix.lower() not in ['.png','.jpg','.jpeg','.tga','.bmp']:continue
            selected.setdefault((source,role),set()).update(kinds)
    return selected


def cache_bytes(source,dds):
    return MAGIC+struct.pack('<QII',len(source),zlib.crc32(source),zlib.crc32(dds))+dds


def codec_for(role,kinds):
    return 99 if role=='srgb' else 83 if kinds=={'normal'} else 98


def dds_metadata(dds,role,codec=None):
    if len(dds)<148:return None
    word=lambda offset:struct.unpack_from('<I',dds,offset)[0]
    if dds[:4]!=b'DDS ' or word(4)!=124 or word(76)!=32 or not word(80)&4 or word(84)!=0x30315844:return None
    if codec is None:codec=99 if role=='srgb' else 98
    if codec not in [83,98,99] or (codec==99)!=(role=='srgb'):return None
    if word(128)!=codec or word(132)!=3 or word(136)!=0 or word(140)!=1 or word(112)!=0:return None
    width,height=word(16),word(12)
    if not 0<width<=8192 or not 0<height<=8192:return None
    levels=max(width,height).bit_length()
    if max(word(28),1)!=levels:return None
    size=sum(((max(1,width>>n)+3)//4)*((max(1,height>>n)+3)//4)*16 for n in range(levels))
    return (width,height,levels) if len(dds)==148+size else None


def cached_dds(cache,source,role,codec=None):
    if len(cache)<172 or cache[:8]!=MAGIC:return None
    size,source_crc,dds_crc=struct.unpack_from('<QII',cache,8);dds=cache[24:]
    if size!=len(source) or source_crc!=zlib.crc32(source) or dds_crc!=zlib.crc32(dds):return None
    if not dds_metadata(dds,role,codec):return None
    return dds


def quality(source_pixels,dds,kinds,codec=98):
    decoded=np.asarray(Image.open(io.BytesIO(dds)).convert('RGBA'))
    if decoded.shape!=source_pixels.shape:raise ValueError('Compression changed source dimensions')
    delta=decoded.astype(np.float32)-source_pixels.astype(np.float32)
    mse=float(np.mean(delta[:,:,:3]**2));psnr=99.0 if mse==0 else 10*math.log10(255**2/mse)
    alpha=float(np.abs(delta[:,:,3]).mean()/255)
    result={'rgbPsnrDb':psnr,'alphaMeanAbsolute':alpha,'maximumByteError':float(np.abs(delta).max())}
    accepted=psnr>=35 and alpha<=1/255
    if 'normal' in kinds:
        before=source_pixels[:,:,:3].astype(np.float32)/127.5-1
        after=decoded[:,:,:3].astype(np.float32)/127.5-1
        if codec==83:
            after[:,:,2]=np.sqrt(np.maximum(1-np.sum(after[:,:,:2]**2,axis=2),0))
        before/=np.maximum(np.linalg.norm(before,axis=2,keepdims=True),1e-5)
        after/=np.maximum(np.linalg.norm(after,axis=2,keepdims=True),1e-5)
        angles=np.degrees(np.arccos(np.clip(np.sum(before*after,axis=2),-1,1)))
        result.update(normalMeanDegrees=float(angles.mean()),normalP99Degrees=float(np.percentile(angles,99)))
        angular_ok=result['normalMeanDegrees']<1 and result['normalP99Degrees']<5
        # Blue and alpha are unused in BC5 normal maps; compare the reconstructed direction.
        accepted=angular_ok if codec==83 and kinds=={'normal'} else accepted and angular_ok
    result['accepted']=bool(accepted);return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('models',type=Path,nargs='+')
    parser.add_argument('--asset-root',type=Path,default=Path(__file__).resolve().parents[1]/'assets')
    parser.add_argument('--texconv',type=Path,required=True)
    parser.add_argument('--report',type=Path,required=True)
    parser.add_argument('--gpu',type=int,default=0)
    parser.add_argument('--force',action='store_true')
    args=parser.parse_args();texconv=args.texconv.resolve();assert texconv.is_file()
    selected=referenced_images(args.models,args.asset_root);report={}
    args.report.parent.mkdir(parents=True,exist_ok=True)
    for (source,role),kinds in selected.items():
        if source.stat().st_size>512*1024*1024:raise ValueError(f'Encoded texture exceeds 512 MiB: {source}')
        original=source.read_bytes();target=source.with_name(source.name+'.'+role+'.mbc')
        codec=codec_for(role,kinds)
        dds=cached_dds(target.read_bytes(),original,role,codec) if target.exists() and not args.force else None
        image=Image.open(io.BytesIO(original))
        if max(image.size)>8192 or image.width*image.height>64*1024*1024:raise ValueError(f'Texture exceeds import limit: {source}')
        pixels=np.asarray(image.convert('RGBA'))
        if pixels.shape[0]%4 or pixels.shape[1]%4:
            report[str(target)]={'accepted':False,'reason':'Non-block-aligned source retained as RGBA'};continue
        if dds is None:
            # TemporaryDirectory owns only this newly created scratch folder.
            with tempfile.TemporaryDirectory(prefix='mirabilis-texture-') as directory:
                command=[str(texconv),'-nologo','-y','-dx10','-f',
                    {83:'BC5_UNORM',98:'BC7_UNORM',99:'BC7_UNORM_SRGB'}[codec],
                    '-m','0','-if','LINEAR','-sepalpha','--ignore-srgb','-gpu',str(args.gpu),'-o',directory]
                if codec!=83:command+=['-bc','x']
                if role=='srgb':command+=['-srgb']
                command+=['--',str(source)]
                process=subprocess.run(command,capture_output=True,text=True,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
                if process.returncode:raise RuntimeError(process.stdout+process.stderr)
                dds=(Path(directory)/(source.stem+'.dds')).read_bytes()
        if not dds_metadata(dds,role,codec):raise ValueError(f'Encoder produced unsupported compressed DDS: {source}')
        result=quality(pixels,dds,kinds,codec)
        result.update(width=pixels.shape[1],height=pixels.shape[0],role=role,ddsBytes=len(dds),
            sourceBytes=len(original),sourceCrc32=zlib.crc32(original),kinds=sorted(kinds),dxgiFormat=codec)
        if source.read_bytes()!=original:raise RuntimeError(f'Source changed during compilation: {source}')
        if result['accepted']:
            packed=cache_bytes(original,dds)
            assert cached_dds(packed,original,role,codec)==dds
            partial=target.with_name(target.name+'.partial')
            partial.write_bytes(packed);os.replace(partial,target)
        elif target.exists():
            target.unlink()  # A previously compiled cache must not bypass a failed quality check.
        report[str(target)]=result;args.report.write_text(json.dumps(report,indent=2))
        print(source.name,role,json.dumps(result),flush=True)
    print('TEXTURE CACHE COMPLETE',sum(v['accepted'] for v in report.values()),'/',len(report),flush=True)


if __name__=='__main__':main()
