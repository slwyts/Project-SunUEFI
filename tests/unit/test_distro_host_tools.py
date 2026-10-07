"""Pinned metadata and real bsdtar narrow .deb extraction; no guest or network."""
from pathlib import Path
import gzip
import hashlib
import importlib.util
import io
import json
import os
import tarfile
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('distro_tools',ROOT/'tools/prepare_distro_host_tools.py')
tools=importlib.util.module_from_spec(spec);spec.loader.exec_module(tools)


def elf(machine=62):
    data=bytearray(80);data[:6]=b'\x7fELF\x02\x01';data[18:20]=machine.to_bytes(2,'little');return bytes(data)


def ar_member(name,data):
    header=f'{name+"/":<16}{0:<12}{0:<6}{0:<6}{100644:<8}{len(data):<10}`\n'.encode()
    assert len(header)==60;return header+data+(b'\n'if len(data)%2 else b'')


def fixture(path,entries):
    memory=io.BytesIO()
    with tarfile.open(fileobj=memory,mode='w')as archive:
        for name,kind,data in entries:
            info=tarfile.TarInfo(name)
            if kind=='file':info.size=len(data);archive.addfile(info,io.BytesIO(data))
            else:info.type=tarfile.SYMTYPE;info.linkname=data;archive.addfile(info)
    data=b'!<arch>\n'+ar_member('debian-binary',b'2.0\n')+ar_member('data.tar.gz',gzip.compress(memory.getvalue()))
    path.write_bytes(data);return {'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),'files':('usr/bin/qemu-aarch64',)}


class DistroHostToolsTests(unittest.TestCase):
    def test_narrow_real_deb_extraction_and_hash(self):
        with tempfile.TemporaryDirectory()as directory:
            root=Path(directory);package=root/'fixture.deb';output=root/'output'
            pin=fixture(package,[('./usr/bin/qemu-aarch64','file',elf()),('./usr/bin/other','file',elf()),('./DEBIAN/postinst','file',b'exit 99')])
            result=tools.extract_needed(package,pin,output)
            self.assertEqual(list(result),['usr/bin/qemu-aarch64']);self.assertEqual((output/'usr/bin/qemu-aarch64').read_bytes(),elf())
            self.assertFalse((output/'usr/bin/other').exists());self.assertFalse((output/'DEBIAN').exists())
            package.write_bytes(package.read_bytes()+b'corrupt')
            with self.assertRaisesRegex(ValueError,'size/SHA'):tools.extract_needed(package,pin,root/'second')

    def test_bounded_selected_library_symlink(self):
        with tempfile.TemporaryDirectory()as directory:
            root=Path(directory);package=root/'fixture.deb';output=root/'output'
            pin=fixture(package,[('./usr/lib/libtalloc.so.2','link','libtalloc.so.2.4.3'),('./usr/lib/libtalloc.so.2.4.3','file',elf())]);pin['files']=('usr/lib/libtalloc.so.2','usr/lib/libtalloc.so.2.4.3')
            result=tools.extract_needed(package,pin,output)
            self.assertEqual(result['usr/lib/libtalloc.so.2']['symlink'],'libtalloc.so.2.4.3');self.assertEqual((output/'usr/lib/libtalloc.so.2').read_bytes(),elf())

    def test_refuses_traversal_duplicate_wrong_arch_and_unbounded_link(self):
        cases=[('../escape','file',elf()),('./usr/bin/qemu-aarch64','file',elf(183)),('./usr/bin/qemu-aarch64','link','/usr/bin/true'),('./usr/bin/qemu-aarch64','link','../../escape')]
        for entry in cases:
            with self.subTest(entry=entry),tempfile.TemporaryDirectory()as directory:
                root=Path(directory);package=root/'fixture.deb';pin=fixture(package,[entry])
                with self.assertRaises(ValueError):tools.extract_needed(package,pin,root/'output')
                self.assertFalse((root/'escape').exists())
        with tempfile.TemporaryDirectory()as directory:
            root=Path(directory);package=root/'fixture.deb';pin=fixture(package,[('./usr/bin/qemu-aarch64','file',elf())]*2)
            with self.assertRaisesRegex(ValueError,'Duplicate'):tools.extract_needed(package,pin,root/'output')

    def test_download_page_requires_exact_version_size_and_digest(self):
        pin=tools.PINS['proot'];text=f'Download Page for {pin["filename"]} <table>Exact Size <b>{pin["bytes"]}</b> Byte SHA256 checksum <code>{pin["sha256"]}</code></table>'.encode()
        tools.verify_page(text,pin)
        for bad in (text.replace(str(pin['bytes']).encode(),b'1'),text.replace(pin['sha256'].encode(),b'0'*64),text.replace(pin['filename'].encode(),b'wrong.deb')):
            with self.assertRaisesRegex(ValueError,'metadata changed'):tools.verify_page(bad,pin)

    def test_actual_workspace_host_helpers_when_prepared(self):
        folder=ROOT/'build/distro-tools';marker=folder/'manifest.json'
        if not marker.exists():self.skipTest('Workspace Debian helpers not downloaded')
        original=os.environ.get('LD_LIBRARY_PATH');manifest=json.loads(marker.read_text());result=tools.inspect(folder,manifest)
        self.assertIn('qemu-aarch64 version 10.0.13',result['versions']['qemu']);self.assertIn('5.1.0',result['versions']['proot'])
        self.assertEqual(result['proot_help_exit'],0);self.assertFalse(result['arm_guest_executed'])
        self.assertEqual(result['launch']['proot'][1],'--library-path');self.assertEqual(os.environ.get('LD_LIBRARY_PATH'),original)


if __name__=='__main__':unittest.main()
