"""Pure args/transforms and pinned FAT header tests, never prepare/main/device."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest import mock
ROOT=Path(__file__).resolve().parents[1]
def load(name,path):
    s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
profile=load('profile',ROOT/'tools/prepare_gui_profile.py')
fmt=load('fmt',ROOT/'tools/prepare_ufs_bounded_fs_test.py')
class ProfileTests(unittest.TestCase):
    def test_default_and_implies_blockio(self):
        p=profile.argument_parser();a=p.parse_args([]);self.assertFalse(a.ufs_bounded_filesystem_test)
        a=p.parse_args(['--ufs-bounded-filesystem-test']);profile.validate_write_options(p,a);self.assertTrue(a.ufs_blockio);self.assertFalse(a.ufs_filesystems)
    def test_all_conflicts_and_usb_screenshot(self):
        for option in ('--ufs-write-preflight','--ufs-write-restore-test','--ufs-shell','--ufs-filesystems','--ufs-setup','--usb-controller','--usb-ep0','--usb-fastboot','--usb-screenshot','--touch-probe','--usb-debug'):
            with self.subTest(option=option),contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit):
                p=profile.argument_parser();a=p.parse_args(['--ufs-bounded-filesystem-test',option]);profile.validate_write_options(p,a)
    def test_transform_only_and_no_os(self):
        src=(ROOT/'bootprofiles/uefi-app/RamApp.c').read_text().replace('  Status=gBS->LoadImage','  PianoProbeUfs(Fdt);\n  Status=gBS->LoadImage',1)
        out=profile.bounded_fs_ram_app(src)
        self.assertIn('PianoUfsRunBoundedFileSystemTest();',out)
        self.assertLess(out.index('PianoProbeUfs(Fdt);'),out.index('Status=PianoUfsRunBoundedFileSystemTest();'))
        for symbol in ('LoadImage','StartImage','UnloadImage'):self.assertNotIn(symbol,out)
        self.assertIn('while(TRUE){gBS->Stall(100000);}',out)
class ImageTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.folder=Path(self.tmp.name)/'image';shutil.copytree(ROOT/'artifacts/ufs/test-window',self.folder)
    def tearDown(self):self.tmp.cleanup()
    def test_valid_header_temp_only(self):
        blocks=fmt.verify(self.folder);self.assertEqual(len(blocks),4)
        out=Path(self.tmp.name)/fmt.HEADER;fmt.prepare(out,self.folder)
        text=out.read_text();self.assertIn('mPianoFatLbas[4]={0,1,3,5}',text);self.assertIn('PC_VERIFIED TRUE',text)
    def test_image_size_and_sha_fail(self):
        image=self.folder/'fat12.img';image.write_bytes(image.read_bytes()[:-1])
        with self.assertRaisesRegex(ValueError,'bytes/SHA'):fmt.verify(self.folder)
    def test_nonzero_other_sector_refused(self):
        image=self.folder/'fat12.img';data=bytearray(image.read_bytes());data[20*4096]=1;image.write_bytes(data)
        with self.assertRaises(ValueError):fmt.verify(self.folder)
    def test_manifest_typed_geometry_and_flag(self):
        manifest=self.folder/'manifest.json';value=json.loads(manifest.read_text());value['device_operations']=0;manifest.write_text(json.dumps(value))
        with self.assertRaises(ValueError):fmt.verify(self.folder)
    def test_manifest_wrong_extents(self):
        manifest=self.folder/'manifest.json';value=json.loads(manifest.read_text());value['geometry']['nonzero_blocks'][0]['logical_lba']=False;manifest.write_text(json.dumps(value))
        with self.assertRaises(ValueError):fmt.verify(self.folder)
    def test_source_symlink_refused(self):
        image=self.folder/'fat12.img';target=Path(self.tmp.name)/'fat.img';image.rename(target);image.symlink_to(target)
        with self.assertRaises(ValueError):fmt.verify(self.folder)
    def test_fresh_archive_is_required(self):
        archive=fmt._archive();archive.verify_capture=mock.Mock(side_effect=ValueError('archive drift'))
        with mock.patch.object(fmt,'_archive',return_value=archive),self.assertRaisesRegex(ValueError,'archive drift'):fmt.verify(self.folder)
if __name__=='__main__':unittest.main()
