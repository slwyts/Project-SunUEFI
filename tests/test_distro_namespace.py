"""Actual private binfmt GNU execution and original-base protection, no device."""
from pathlib import Path
import importlib.util
import os
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import run_distro_userland as distro


class DistroNamespaceTests(unittest.TestCase):
    def paths(self):
        rootfs=ROOT/'artifacts/distros/debian13-arm64-v1/rootfs';tools=ROOT/'build/distro-tools'
        if not(rootfs/'usr/bin/bash').exists()or not(tools/'manifest.json').exists():self.skipTest('Official guest/helper inputs not present')
        return rootfs,tools

    def test_base_immutable_and_old_proot_explicit(self):
        rootfs,tools=self.paths()
        with self.assertRaisesRegex(ValueError,'base is immutable'):distro.build_command(rootfs,tools,['/usr/bin/true'],True,'namespace')
        old=distro.build_command(rootfs,tools,['/usr/bin/true'],False,'proot')
        self.assertIn('/opt/usr/bin/proot',old);self.assertNotIn('unshare',old)

    def test_private_namespace_real_arm_fork_no_global_binfmt_change(self):
        rootfs,tools=self.paths();host=Path('/proc/sys/fs/binfmt_misc');before=sorted(x.name for x in host.iterdir())if host.exists()else None
        with tempfile.TemporaryDirectory(prefix='distro-ns-',dir=ROOT/'build')as directory:
            log=Path(directory)/'probe.log'
            result=distro.run(rootfs,tools,['/usr/bin/bash','-c','set -eu; dpkg --print-architecture; getconf LONG_BIT; /bin/bash -c "printf native-child-ok"'],log,False,timeout=30,backend='namespace')
            self.assertEqual(result['exit_code'],0);self.assertTrue(result['private_binfmt']);self.assertFalse(result['host_binfmt_changed']);self.assertFalse(result['pid1_boot_verified'])
            content=log.read_text();self.assertIn('arm64',content);self.assertIn('64',content);self.assertIn('native-child-ok',content);self.assertIn('flags: F',content)
            self.assertNotIn('proot:',content);self.assertEqual(result['execution_provenance']['runner_sha256'],distro.hashlib.sha256(Path(distro.__file__).read_bytes()).hexdigest())
        after=sorted(x.name for x in host.iterdir())if host.exists()else None;self.assertEqual(before,after)


if __name__=='__main__':unittest.main()
