#!/usr/bin/env python3
"""Host-only fixture tests. Fixture kernels are never published as artifacts."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('prepare_boot_files',Path(__file__).with_name('prepare_boot_files.py'))
boot=importlib.util.module_from_spec(spec);spec.loader.exec_module(boot)

def fake_image():
    data=bytearray(512);data[:2]=b'MZ';data[56:60]=b'ARM\x64';struct.pack_into('<I',data,60,128)
    data[128:132]=b'PE\0\0';struct.pack_into('<H',data,132,0xaa64)
    struct.pack_into('<H',data,152,0x20b);struct.pack_into('<H',data,220,10)
    return bytes(data)

def fake_fdt():
    compatible=b'xiaomi,piano\0';strings=b'compatible\0'
    body=struct.pack('>I',1)+b'\0\0\0\0'+struct.pack('>III',3,len(compatible),0)+compatible
    body+=b'\0'*((-len(body))%4);body+=struct.pack('>II',2,9)
    total=56+len(body)+len(strings)
    return struct.pack('>10I',0xd00dfeed,total,56,56+len(body),40,17,16,0,len(strings),len(body))+b'\0'*16+body+strings

def fake_cpio():
    data=bytearray()
    for name,contents,mode in (('init',b'#!/bin/sh\nexit 0\n',0o100755),('TRAILER!!!',b'',0)):
        fields=[1,mode,0,0,1,0,len(contents),0,0,0,0,len(name)+1,0]
        data.extend(b'070701'+''.join(f'{x:08x}' for x in fields).encode())
        data.extend(name.encode()+b'\0');data.extend(b'\0'*((-len(data))%4))
        data.extend(contents);data.extend(b'\0'*((-len(data))%4))
    return data

class BootFilesTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
        self.lock=boot.read_json(boot.ROOT/'linux/kernel-profiles.json')
        self.ramdisk=self.root/'fixture.cpio';self.ramdisk.write_bytes(fake_cpio())
        self.stable=self.make_build('stable');self.next=self.make_build('next')
    def tearDown(self):self.temp.cleanup()
    def make_build(self,name):
        folder=self.root/name;folder.mkdir()
        (folder/'Image').write_bytes(fake_image());(folder/'piano.dtb').write_bytes(fake_fdt())
        (folder/'config').write_text('CONFIG_EFI=y\nCONFIG_BLK_DEV_INITRD=y\nCONFIG_RD_GZIP=y\nCONFIG_CMDLINE=""\n# CONFIG_CMDLINE_FORCE is not set\n')
        pin=self.lock['profiles'][name]
        manifest={'profile':name,'mode':'ram','source_commit':pin['commit'],'base_commit':pin['base_commit'],
            'canonical_patch_commits':boot.canonical_commits(self.lock,name),'source_dirty':False,'hardware_verified':False,
            'status':'HOST_BUILT_NOT_HARDWARE_VERIFIED','compiler':'TEST FIXTURE ONLY','kernel_release':'test-fixture',
            'config_sha256':boot.digest(folder/'config'),'fragment_sha256':boot.digest(boot.ROOT/'linux/configs/piano-ram.config'),
            'image':boot.image_info(folder/'Image'),'dtb':{'source':'TEST FIXTURE ONLY','sha256':boot.digest(folder/'piano.dtb'),'validation':'fixture header only'}}
        path=folder/'manifest.json';path.write_text(json.dumps(manifest));return path
    def args(self,**kwargs):
        values=dict(output=self.root/'tree',mode='ram',stable_manifest=self.stable,next_manifest=self.next,
            initramfs=self.ramdisk,stable_initramfs=None,next_initramfs=None,shell_efi=None,
            plan_only=False,allow_unverified=True)
        values.update(kwargs);return argparse.Namespace(**values)
    def modify(self,path,**kwargs):
        data=boot.read_json(path);data.update(kwargs);path.write_text(json.dumps(data))
    def test_source_and_real_key_schema(self):
        self.assertTrue(boot.source_contract())
        menu=boot.read_json(boot.TEMPLATE);self.assertTrue(boot.validate_menu(menu))
        menu['boot']['configs']['piano-stable']['extra']['use_efi']=True
        with self.assertRaisesRegex(ValueError,'obsolete'):boot.validate_menu(menu)
    def test_unverified_requires_explicit_flag(self):
        with self.assertRaisesRegex(ValueError,'allow-unverified'):boot.prepare(self.args(allow_unverified=False))
        self.assertFalse((self.root/'tree').exists())
    def test_candidate_tree_has_independent_profiles_and_no_rescue_claim(self):
        result=boot.prepare(self.args());self.assertEqual(result['default'],'simple-init')
        self.assertFalse(result['profiles']['stable']['rescue_eligible'])
        self.assertEqual(result['profiles']['next']['status'],'UNVERIFIED_HOST_STAGED')
        self.assertEqual(boot.check_tree(self.root/'tree')['state'],'HOST_ONLY_NOT_DEPLOYED')
        self.assertTrue((self.root/'tree/EFI/Piano/Stable/Image.efi').exists())
        self.assertTrue((self.root/'tree/EFI/Piano/Next/piano.dtb').exists())
        self.assertEqual(boot.initramfs_info(self.root/'tree/EFI/Piano/Stable/initramfs.cpio.gz')['format'],'gzip-newc')
    def test_verified_stable_rescue_and_independent_next(self):
        self.modify(self.stable,hardware_verified=True,status='HARDWARE_VERIFIED')
        result=boot.prepare(self.args());self.assertEqual(result['default'],'piano-stable')
        self.assertTrue(result['profiles']['stable']['rescue_eligible'])
        self.assertFalse(result['profiles']['next']['hardware_verified'])
    def test_missing_dtb_is_not_replaced(self):
        self.modify(self.stable,dtb=None)
        with self.assertRaisesRegex(ValueError,'DTB absent'):boot.prepare(self.args())
    def test_configured_is_not_built(self):
        self.modify(self.stable,status='CONFIGURED_NOT_BUILT')
        with self.assertRaisesRegex(ValueError,'not built'):boot.prepare(self.args())
    def test_stale_source_and_config_hash_rejected(self):
        self.modify(self.stable,source_commit='0'*40)
        with self.assertRaisesRegex(ValueError,'stale'):boot.prepare(self.args())
        self.modify(self.stable,source_commit=self.lock['profiles']['stable']['commit'],config_sha256='0'*64)
        with self.assertRaisesRegex(ValueError,'config hash'):boot.prepare(self.args())
    def test_stale_fragment_and_tampered_image_rejected(self):
        self.modify(self.stable,fragment_sha256='0'*64)
        with self.assertRaisesRegex(ValueError,'fragment hash'):boot.prepare(self.args())
        self.modify(self.stable,fragment_sha256=boot.digest(boot.ROOT/'linux/configs/piano-ram.config'))
        image=self.stable.parent/'Image';data=bytearray(image.read_bytes());data[-1]=7;image.write_bytes(data)
        with self.assertRaisesRegex(ValueError,'Image hash'):boot.prepare(self.args())
    def test_truncated_dtb_and_initramfs_rejected(self):
        path=self.root/'bad.dtb';path.write_bytes(fake_fdt()[:-1])
        with self.assertRaisesRegex(ValueError,'incomplete'):boot.fdt_info(path)
        path=self.root/'bad.cpio';path.write_bytes(fake_cpio()[:115])
        with self.assertRaises(ValueError):boot.initramfs_info(path)
    def test_canonical_patch_list_rejected(self):
        self.modify(self.stable,canonical_patch_commits=['0'*40])
        with self.assertRaisesRegex(ValueError,'canonical patch'):boot.prepare(self.args())
    def test_plan_has_no_fabricated_images_and_tamper_is_detected(self):
        result=boot.prepare(self.args(plan_only=True));self.assertEqual(result['profiles']['stable']['status'],'NOT_STAGED')
        self.assertFalse((self.root/'tree/EFI/Piano/Stable/Image.efi').exists())
        boot.check_tree(self.root/'tree')
        (self.root/'tree/simpleinit.static.uefi.json').write_text('{}')
        with self.assertRaises((KeyError,ValueError)):boot.check_tree(self.root/'tree')
    def test_preserve_existing_tree(self):
        (self.root/'tree').mkdir();marker=self.root/'tree/keep';marker.write_text('baseline')
        with self.assertRaisesRegex(ValueError,'refusing'):boot.prepare(self.args())
        self.assertEqual(marker.read_text(),'baseline')

if __name__=='__main__':unittest.main()
