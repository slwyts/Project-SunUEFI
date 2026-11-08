"""Real full public config and source/module identity checks; no rebuild/device."""
from pathlib import Path
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import build_piano_full_kernel as full
# Inspect the pinned configuration, not the moving reference submodule HEAD.
MANIFEST=ROOT/'artifacts/kernels/release/manifest.json'
RECORD=json.loads(MANIFEST.read_text()) if MANIFEST.exists() else {}
KERNEL=Path(RECORD.get('prepared_source_worktree',full.WORK))
CONFIG=ROOT/'artifacts/kernels/release/config'
CONFIGURED='needs the release kernel build (./build.sh linux)'


class PianoFullKernelTests(unittest.TestCase):
    def setUp(self):
        if not (KERNEL/'arch/arm64/configs/piano_defconfig').exists():
            self.skipTest('pinned kernel source not fetched (./build.sh sources)')

    def test_uhid_fragment_is_the_only_local_module_override(self):
        fragment=(ROOT/'linux/configs/piano-bluetooth.config').read_text()
        modules={**RECORD.get('local_module_overrides',{}),**full.module_overrides(fragment)}
        public=(KERNEL/'arch/arm64/configs/piano_rootfs.config').read_text()
        command=full.command_line((ROOT/'linux/configs/piano-full.config').read_text(),public,RECORD.get('root_policy','ram'))
        if not CONFIG.exists():self.skipTest(CONFIGURED)
        config=CONFIG.read_text()
        values=full.config_values(config)
        old='CONFIG_UHID='+values['CONFIG_UHID']if values['CONFIG_UHID']!='n'else '# CONFIG_UHID is not set'
        updated=config.replace(old,'CONFIG_UHID=m')
        requirements=full.validate_config(updated,public,command,modules)
        self.assertEqual(requirements['UHID'],'m')
        self.assertEqual(full.config_values(full.effective_config(command,modules))['CONFIG_UHID'],'m')
        with self.assertRaisesRegex(ValueError,'module requirement'):
            full.validate_config(updated.replace('CONFIG_UHID=m','# CONFIG_UHID is not set'),public,command,modules)
        for bad in ('CONFIG_UHID=y\n','CONFIG_UHID=n\n','CONFIG_UHID=m\nCONFIG_BT_LE=y\n','CONFIG_UHID=m\nCONFIG_UHID=n\n'):
            with self.subTest(fragment=bad),self.assertRaises(ValueError):full.module_overrides(bad)
        with self.assertRaises(ValueError):full.validate_config(updated,public,command,{'CONFIG_BT_LE':'y'})
        for name,value in full.config_values(public).items():
            if name=='CONFIG_CMDLINE':continue
            value=modules.get(name,value)
            changed=updated.replace(name+'='+value,name+'='+('n'if value!='n'else 'y'))
            if changed!=updated:
                with self.subTest(symbol=name),self.assertRaises(ValueError):full.validate_config(changed,public,command,modules)

    def test_filesystem_fragment_matches_overrides(self):
        fragment=(ROOT/'linux/configs/piano-filesystem.config').read_text()
        modules=full.module_overrides(fragment,'filesystem')
        for symbol in ('CONFIG_EXFAT_FS','CONFIG_NTFS3_FS','CONFIG_NTFS3_LZX_XPRESS','CONFIG_NLS_UTF8'):
            self.assertEqual(modules[symbol],'y')
        self.assertEqual(len(modules),len(full.FILESYSTEM_OVERRIDES))
        for bad in ('CONFIG_EXFAT_FS=m\n',
                    'CONFIG_EXFAT_FS=y\nCONFIG_NTFS3_FS=y\nCONFIG_NTFS3_LZX_XPRESS=y\nCONFIG_NLS_UTF8=m\n'):
            with self.subTest(fragment=bad),self.assertRaises(ValueError):full.module_overrides(bad,'filesystem')
        for key,value in modules.items():
            self.assertEqual(full.ALLOWED_OVERRIDES[key],value)
            self.assertEqual(full.REQUIRED[key.removeprefix('CONFIG_')],value)

    def test_filesystem_and_bluetooth_in_allowed_overrides(self):
        for key,value in full.FILESYSTEM_OVERRIDES.items():
            self.assertIn(key,full.ALLOWED_OVERRIDES)
            self.assertEqual(full.ALLOWED_OVERRIDES[key],value)
        for key,value in full.BLUETOOTH_OVERRIDES.items():
            self.assertIn(key,full.ALLOWED_OVERRIDES)
            self.assertEqual(full.ALLOWED_OVERRIDES[key],value)

    def test_real_generated_full_config_preserves_public_profile(self):
        path=CONFIG
        if not path.exists():self.skipTest(CONFIGURED)
        full.verify_source(KERNEL,RECORD['source_commit']);public=(KERNEL/'arch/arm64/configs/piano_rootfs.config').read_text()
        command=full.command_line((ROOT/'linux/configs/piano-full.config').read_text(),public,RECORD['root_policy'])
        config=path.read_text();modules=RECORD['local_module_overrides'];requirements=full.validate_config(config,public,command,modules)
        self.assertEqual(requirements['VIDEO_QCOM_CAMSS'],'m');self.assertEqual(requirements['DRM_MSM'],'m');self.assertEqual(requirements['SND_SOC_SC8280XP'],'m')
        for symbol in ('VIDEO_OV32D40','HID_NANOSIC_WN8030','BATTERY_PIANO_MCA','CHARGER_SC8541','QCOM_Q6V5_PAS'):
            with self.subTest(symbol=symbol),self.assertRaises(ValueError):full.validate_config(config.replace('CONFIG_'+symbol+'=m','# CONFIG_'+symbol+' is not set'),public,command,modules)

    def test_root_policy_is_only_public_commandline_substitution(self):
        public=(KERNEL/'arch/arm64/configs/piano_rootfs.config').read_text();fragment=(ROOT/'linux/configs/piano-full.config').read_text()
        command=full.command_line(fragment,public,'LABEL=PIANO_EXTERNAL')
        self.assertIn('piano.root=LABEL=PIANO_EXTERNAL',command);self.assertNotIn('root=PARTLABEL=userdata',command)
        disk=full.command_line(fragment,public,'PARTUUID=ffc480ed-c219-400b-a8f9-5f6805aa1f34')
        self.assertIn('piano.root=PARTUUID=ffc480ed-c219-400b-a8f9-5f6805aa1f34',disk)
        for flag in ('rdinit=/pianoinit','pd_ignore_unused','clk_ignore_unused','iommu.passthrough=1'):self.assertIn(flag,command)
        for bad in ('PARTLABEL=userdata','/dev/sda','ram root=PARTLABEL=userdata','LABEL=userdata'):
            with self.subTest(root=bad),self.assertRaises(ValueError):full.command_line(fragment,public,bad)
        for bad in (fragment+'\nCONFIG_DRM_MSM=n\n',fragment.replace('iommu.passthrough=1','iommu.passthrough=0')):
            with self.assertRaises(ValueError):full.command_line(bad,public,'ram')

    def test_real_git_clean_head_and_independent_public_hash_guards(self):
        with tempfile.TemporaryDirectory(prefix='full-source-')as directory:
            work=Path(directory)
            for name in full.SOURCE_PINS:
                path=work/name;path.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(KERNEL/name,path)
            def git(*args):return subprocess.check_output(['git','-C',str(work),*args],stderr=subprocess.STDOUT,text=True).strip()
            git('init','-q');git('add','arch');git('-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','-qm','fixture')
            commit=git('rev-parse','HEAD')
            with patch.object(full,'COMMIT',commit):
                full.verify_source(work);path=work/'untracked';path.write_text('drift')
                with self.assertRaisesRegex(ValueError,'dirty'):full.verify_source(work)
                path.unlink();name=next(iter(full.SOURCE_PINS));git('update-index','--assume-unchanged',name)
                (work/name).write_text((work/name).read_text()+'\n# hidden drift\n');self.assertFalse(git('status','--porcelain'))
                with self.assertRaisesRegex(ValueError,'config changed'):full.verify_source(work)
                shutil.copyfile(KERNEL/name,work/name);git('update-index','--no-assume-unchanged',name)
                (work/'new').write_text('new');git('add','new');git('-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','-qm','new head')
                with self.assertRaisesRegex(ValueError,'HEAD drifted'):full.verify_source(work)
                descendant=git('rev-parse','HEAD');full.verify_source(work,descendant)
                with tempfile.TemporaryDirectory(prefix='full-release-policy-')as policy_dir:
                    policy_root=Path(policy_dir);(policy_root/'config').mkdir()
                    (policy_root/'config/release.json').write_text(json.dumps({'kernel':{
                        'base_commit':commit,'target_tree':git('rev-parse','HEAD^{tree}'),
                        'patch_series':'config/series.json'}}))
                    (policy_root/'config/series.json').write_text(json.dumps({'original_target_commit':descendant}))
                    (work/'new').write_text('release derivative');git('add','new')
                    git('-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','-qm','release derivative')
                    with patch.object(full,'ROOT',policy_root),patch.object(full,'COMMIT','f'*40):
                        self.assertEqual(full.verify_source(work,git('rev-parse','HEAD')),commit)
                disconnected=git('-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit-tree','HEAD^{tree}','-m','disconnected')
                git('checkout','--detach',disconnected)
                with self.assertRaisesRegex(ValueError,'does not descend'):full.verify_source(work,disconnected)

    def test_real_module_vermagic_hash_and_duplicate_rejection(self):
        source=ROOT/'artifacts/kernels/stable/userspace-debug/modules'
        modules=list(source.rglob('*.ko'))
        if not modules:self.skipTest('Existing actual ARM64 modules unavailable')
        module=modules[0];release=full.modinfo(module)['vermagic'].split()[0]
        with tempfile.TemporaryDirectory(prefix='full-modules-')as directory:
            folder=Path(directory);library=folder/'lib/modules'/release;kernel=library/'kernel';kernel.mkdir(parents=True)
            shutil.copyfile(module,kernel/module.name);(library/'modules.dep').write_text('kernel/'+module.name+':\n')
            rows,summary=full.seal_modules(folder,release);self.assertEqual(summary['count'],1);self.assertEqual(rows[0]['sha256'],full.sha(module));self.assertEqual(rows[0]['vermagic'],full.modinfo(module)['vermagic'])
            with self.assertRaisesRegex(ValueError,'release mismatch'):full.seal_modules(folder,'wrong-release')
            shutil.copyfile(module,kernel/('duplicate-'+module.name))
            with self.assertRaisesRegex(ValueError,'duplicate'):full.seal_modules(folder,release)


if __name__=='__main__':unittest.main()
