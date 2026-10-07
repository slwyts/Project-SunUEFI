from pathlib import Path
import importlib.util,subprocess,tempfile,unittest,re,shutil,sys
ROOT=Path(__file__).resolve().parents[2];BASE=ROOT/'upstream/Mu-Silicium/Mu_Basecore'
sys.path.insert(0,str(ROOT/'tools'))
spec=importlib.util.spec_from_file_location('late_prepare',ROOT/'tools/prepare_product_handoff.py');prepare=importlib.util.module_from_spec(spec);spec.loader.exec_module(prepare)
def function(text,name):
 matches=re.finditer(r'(?m)^(?:STATIC\s+)?(?:EFI_STATUS\s+(?:EFIAPI\s+)?|BOOLEAN\s+|UINTN\s+|EFI_LOADED_IMAGE_PROTOCOL\s*\*\s*)'+name+r'\s*\(',text)
 m=next((m for m in matches if ';'not in text[m.start():text.index('{',m.start())]),None)
 if not m:raise ValueError(name)
 at=text.index('{',m.start());end=at+1;depth=1
 while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[m.start():end]
class LateHandoffTests(unittest.TestCase):
 def original_sources(self):
  return {name:subprocess.check_output(['git','-C',str(BASE),'show','HEAD:'+name[len(prepare.BASE):]])for name in prepare.PINS}
 def test_original_and_installed_footer_comments_keep_exact_hooks(self):
  originals=self.original_sources()
  footers=(' \t\n\n','\n// Build annotation.\n','\n/* Build annotation. */\n','\n/* Build annotation.\n   No executable content. */\n\t')
  for newline in ('\n','\r\n'):
   for footer in footers:
    with self.subTest(newline=newline,footer=footer),tempfile.TemporaryDirectory()as directory:
     root=Path(directory)
     for name,data in originals.items():
      path=root/name;path.parent.mkdir(parents=True,exist_ok=True)
      text=data.decode().replace('\r\n','\n').rstrip(' \t\n')+footer
      path.write_bytes(text.replace('\n',newline).encode())
     desired=prepare.desired_core(root)
     for name,data in desired.items():(root/name).write_bytes(data)
     self.assertEqual(desired,prepare.desired_core(root))
     for name in desired:
      path=root/name;path.write_bytes(path.read_bytes()+('\n// Installed annotation.\n').replace('\n',newline).encode())
     installed={name:(root/name).read_bytes()for name in desired}
     self.assertEqual(installed,prepare.desired_core(root))
     dxe=installed[prepare.BASE+'MdeModulePkg/Core/Dxe/DxeMain/DxeMain.c'].decode().replace('\r\n','\n')
     self.assertEqual(dxe.count(prepare.EXIT_HOOK),1)
     self.assertLess(dxe.index(prepare.EXIT_HOOK),dxe.index('// Notify other drivers of their last chance to use boot services'))
 def test_footer_cannot_hide_code_or_preprocessor_continuations(self):
  originals=self.original_sources();selected=next(iter(originals))
  tails=('\n/* first */\nVOID Evil (VOID) {}\n/* last */\n',
    '\n/* note */ VOID Evil (VOID) {}\n','\n/* unclosed\n','\n*/\n',
    '\nVOID Evil (VOID) {} // note\n','\n// note \\\n','\n// note ??/\n','\r// note\n')
  with tempfile.TemporaryDirectory()as directory:
   root=Path(directory)
   for name,data in originals.items():
    path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
   for tail in tails:
    with self.subTest(tail=tail):
     (root/selected).write_bytes(originals[selected]+tail.encode())
     with self.assertRaises(ValueError):prepare.desired_core(root)
 def test_native_boundaries_and_installed_hook_tampering_still_fail(self):
  originals=self.original_sources();dxe=prepare.BASE+'MdeModulePkg/Core/Dxe/DxeMain/DxeMain.c'
  with tempfile.TemporaryDirectory()as directory:
   root=Path(directory)
   for name,data in originals.items():
    path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
   text=originals[dxe].decode().replace('\r\n','\n')
   mutations=(text.replace('// Notify other drivers of their last chance to use boot services','// Changed EBS boundary'),
     text.replace('#include "DxeMain.h"','#include "ChangedDxeMain.h"'))
   for changed in mutations:
    (root/dxe).write_text(changed)
    with self.assertRaises(ValueError):prepare.desired_core(root)
   (root/dxe).write_bytes(originals[dxe]);installed=prepare.desired_core(root)
   for name,data in installed.items():(root/name).write_bytes(data)
   text=installed[dxe].decode().replace('\r\n','\n')
   mutations=(text.replace(prepare.EXIT_HOOK,'',1),text.replace(prepare.EXIT_HOOK,prepare.EXIT_HOOK*2,1),
     text.replace('MapKey, CorePianoCurrentMemoryMapKey ()','MapKey, 0'),
     text.replace(prepare.EXIT_HOOK,'',1)+prepare.EXIT_HOOK)
   for changed in mutations:
    (root/dxe).write_text(changed)
    with self.assertRaises(ValueError):prepare.desired_core(root)
   (root/dxe).write_bytes(installed[dxe])
   for name,getter in ((prepare.BASE+'MdeModulePkg/Core/Dxe/Image/Image.c',prepare.IMAGE_GETTER),
                       (prepare.BASE+'MdeModulePkg/Core/Dxe/Mem/Page.c',prepare.KEY_GETTER)):
    text=installed[name].decode().replace('\r\n','\n')
    for changed in (text.replace(getter,'',1),text+getter,
                    text.replace(getter,'',1)+getter.replace('return','return /* altered */',1),
                    getter+text.replace(getter,'',1)):
     (root/name).write_text(changed)
     with self.assertRaises(ValueError):prepare.desired_core(root)
    (root/name).write_bytes(installed[name])
 def test_temporary_exact_stage_idempotence_and_drift(self):
  before={name:(ROOT/name).read_bytes()for name in prepare.PINS}
  with tempfile.TemporaryDirectory(prefix='piano-late-stage-')as directory:
   root=Path(directory);shutil.copytree(ROOT/'uefi/components/product-handoff',root/'uefi/components/product-handoff')
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
  inc=BASE/'MdePkg/Include';shared=ROOT/'uefi/components/product-handoff/Mu_Basecore/MdePkg/Include'
  with tempfile.TemporaryDirectory(prefix='piano-late-ebs-')as directory:
   p=Path(directory);(p/'PianoActualNativeExit.h').write_text(actual)
   linux=(ROOT/'uefi/components/os-boot/PianoLinuxEfiSession.c').read_text()
   (p/'PianoActualInitrdCopy.h').write_text(function(linux,'Overlap')+'\n'+function(linux,'LoadInitrd'))
   fixture=(ROOT/'tests/native/PianoProductOwnersTest.c').read_text().replace('int main(void)','int OriginalOwnersMain(void)',1)
   fixture=fixture.replace('#undef NULL\n','',1)
   fixture=fixture.replace("record('M');return EventStatus;","record('M');++mMemoryMapKey;return EventStatus;")
   (p/'PianoLateOwnersFixture.h').write_text('#include <Uefi.h>\nstatic UINTN mMemoryMapKey;\n'+fixture)
   exe=p/'late';command=['cc','-std=gnu11','-DNO_MSABI_VA_FUNCS','-DPIANO_PRODUCT_NATIVE_LATE=1','-fshort-wchar','-Wall','-Wextra','-Werror',
     '-Wno-unused-parameter','-Wno-misleading-indentation','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',
     '-I'+str(inc),'-I'+str(inc/'X64'),'-I'+str(shared),'-I'+str(ROOT/'uefi/core'),'-I'+str(ROOT/'tests/native'),'-I'+str(p),
     '-Wno-deprecated-declarations','-I'+str(BASE/'CryptoPkg/Include'),
     str(ROOT/'tests/native/PianoLateHandoffTest.c'),str(ROOT/'uefi/components/product-handoff/PianoLateHandoff.c'),
     str(ROOT/'uefi/components/product-handoff/Mu_Basecore/MdePkg/Library/PianoProductExitLib/PianoProductExitLib.c'),
     str(ROOT/'uefi/components/os-boot/PianoCpuInput.c'),'-lcrypto','-o',str(exe)]
   subprocess.run(command,check=True,timeout=60)
   for case in range(26):subprocess.run([str(exe),str(case)],check=True,timeout=60)
   native=p/'native-launch';joint=list(command)
   joint[joint.index(str(ROOT/'tests/native/PianoLateHandoffTest.c'))]=str(ROOT/'tests/native/PianoNativeLaunchTest.c')
   joint[joint.index('-o')+1]=str(native)
   joint.insert(joint.index('-o'),str(ROOT/'uefi/core/PianoFastbootLaunch.c'))
   joint.insert(joint.index('-o'),str(ROOT/'uefi/core/PianoFastbootBoot.c'))
   subprocess.run(joint,check=True,timeout=60)
   for case in range(20,28):subprocess.run([str(native),str(case)],check=True,timeout=60)
   linux=p/'native-linux';combined=list(joint)
   combined[combined.index(str(ROOT/'tests/native/PianoNativeLaunchTest.c'))]=str(ROOT/'tests/native/PianoNativeLinuxSessionTest.c')
   fdt=BASE/'MdePkg/Library/BaseFdtLib/libfdt/libfdt'
   combined +=['-I'+str(fdt),'-I'+str(ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include')]
   combined +=[str(ROOT/'uefi/components/os-boot/PianoLinuxEfiSession.c'),str(ROOT/'tests/native/PianoLinuxFdtHost.c')]
   combined +=[str(fdt/name)for name in ('fdt.c','fdt_ro.c','fdt_rw.c','fdt_wip.c','fdt_sw.c','fdt_check.c','fdt_empty_tree.c')]
   combined[combined.index('-o')+1]=str(linux)
   subprocess.run(combined,check=True,timeout=60)
   for case in range(30,38):subprocess.run([str(linux),str(case)],check=True,timeout=60)
 def test_actual_aarch64_sources_and_no_current_mu_mutation(self):
  before={name:(ROOT/name).read_bytes()for name in prepare.PINS}
  inc=BASE/'MdePkg/Include';shared=ROOT/'uefi/components/product-handoff/Mu_Basecore/MdePkg/Include'
  for name in ('PianoLateHandoff.c','Mu_Basecore/MdePkg/Library/PianoProductExitLib/PianoProductExitLib.c','Mu_Basecore/MdePkg/Library/PianoProductExitLibNull/PianoProductExitLibNull.c'):
   subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding',
     '-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-I'+str(inc),'-I'+str(inc/'AArch64'),
     '-I'+str(shared),'-I'+str(ROOT/'uefi/core'),str(ROOT/'uefi/components/product-handoff'/name)],check=True)
  report=prepare.prepare(apply=False);self.assertFalse(report['product_provider_bound'])
  self.assertEqual(before,{name:(ROOT/name).read_bytes()for name in prepare.PINS})
if __name__=='__main__':unittest.main()
