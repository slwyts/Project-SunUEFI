"""Real full public config and source/module identity checks; no rebuild/device."""
from pathlib import Path
import importlib.util
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import build_piano_full_kernel as full


class PianoFullKernelTests(unittest.TestCase):
    def test_uhid_fragment_is_the_only_local_module_override(self):
        fragment=(ROOT/'linux/configs/piano-bluetooth.config').read_text()
        modules=full.module_overrides(fragment)
        public=(full.WORK/'arch/arm64/configs/piano_rootfs.config').read_text()
        command=full.command_line((ROOT/'linux/configs/piano-full.config').read_text(),public,'ram')
        config=(full.OUT/'.config').read_text()
        values=full.config_values(config)
        old='CONFIG_UHID='+values['CONFIG_UHID']if values['CONFIG_UHID']!='n'else '# CONFIG_UHID is not set'
        updated=config.replace(old,'CONFIG_UHID=m')
        requirements=full.validate_config(updated,public,command,modules)
        self.assertEqual(requirements['UHID'],'m')
        self.assertEqual(full.config_values(full.effective_config(command,modules))['CONFIG_UHID'],'m')
        with self.assertRaisesRegex(ValueError,'module requirement'):
            full.validate_config(config,public,command,modules)
        for bad in ('CONFIG_UHID=y\n','CONFIG_UHID=n\n','CONFIG_UHID=m\nCONFIG_BT_LE=y\n','CONFIG_UHID=m\nCONFIG_UHID=n\n'):
            with self.subTest(fragment=bad),self.assertRaises(ValueError):full.module_overrides(bad)
        with self.assertRaises(ValueError):full.validate_config(updated,public,command,{'CONFIG_BT_LE':'y'})
        for name,value in full.config_values(public).items():
            if name=='CONFIG_CMDLINE':continue
            changed=updated.replace(name+'='+value,name+'='+('n'if value!='n'else 'y'))
            if changed!=updated:
                with self.subTest(symbol=name),self.assertRaises(ValueError):full.validate_config(changed,public,command,modules)

    def test_real_generated_full_config_preserves_public_profile(self):
        full.verify_source();public=(full.WORK/'arch/arm64/configs/piano_rootfs.config').read_text()
        command=full.command_line((ROOT/'linux/configs/piano-full.config').read_text(),public,'ram')
        path=full.OUT/'.config'
        if not path.exists():self.skipTest('Complete candidate not yet configured')
        config=path.read_text();requirements=full.validate_config(config,public,command)
        self.assertEqual(requirements['VIDEO_QCOM_CAMSS'],'m');self.assertEqual(requirements['DRM_MSM'],'m');self.assertEqual(requirements['SND_SOC_SC8280XP'],'m')
        for symbol in ('VIDEO_OV32D40','HID_NANOSIC_WN8030','BATTERY_PIANO_MCA','CHARGER_SC8541','QCOM_Q6V5_PAS'):
            with self.subTest(symbol=symbol),self.assertRaises(ValueError):full.validate_config(config.replace('CONFIG_'+symbol+'=m','# CONFIG_'+symbol+' is not set'),public,command)

    def test_root_policy_is_only_public_commandline_substitution(self):
        public=(full.WORK/'arch/arm64/configs/piano_rootfs.config').read_text();fragment=(ROOT/'linux/configs/piano-full.config').read_text()
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
                path=work/name;path.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(full.WORK/name,path)
            def git(*args):return subprocess.check_output(['git','-C',str(work),*args],stderr=subprocess.STDOUT,text=True).strip()
            git('init','-q');git('add','arch');git('-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','-qm','fixture')
            commit=git('rev-parse','HEAD')
            with patch.object(full,'COMMIT',commit):
                full.verify_source(work);path=work/'untracked';path.write_text('drift')
                with self.assertRaisesRegex(ValueError,'dirty'):full.verify_source(work)
                path.unlink();name=next(iter(full.SOURCE_PINS));git('update-index','--assume-unchanged',name)
                (work/name).write_text((work/name).read_text()+'\n# hidden drift\n');self.assertFalse(git('status','--porcelain'))
                with self.assertRaisesRegex(ValueError,'config changed'):full.verify_source(work)
                shutil.copyfile(full.WORK/name,work/name);git('update-index','--no-assume-unchanged',name)
                (work/'new').write_text('new');git('add','new');git('-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','-qm','new head')
                with self.assertRaisesRegex(ValueError,'HEAD drifted'):full.verify_source(work)
                descendant=git('rev-parse','HEAD');full.verify_source(work,descendant)
                disconnected=git('-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit-tree','HEAD^{tree}','-m','disconnected')
                git('checkout','--detach',disconnected)
                with self.assertRaisesRegex(ValueError,'must descend'):full.verify_source(work,disconnected)

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
