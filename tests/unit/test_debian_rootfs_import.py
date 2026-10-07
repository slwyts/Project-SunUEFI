"""Actual content/guest-path checks; no network, execution or device."""
import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'tools'))
import prepare_debian_rootfs as importer


class DebianRootfsImportTests(unittest.TestCase):
    def archive(self, path, records):
        with tarfile.open(path, 'w') as archive:
            for name, kind, data in records:
                info=tarfile.TarInfo(name);info.mode=0o755
                if kind=='file':
                    info.size=len(data);archive.addfile(info,io.BytesIO(data))
                else:
                    info.type=tarfile.SYMTYPE if kind=='link' else tarfile.LNKTYPE
                    info.linkname=data;archive.addfile(info)

    def test_exact_digest_refuses_mutation_and_bad_digest(self):
        data=b'actual bytes';expected=importer.sha(data)
        self.assertEqual(importer.checked(data,expected),data)
        with self.assertRaisesRegex(ValueError,'mismatch'):
            importer.checked(data+b'!',expected)
        with self.assertRaises(ValueError):
            importer.checked(data,'sha256:../outside')

    def test_guest_absolute_symlinks_are_deferred_and_never_followed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);archive=root/'image.tar';target=root/'rootfs'
            self.archive(archive,[('bin','link','/usr/bin'),('usr/bin/sh','file',b'ARM ELF fixture'),
                ('usr/bin/alias','hard','usr/bin/sh')])
            self.assertEqual(importer.extract_layer(archive,target),15)
            self.assertEqual((target/'bin').readlink(),Path('/usr/bin'))
            self.assertEqual((target/'usr/bin/alias').read_bytes(),b'ARM ELF fixture')
            first,count=importer.tree_digest(target)
            self.assertGreater(count,2)
            (target/'usr/bin/sh').write_bytes(b'changed')
            self.assertNotEqual(importer.tree_digest(target)[0],first)

    def test_escape_whiteout_and_special_node_are_refused(self):
        for member in ('../outside','/outside','usr/../../outside','usr/.wh.deleted'):
            with self.subTest(member=member),tempfile.TemporaryDirectory() as temporary:
                root=Path(temporary);archive=root/'bad.tar'
                self.archive(archive,[(member,'file',b'bad')])
                with self.assertRaises(ValueError):
                    importer.extract_layer(archive,root/'rootfs')
                self.assertFalse((root/'outside').exists())
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);archive=root/'node.tar'
            with tarfile.open(archive,'w') as stream:
                info=tarfile.TarInfo('dev/block');info.type=tarfile.BLKTYPE;stream.addfile(info)
            with self.assertRaisesRegex(ValueError,'Special node'):
                importer.extract_layer(archive,root/'rootfs')

    def test_existing_symlink_parent_and_external_hardlink_are_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            base=Path(temporary);outside=base/'outside';outside.mkdir()
            root=base/'rootfs';root.mkdir();(root/'usr').symlink_to(outside)
            with self.assertRaises(ValueError):
                importer.parents(root,importer.name('usr/file'))
            self.assertFalse((outside/'file').exists())
            archive=base/'hard.tar'
            self.archive(archive,[('bad','hard','../outside/file')])
            with self.assertRaises(ValueError):
                importer.extract_layer(archive,base/'second-root')


if __name__=='__main__':unittest.main()
