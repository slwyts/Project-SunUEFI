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


class KernelPolicyCacheTests(unittest.TestCase):
    @staticmethod
    def bind_release(record, output):
        suffix=full.kernel_abi_suffix(record['kernel_policy']['abi_epoch'],record['source_tree'],
                                      record['config_sha256'],record['inputs'],record['toolchain']['sha256'])
        record['kernel_release']='7.3.3-piano'+suffix
        (output/'include/config/kernel.release').write_text(record['kernel_release']+'\n')

    def test_non_abi_config_changes_keep_the_reuse_decision_and_outputs(self):
        with tempfile.TemporaryDirectory(prefix='kernel-policy-') as directory:
            root=Path(directory);(root/'config').mkdir()
            config=root/'config/release.json'
            release=json.loads((ROOT/'config/release.json').read_text())
            config.write_text(json.dumps(release,indent=2)+'\n')
            policy=full.kernel_policy_identity(root)
            output=root/'kernel-build';(output/'include/config').mkdir(parents=True)
            (output/'.config').write_text('CONFIG_MODULES=y\n')
            (output/'Module.symvers').write_text('0x12345678\tmodule_symbol\tvmlinux\tEXPORT_SYMBOL\n')
            inputs={'tools/kernel.py':'producer-sha256'}
            tools={'sha256':{'clang':'compiler-sha256'}}
            record={'status':'HOST_BUILT_FULL_CANDIDATE_NOT_HARDWARE_VERIFIED',
                    'source_commit':'a'*40,'source_tree':'b'*40,'inputs':inputs,'kernel_policy':policy['policy'],
                    'kernel_policy_sha256':policy['sha256'],'local_module_overrides':{'CONFIG_UHID':'m'},
                    'root_policy':'LABEL=PIANOROOT','command_line':'rdinit=/pianoinit piano.root=LABEL=PIANOROOT',
                    'toolchain':tools,'config_sha256':full.sha(output/'.config'),
                    'module_symvers_sha256':full.sha(output/'Module.symvers')}
            self.bind_release(record, output)
            marker=output/'manifest.json';marker.write_text(json.dumps(record,indent=2)+'\n')
            before={path: (path.read_bytes(),path.stat().st_mtime_ns)
                    for path in output.rglob('*') if path.is_file()}

            def reusable():
                return full.matching_build(record,'a'*40,inputs,full.kernel_policy_identity(root),
                                           {'CONFIG_UHID':'m'},'LABEL=PIANOROOT',record['command_line'],tools,output)

            self.assertTrue(reusable())
            release['login']['autologin']=not release['login']['autologin']
            release['sensors']['required']=not release['sensors']['required']
            release['mesa']={'version':'27.0.0'}
            release['kernel']['package_epoch']+=1
            config.write_text(json.dumps(release,sort_keys=True,separators=(',',':')))
            self.assertEqual(full.kernel_policy_identity(root),policy)
            self.assertTrue(reusable())
            self.assertEqual({path: (path.read_bytes(),path.stat().st_mtime_ns) for path in before},before)

            release['kernel']['abi_epoch']+=1
            config.write_text(json.dumps(release))
            self.assertFalse(reusable())
            release['kernel']['abi_epoch']-=1
            release['kernel']['command_line']+=' panic_on_warn=1'
            config.write_text(json.dumps(release))
            self.assertFalse(reusable())
            self.assertEqual({path: (path.read_bytes(),path.stat().st_mtime_ns) for path in before},before)

    def test_reuse_still_rejects_source_config_tools_and_symbol_changes(self):
        with tempfile.TemporaryDirectory(prefix='kernel-reuse-') as directory:
            output=Path(directory);(output/'include/config').mkdir(parents=True)
            (output/'.config').write_text('CONFIG_MODULES=y\n')
            (output/'Module.symvers').write_text('kernel symbols\n')
            policy=full.kernel_policy_identity(ROOT)
            hashes={'recipe.py':'source-digest'};modules={'CONFIG_UHID':'m'}
            tools={'sha256':{'clang':'compiler-digest'}}
            record={'status':'HOST_BUILT_FULL_CANDIDATE_NOT_HARDWARE_VERIFIED',
                    'source_commit':'a'*40,'source_tree':'b'*40,'inputs':hashes,'kernel_policy':policy['policy'],
                    'kernel_policy_sha256':policy['sha256'],'local_module_overrides':modules,
                    'root_policy':'LABEL=PIANOROOT','command_line':'piano.root=LABEL=PIANOROOT',
                    'toolchain':tools,'config_sha256':full.sha(output/'.config'),
                    'module_symvers_sha256':full.sha(output/'Module.symvers')}
            self.bind_release(record, output)

            def reusable(candidate):
                return full.matching_build(candidate,'a'*40,hashes,policy,modules,
                                           'LABEL=PIANOROOT',record['command_line'],tools,output)

            self.assertTrue(reusable(record))
            for key,value in (('source_commit','c'*40),('source_tree','c'*40),
                              ('inputs',{}),('local_module_overrides',{}),('kernel_release','7.3.3-piano'),
                              ('root_policy','ram'),('command_line','changed'),
                              ('toolchain',{'sha256':{'clang':'other-compiler'}}),
                              ('config_sha256','0'*64),('module_symvers_sha256','0'*64)):
                with self.subTest(field=key):
                    self.assertFalse(reusable({**record,key:value}))
            (output/'include/config/kernel.release').write_text('7.3.3-piano\n')
            self.assertFalse(reusable(record))

    def test_abi_identity_changes_with_real_build_inputs(self):
        policy=full.release_kernel_policy()
        epoch=policy['abi_epoch']
        values=(epoch,'a'*40,'b'*64,{'config':'c'*64},{'clang':'d'*64,'ld.lld':'e'*64})
        suffix=full.kernel_abi_suffix(*values)
        self.assertEqual(suffix,full.kernel_abi_suffix(
            *values[:-1],dict(reversed(list(values[-1].items())))))
        for index, changed in enumerate((epoch+1,'f'*40,'f'*64,
                                          {'config':'f'*64},{**values[-1],'clang':'f'*64})):
            with self.subTest(input=index):
                self.assertNotEqual(suffix,full.kernel_abi_suffix(
                    *values[:index],changed,*values[index+1:]))
        environment=full.kernel_build_environment(epoch)
        self.assertEqual(environment['SOURCE_DATE_EPOCH'],str(epoch))
        self.assertTrue(environment['KBUILD_BUILD_TIMESTAMP'].endswith('+00:00'))

    def test_legacy_whole_file_hash_does_not_prove_the_kernel_section(self):
        policy=full.kernel_policy_identity(ROOT)
        legacy={'inputs':{'config/release.json':full.sha(ROOT/'config/release.json')}}
        self.assertFalse(full.kernel_policy_matches(legacy,policy))
        self.assertEqual(set(legacy),{'inputs'})
        self.assertFalse(full.kernel_policy_matches(
            {'kernel_policy':policy['policy'],'kernel_policy_sha256':'0'*64},policy))


class PianoFullKernelTests(unittest.TestCase):
    def setUp(self):
        if not (KERNEL/'arch/arm64/configs/piano_defconfig').exists():
            self.skipTest('pinned kernel source not fetched (./build.sh sources)')

    def test_uhid_fragment_is_the_only_local_module_override(self):
        fragment=(ROOT/'linux/configs/piano-bluetooth.config').read_text()
        modules={**RECORD.get('local_module_overrides',{}),**full.module_overrides(fragment)}
        public=(KERNEL/'arch/arm64/configs/piano_rootfs.config').read_text()
        if not CONFIG.exists():self.skipTest(CONFIGURED)
        config=CONFIG.read_text()
        values=full.config_values(config)
        old='CONFIG_UHID='+values['CONFIG_UHID']if values['CONFIG_UHID']!='n'else '# CONFIG_UHID is not set'
        updated=config.replace(old,'CONFIG_UHID=m')
        requirements=full.validate_config(updated,public,modules)
        self.assertEqual(requirements['UHID'],'m')
        self.assertEqual(full.config_values(full.effective_config(modules))['CONFIG_UHID'],'m')
        with self.assertRaisesRegex(ValueError,'module requirement'):
            full.validate_config(updated.replace('CONFIG_UHID=m','# CONFIG_UHID is not set'),public,modules)
        for bad in ('CONFIG_UHID=y\n','CONFIG_UHID=n\n','CONFIG_UHID=m\nCONFIG_BT_LE=y\n','CONFIG_UHID=m\nCONFIG_UHID=n\n'):
            with self.subTest(fragment=bad),self.assertRaises(ValueError):full.module_overrides(bad)
        with self.assertRaises(ValueError):full.validate_config(updated,public,{'CONFIG_BT_LE':'y'})
        for name,value in full.config_values(public).items():
            if name in ('CONFIG_CMDLINE','CONFIG_CMDLINE_FORCE','CONFIG_CMDLINE_FROM_BOOTLOADER'):continue
            value=modules.get(name,value)
            changed=updated.replace(name+'='+value,name+'='+('n'if value!='n'else 'y'))
            if changed!=updated:
                with self.subTest(symbol=name),self.assertRaises(ValueError):full.validate_config(changed,public,modules)

    def test_filesystem_fragment_matches_overrides(self):
        fragment=(ROOT/'linux/configs/piano-filesystem.config').read_text()
        modules=full.module_overrides(fragment,'filesystem')
        disk_options=('CONFIG_EXFAT_FS','CONFIG_NTFS3_FS','CONFIG_NTFS3_LZX_XPRESS','CONFIG_NLS_UTF8')
        for symbol in disk_options:
            self.assertEqual(modules[symbol],'y')
            self.assertEqual(full.REQUIRED[symbol.removeprefix('CONFIG_')],'y')
        self.assertEqual(modules['CONFIG_EROFS_FS'],'m')
        self.assertEqual(modules['CONFIG_EROFS_FS_ZIP'],'y')
        self.assertEqual(len(modules),len(full.FILESYSTEM_OVERRIDES))
        for bad in ('CONFIG_EXFAT_FS=m\n',
                    'CONFIG_EXFAT_FS=y\nCONFIG_NTFS3_FS=y\nCONFIG_NTFS3_LZX_XPRESS=y\nCONFIG_NLS_UTF8=m\n'):
            with self.subTest(fragment=bad),self.assertRaises(ValueError):full.module_overrides(bad,'filesystem')
        for key,value in modules.items():
            self.assertEqual(full.ALLOWED_OVERRIDES[key],value)

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
        # The installed build may predate the pinned release series.
        head=full.text(['git','rev-parse','HEAD'],KERNEL)
        tree=full.text(['git','rev-parse','HEAD^{tree}'],KERNEL)
        self.assertEqual((head,tree),(RECORD['source_commit'],RECORD['source_tree']))
        self.assertFalse(full.text(['git','status','--porcelain=v1','--untracked-files=all'],KERNEL))
        for name,digest in full.SOURCE_PINS.items():
            self.assertEqual(full.sha(KERNEL/name),digest)
        public=(KERNEL/'arch/arm64/configs/piano_rootfs.config').read_text()
        config=path.read_text();modules=RECORD['local_module_overrides'];requirements=full.validate_config(config,public,modules)
        self.assertEqual(requirements['VIDEO_QCOM_CAMSS'],'m');self.assertEqual(requirements['DRM_MSM'],'m');self.assertEqual(requirements['SND_SOC_SC8280XP'],'m')
        for symbol in ('VIDEO_OV32D40','HID_NANOSIC_WN8030','BATTERY_PIANO_MCA','CHARGER_SC8541','QCOM_Q6V5_PAS'):
            with self.subTest(symbol=symbol),self.assertRaises(ValueError):full.validate_config(config.replace('CONFIG_'+symbol+'=m','# CONFIG_'+symbol+' is not set'),public,modules)

    def test_boot_arguments_are_external_and_protect_android(self):
        command=full.command_line('LABEL=PIANO_EXTERNAL')
        self.assertIn('piano.root=LABEL=PIANO_EXTERNAL',command);self.assertNotIn('root=PARTLABEL=userdata',command)
        disk=full.command_line('PARTUUID=ffc480ed-c219-400b-a8f9-5f6805aa1f34')
        self.assertIn('piano.root=PARTUUID=ffc480ed-c219-400b-a8f9-5f6805aa1f34',disk)
        for flag in ('rdinit=/pianoinit','pd_ignore_unused','clk_ignore_unused','iommu.passthrough=1'):self.assertIn(flag,command)
        for bad in ('PARTLABEL=userdata','/dev/sda','ram root=PARTLABEL=userdata','LABEL=userdata'):
            with self.subTest(root=bad),self.assertRaises(ValueError):full.command_line(bad)
        values=full.config_values(full.effective_config({}))
        self.assertEqual(values['CONFIG_CMDLINE'],'""')
        self.assertEqual(values['CONFIG_CMDLINE_FORCE'],'n')
        public=(KERNEL/'arch/arm64/configs/piano_rootfs.config').read_text()
        for bad in ('CONFIG_CMDLINE="console=tty0"\n','CONFIG_CMDLINE_FORCE=y\n'):
            with self.assertRaisesRegex(ValueError,'must not embed or force'):
                full.validate_config(bad,public)

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
                        'patch_series':'config/series.json'}}))
                    series=policy_root/'config/series.json'
                    pins={'base_commit':commit,'target_tree':git('rev-parse','HEAD^{tree}')}
                    with patch.object(full,'ROOT',policy_root),patch.object(full,'COMMIT','f'*40):
                        # A thin CI checkout has the fixed base and replayed
                        # commits, not the downstream author's original object.
                        series.write_text(json.dumps({**pins,'original_target_commit':'e'*40}))
                        self.assertEqual(full.verify_source(work,descendant),commit)
                        series.write_text(json.dumps({**pins,'original_target_commit':descendant}))
                        (work/'new').write_text('release derivative');git('add','new')
                        git('-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','-qm','release derivative')
                        self.assertEqual(full.verify_source(work,git('rev-parse','HEAD')),commit)
                disconnected=git('-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit-tree','HEAD^{tree}','-m','disconnected')
                git('checkout','--detach',disconnected)
                with self.assertRaisesRegex(ValueError,'does not descend'):full.verify_source(work,disconnected)

    def test_real_module_vermagic_hash_and_duplicate_rejection(self):
        source=ROOT/'artifacts/kernels/release/modules'
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
