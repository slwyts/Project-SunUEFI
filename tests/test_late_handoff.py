from pathlib import Path
import importlib.util,subprocess,tempfile,unittest,re,shutil
ROOT=Path(__file__).resolve().parents[1];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
spec=importlib.util.spec_from_file_location('late_prepare',ROOT/'tools/prepare_product_handoff.py');prepare=importlib.util.module_from_spec(spec);spec.loader.exec_module(prepare)
def function(text,name):
 matches=re.finditer(r'(?m)^(?:STATIC\s+)?(?:EFI_STATUS\s+(?:EFIAPI\s+)?|BOOLEAN\s+|UINTN\s+|EFI_LOADED_IMAGE_PROTOCOL\s*\*\s*)'+name+r'\s*\(',text)
 m=next((m for m in matches if ';'not in text[m.start():text.index('{',m.start())]),None)
 if not m:raise ValueError(name)
 at=text.index('{',m.start());end=at+1;depth=1
 while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[m.start():end]
class LateHandoffTests(unittest.TestCase):
 def test_temporary_exact_stage_idempotence_and_drift(self):
  before={name:(ROOT/name).read_bytes()for name in prepare.PINS}
  with tempfile.TemporaryDirectory(prefix='piano-late-stage-')as directory:
   root=Path(directory);shutil.copytree(ROOT/'bootprofiles/product-handoff',root/'bootprofiles/product-handoff')
   files=list(prepare.PINS)+[
    prepare.BASE+'MdeModulePkg/Core/Dxe/DxeMain.inf',prepare.BASE+'MdePkg/MdePkg.dec',
    prepare.BASE+'MdePkg/MdeLibs.dsc.inc','upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/SiliciumPkg.dsc.inc']
   for name in files:
    path=root/name;path.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(ROOT/name,path)
   desired=prepare.prepare(root,apply=False);applied=prepare.prepare(root,apply=True)
   self.assertEqual(desired['files'],applied['files']);self.assertEqual(applied,prepare.prepare(root,apply=True))
   path=root/(prepare.BASE+'MdeModulePkg/Core/Dxe/DxeMain/DxeMain.c')
   path.write_text(path.read_text().replace('PianoProductBeforeExitBootServices','ChangedProductBeforeExitBootServices'))
   with self.assertRaises((ValueError,subprocess.CalledProcessError)):prepare.prepare(root,apply=True)
  self.assertEqual(before,{name:(ROOT/name).read_bytes()for name in prepare.PINS})
 def test_actual_native_core_provider_and_owner_retirement(self):
  desired=prepare.desired_core();exit=function(desired[prepare.BASE+'MdeModulePkg/Core/Dxe/DxeMain/DxeMain.c'].decode(),'CoreExitBootServices')
  memory=(BASE/'MdeModulePkg/Core/Dxe/Mem/Page.c').read_text()
  actual='\n\n'.join((prepare.IMAGE_GETTER,prepare.KEY_GETTER,function(memory,'CoreTerminateMemoryMap'),exit))
  inc=BASE/'MdePkg/Include';shared=ROOT/'bootprofiles/product-handoff/Mu_Basecore/MdePkg/Include'
  with tempfile.TemporaryDirectory(prefix='piano-late-ebs-')as directory:
   p=Path(directory);(p/'PianoActualNativeExit.h').write_text(actual)
   linux=(ROOT/'bootprofiles/os-boot/PianoLinuxEfiSession.c').read_text()
   (p/'PianoActualInitrdCopy.h').write_text(function(linux,'Overlap')+'\n'+function(linux,'LoadInitrd'))
   fixture=(ROOT/'tests/PianoProductOwnersTest.c').read_text().replace('int main(void)','int OriginalOwnersMain(void)',1)
   fixture=fixture.replace('#undef NULL\n','',1)
   fixture=fixture.replace("record('M');return EventStatus;","record('M');++mMemoryMapKey;return EventStatus;")
   (p/'PianoLateOwnersFixture.h').write_text('#include <Uefi.h>\nstatic UINTN mMemoryMapKey;\n'+fixture)
   exe=p/'late';command=['cc','-std=gnu11','-DNO_MSABI_VA_FUNCS','-DPIANO_PRODUCT_NATIVE_LATE=1','-fshort-wchar','-Wall','-Wextra','-Werror',
     '-Wno-unused-parameter','-Wno-misleading-indentation','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',
     '-I'+str(inc),'-I'+str(inc/'X64'),'-I'+str(shared),'-I'+str(ROOT/'bootprofiles/uefi-app'),'-I'+str(ROOT/'tests'),'-I'+str(p),
     '-Wno-deprecated-declarations','-I'+str(BASE/'CryptoPkg/Include'),
     str(ROOT/'tests/PianoLateHandoffTest.c'),str(ROOT/'bootprofiles/product-handoff/PianoLateHandoff.c'),
     str(ROOT/'bootprofiles/product-handoff/Mu_Basecore/MdePkg/Library/PianoProductExitLib/PianoProductExitLib.c'),
     str(ROOT/'bootprofiles/os-boot/PianoCpuInput.c'),'-lcrypto','-o',str(exe)]
   subprocess.run(command,check=True,timeout=60)
   for case in range(18):subprocess.run([str(exe),str(case)],check=True,timeout=60)
   native=p/'native-launch';joint=list(command)
   joint[joint.index(str(ROOT/'tests/PianoLateHandoffTest.c'))]=str(ROOT/'tests/PianoNativeLaunchTest.c')
   joint[joint.index('-o')+1]=str(native)
   joint.insert(joint.index('-o'),str(ROOT/'bootprofiles/uefi-app/PianoFastbootLaunch.c'))
   joint.insert(joint.index('-o'),str(ROOT/'bootprofiles/uefi-app/PianoFastbootBoot.c'))
   subprocess.run(joint,check=True,timeout=60)
   for case in range(20,28):subprocess.run([str(native),str(case)],check=True,timeout=60)
   linux=p/'native-linux';combined=list(joint)
   combined[combined.index(str(ROOT/'tests/PianoNativeLaunchTest.c'))]=str(ROOT/'tests/PianoNativeLinuxSessionTest.c')
   fdt=BASE/'MdePkg/Library/BaseFdtLib/libfdt/libfdt'
   combined +=['-I'+str(fdt),'-I'+str(ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include')]
   combined +=[str(ROOT/'bootprofiles/os-boot/PianoLinuxEfiSession.c'),str(ROOT/'tests/PianoLinuxFdtHost.c')]
   combined +=[str(fdt/name)for name in ('fdt.c','fdt_ro.c','fdt_rw.c','fdt_wip.c','fdt_sw.c','fdt_check.c','fdt_empty_tree.c')]
   combined[combined.index('-o')+1]=str(linux)
   subprocess.run(combined,check=True,timeout=60)
   for case in range(30,38):subprocess.run([str(linux),str(case)],check=True,timeout=60)
 def test_actual_aarch64_sources_and_no_current_mu_mutation(self):
  before={name:(ROOT/name).read_bytes()for name in prepare.PINS}
  inc=BASE/'MdePkg/Include';shared=ROOT/'bootprofiles/product-handoff/Mu_Basecore/MdePkg/Include'
  for name in ('PianoLateHandoff.c','Mu_Basecore/MdePkg/Library/PianoProductExitLib/PianoProductExitLib.c','Mu_Basecore/MdePkg/Library/PianoProductExitLibNull/PianoProductExitLibNull.c'):
   subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding',
     '-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-I'+str(inc),'-I'+str(inc/'AArch64'),
     '-I'+str(shared),'-I'+str(ROOT/'bootprofiles/uefi-app'),str(ROOT/'bootprofiles/product-handoff'/name)],check=True)
  report=prepare.prepare(apply=False);self.assertFalse(report['product_provider_bound'])
  self.assertEqual(before,{name:(ROOT/name).read_bytes()for name in prepare.PINS})
if __name__=='__main__':unittest.main()
