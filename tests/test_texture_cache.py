import json
from pathlib import Path
import struct
import tempfile
import sys
import unittest
from unittest.mock import patch
import numpy as np
from PIL import Image
from scripts import cache_textures as tc


def dds_fixture(codec):
    data=bytearray(148+80)
    for offset,value in {0:0x20534444,4:124,12:4,16:8,28:4,76:32,80:4,
                         84:0x30315844,128:codec,132:3,140:1}.items():
        struct.pack_into('<I',data,offset,value)
    return bytes(data)


class TextureCacheTests(unittest.TestCase):
    def test_failed_quality_removes_only_generated_cache_and_retains_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=root/'color.png'
            Image.fromarray(np.full((4,8,4),255,dtype=np.uint8)).save(source)
            original=source.read_bytes();target=root/'color.png.srgb.mbc'
            target.write_bytes(tc.cache_bytes(original,dds_fixture(99)))
            model=root/'scene.gltf';model.write_text(json.dumps({'images':[{'uri':'color.png'}],
                'textures':[{'source':0}],'materials':[{'pbrMetallicRoughness':{'baseColorTexture':{'index':0}}}]}))
            tool=root/'texconv.exe';tool.touch()
            argv=['cache_textures.py',str(model),'--asset-root',str(root),'--texconv',str(tool),'--report',str(root/'quality.json')]
            with patch.object(sys,'argv',argv),patch.object(tc.subprocess,'run') as run:
                tc.main();run.assert_not_called()
            self.assertFalse(target.exists());self.assertEqual(source.read_bytes(),original)
            self.assertFalse(next(iter(json.loads((root/'quality.json').read_text()).values()))['accepted'])

    def test_cache_validates_source_payload_and_exact_codec(self):
        for role,codec in [('srgb',99),('linear',98),('linear',83)]:
            with self.subTest(codec=codec):
                dds=dds_fixture(codec);cache=tc.cache_bytes(b'source',dds)
                self.assertEqual(tc.cached_dds(cache,b'source',role,codec),dds)
                self.assertIsNone(tc.cached_dds(cache,b'changed',role,codec))
                self.assertIsNone(tc.cached_dds(cache[:-1]+b'x',b'source',role,codec))
                self.assertIsNone(tc.cached_dds(cache,b'source',role,71))
                self.assertIsNone(tc.cached_dds(cache+b'x',b'source',role,codec))
                if codec==83:self.assertIsNone(tc.cached_dds(cache,b'source',role,98))

    def test_role_changes_cannot_drop_data_channels(self):
        document={'textures':[{'source':0}],'materials':[
            {'normalTexture':{'index':0}},
            {'pbrMetallicRoughness':{'metallicRoughnessTexture':{'index':0},'baseColorTexture':{'index':0}}}]}
        roles=tc.image_roles(document)
        self.assertEqual(roles[(0,'linear')],{'normal','data'})
        self.assertEqual(tc.codec_for('linear',roles[(0,'linear')]),98)
        self.assertEqual(tc.codec_for('linear',{'normal'}),83)
        self.assertEqual(tc.codec_for('srgb',{'color'}),99)

    def test_source_paths_remain_inside_asset_root(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);model=root/'scene.gltf'
            document={'textures':[{'source':0}],'images':[{'uri':'../outside.png'}],
                'materials':[{'normalTexture':{'index':0}}]}
            model.write_text(json.dumps(document))
            with self.assertRaises(ValueError):tc.referenced_images([model],root)
            document['images'][0]['uri']='stone%20normal.png';model.write_text(json.dumps(document))
            self.assertEqual(tc.referenced_images([model],root),{(root/'stone normal.png','linear'):{'normal'}})

    def test_normal_quality_compares_reconstructed_direction(self):
        original=np.full((4,4,4),[128,128,255,255],dtype=np.uint8)
        bc5=np.full((4,4,4),[128,128,0,255],dtype=np.uint8)
        with patch.object(tc.Image,'open',return_value=Image.fromarray(bc5)):
            result=tc.quality(original,b'dds',{'normal'},83)
            self.assertTrue(result['accepted']);self.assertLess(result['normalMeanDegrees'],.1)
            self.assertFalse(tc.quality(original,b'dds',{'normal','data'},98)['accepted'])
        bad=np.full((4,4,4),[255,128,0,255],dtype=np.uint8)
        with patch.object(tc.Image,'open',return_value=Image.fromarray(bad)):
            self.assertFalse(tc.quality(original,b'dds',{'normal'},83)['accepted'])


if __name__=='__main__':unittest.main()
