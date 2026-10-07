"""Actual overlay preservation and RAM-entry block-root refusal, no device."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import stage_piano_full_userspace as stage


class PianoFullUserspaceTests(unittest.TestCase):
    def test_complete_real_overlay_keeps_hardware_services_and_replaces_only_root_policy(self):
        source=ROOT/'upstream/debian-piano-current'
        if not source.is_dir():self.skipTest('Pinned source not fetched')
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='full-user-test-')as temporary:
            target=Path(temporary)/'rootfs';target.mkdir()
            result=stage.stage(target,source)
            for name in ('display-start','touch-start','keyboard-start','radio-start','adsp-start','audio-start','video-start','camera-start'):
                relative=Path('usr/lib/piano')/name
                self.assertEqual((target/relative).read_bytes(),(source/'rootfs/overlay'/relative).read_bytes())
            for path in (source/'rootfs/overlay/etc/systemd/system').glob('piano-*.service'):
                if path.name=='piano-swapfile.service':continue
                self.assertEqual((target/'etc/systemd/system'/path.name).read_bytes(),path.read_bytes())
            camerad=Path('usr/lib/systemd/system/piano-camerad.service')
            self.assertEqual((target/camerad).read_bytes(),(source/'rootfs/overlay'/camerad).read_bytes())
            self.assertNotIn('userdata',(target/'etc/fstab').read_text())
            self.assertEqual(os.readlink(target/'etc/systemd/system/piano-swapfile.service'),'/dev/null')
            self.assertEqual(os.readlink(target/'etc/systemd/system/qbootctl.service'),'/dev/null')
            self.assertIn('UDISKS_IGNORE',(target/'etc/udev/rules.d/01-piano-protect-android.rules').read_text())
            self.assertFalse(result['piano_boot_verified'])
            subprocess.run(['bash','-n',str(target/'pianoinit')],check=True)

    def test_actual_entry_refuses_android_root_before_any_mount(self):
        script=ROOT/'linux/userspace/pianoinit'
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='ram-entry-test-')as temporary:
            path=Path(temporary);(path/'proc').mkdir();(path/'usr').mkdir()
            (path/'bin').symlink_to('usr/bin');(path/'lib').symlink_to('usr/lib');(path/'lib64').symlink_to('usr/lib')
            (path/'pianoinit').write_bytes(script.read_bytes());(path/'pianoinit').chmod(0o755)
            (path/'proc/cmdline').write_text('root=PARTLABEL=userdata piano.root=ram\n')
            result=subprocess.run(['bwrap','--unshare-user','--unshare-pid','--die-with-parent',
                '--ro-bind',str(path),'/','--ro-bind','/usr','/usr','--','/bin/bash','/pianoinit'],
                capture_output=True,text=True)
            self.assertEqual(result.returncode,1,result.stdout+result.stderr)
            self.assertIn('Android block-root arguments are forbidden',result.stdout)
            self.assertFalse((path/'run').exists())


if __name__=='__main__':unittest.main()
