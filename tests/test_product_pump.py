"""Actual client and exact extracted CoreWaitForEvent/gui_main; no device/build."""
import os
import importlib.util
from pathlib import Path
import re
import subprocess
import tempfile
import shutil
import sys
from unittest.mock import patch
import unittest

ROOT=Path(__file__).resolve().parents[1];BASE=ROOT/"upstream/Mu-Silicium/Mu_Basecore"
spec=importlib.util.spec_from_file_location("prepare_product_pump",ROOT/"tools/prepare_product_pump.py")
prepare=importlib.util.module_from_spec(spec);spec.loader.exec_module(prepare)
sys.modules['prepare_product_pump']=prepare
spec=importlib.util.spec_from_file_location('simpleinit_build_identity',ROOT/'tools/simpleinit_build_identity.py')
identity=importlib.util.module_from_spec(spec);spec.loader.exec_module(identity)


def function(text,name):
    match=re.search(r'(?m)^(?:EFI_STATUS\s+EFIAPI\s+|int\s+|void\s+|uint32_t\s+)'+name+r'\s*\(',text)
    if not match:raise ValueError("actual function absent: "+name)
    begin=text.index("{",match.start());depth=1;end=begin+1
    while depth:
        if text[end]=="{":depth+=1
        elif text[end]=="}":depth-=1
        end+=1
    return text[match.start():end]


class ProductPumpTests(unittest.TestCase):
    def test_canonical_sources_are_installed_in_actual_build_checkout(self):
        manifest=prepare.prepare(apply=False)
        self.assertEqual(len(manifest['files']),15)
        self.assertEqual(manifest['default_binding'],'PianoProductPumpLibNull')

    def checkout_fixture(self, directory):
        root=Path(directory)
        shutil.copytree(ROOT/'bootprofiles/product-pump',root/'bootprofiles/product-pump')
        p=root/'bootprofiles/uefi-app/Protocol';p.mkdir(parents=True)
        shutil.copyfile(ROOT/'bootprofiles/uefi-app/Protocol/PianoProductRuntime.h',p/'PianoProductRuntime.h')
        for relative,_,_ in prepare.HOOKS:
            path=root/relative
            if path.exists():continue
            repo=next(key for key in sorted(prepare.PINS,key=len,reverse=True)if relative.startswith(key+'/'))
            name=relative[len(repo)+1:]
            path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes(subprocess.check_output(['git','-C',str(ROOT/repo),'show','HEAD:'+name]))
        return root

    def test_clean_pinned_source_apply_is_idempotent_and_default_null(self):
        with tempfile.TemporaryDirectory(prefix='product-hook-apply-')as directory:
            root=self.checkout_fixture(directory)
            first=prepare.prepare(root,apply=True,check_pins=False)
            second=prepare.prepare(root,apply=True,check_pins=False)
            self.assertEqual(first,second)
            self.assertEqual(first,prepare.prepare(root,apply=False,check_pins=False))
            gui=(root/'upstream/simple-init/src/gui/gui_init.c').read_text()
            self.assertEqual(gui.count('#include"piano_product_runtime.h"'),1)
            self.assertNotIn('ReadKeyStroke=',gui)

    def test_modified_or_duplicate_hook_refused_before_any_mutation(self):
        for mutation in ('duplicate','modified','gui_wait','gui_tick'):
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory(prefix='product-hook-refuse-')as directory:
                root=self.checkout_fixture(directory)
                prepare.prepare(root,apply=True,check_pins=False)
                event=root/(prepare.SI+'src/gui/gui_init.c')if mutation.startswith('gui_')else root/(prepare.BASE+'MdeModulePkg/Core/Dxe/Event/Event.c')
                text=event.read_text()
                if mutation=='duplicate':text+='\n'+prepare.WAIT_HOOK
                elif mutation=='modified':text=text.replace('PIANO_PRODUCT_PUMP_WAIT_EVENT, 1000','PIANO_PRODUCT_PUMP_WAIT_EVENT, 1001')
                elif mutation=='gui_wait':text=text.replace('piano_product_gui_wait(time)','piano_product_gui_wait(time*10)')
                else:text=text.replace('return (uint32_t)piano_product_gui_tick();','return (uint32_t)piano_product_gui_tick()+1;')
                event.write_text(text)
                library=root/(prepare.BASE+'MdePkg/Library/PianoProductPumpLib/PianoProductPumpLib.c')
                library.write_text('preserve me')
                with self.assertRaises(ValueError):prepare.prepare(root,apply=True,check_pins=False)
                self.assertEqual(library.read_text(),'preserve me')

    def test_build_identity_refuses_wrong_actual_binding_flags_and_output(self):
        hooks=prepare.prepare(apply=False)
        for mutation in ('none','report','link','gui','output','dsc','sources','navigation_sources'):
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory(prefix='product-si-id-')as directory:
                base=Path(directory);build=base/'build';output=base/'output';build.mkdir();output.mkdir()
                dsc=build/'SunSimpleInit.dsc';dsc.write_text('real product DSC')
                owned=ROOT/'upstream/simple-init/src/main/uefimain.c'
                source={'product_gui_pump':True,'pump_hooks':hooks,'dsc_sha256':identity.sha(dsc),'owned_sources':{str(owned.relative_to(ROOT)):identity.sha(owned),**{name:identity.sha(ROOT/name)for name in identity.PRODUCT_NAVIGATION_SOURCES}}}
                (build/'source-manifest.json').write_text(__import__('json').dumps(source))
                report=build/'simpleinit-build-report.txt';report.write_text('MdePkg/Library/PianoProductPumpLib/PianoProductPumpLib.inf')
                target=build/'Build/SimpleInit/NOOPT_CLANGDWARF/AARCH64';target.mkdir(parents=True)
                gui=target/'src/gui/SimpleInitGUI/GNUmakefile';gui.parent.mkdir(parents=True);gui.write_text('-DPIANO_PRODUCT_GUI_PUMP=1')
                main=target/'src/main/SimpleInitMain/GNUmakefile';main.parent.mkdir(parents=True);main.write_text('/MdePkg/Library/PianoProductPumpLib/PianoProductPumpLib/OUTPUT/PianoProductPumpLib.lib')
                (target/'SimpleInitMain.efi').write_bytes(b'actual build output');(output/'SimpleInit.efi').write_bytes(b'actual build output')
                if mutation=='report':report.write_text('MdePkg/Library/PianoProductPumpLibNull/PianoProductPumpLibNull.inf')
                if mutation=='link':main.write_text('/MdePkg/Library/PianoProductPumpLibNull/PianoProductPumpLibNull/OUTPUT/PianoProductPumpLibNull.lib')
                if mutation=='gui':gui.write_text('-DPIANO_PRODUCT_GUI_PUMP=0')
                if mutation=='output':(output/'SimpleInit.efi').write_bytes(b'old output')
                if mutation=='dsc':dsc.write_text('changed DSC')
                if mutation=='sources':source['pump_hooks']={};(build/'source-manifest.json').write_text(__import__('json').dumps(source))
                if mutation=='navigation_sources':source['owned_sources'].pop(identity.PRODUCT_NAVIGATION_SOURCES[0]);(build/'source-manifest.json').write_text(__import__('json').dumps(source))
                # This fixture isolates post-compile metadata checks. Actual
                # installed UI/GUI source verification is covered separately.
                with patch.dict(sys.modules,{'prepare_product_ui':type('Ui',(),{'prepare':staticmethod(lambda *args,**kwargs:None)})}):
                    if mutation=='none':self.assertTrue(identity.inspect(build,output,True)['product_gui_pump'])
                    else:
                        with self.assertRaises(ValueError):identity.inspect(build,output,True)

    def test_actual_client_core_wait_gui_matrix(self):
        event=(BASE/"MdeModulePkg/Core/Dxe/Event/Event.c").read_text()
        gui=(ROOT/"upstream/simple-init/src/gui/gui_init.c").read_text()
        extracted="\n\n".join((function(event,"CoreWaitForEvent"),function(gui,"gui_main"),function(gui,"gui_run_and_exit"),
            "#if PIANO_PRODUCT_GUI_PUMP\n"+function(gui,"custom_tick_get")+"\n#endif"))
        include=BASE/"MdePkg/Include"
        with tempfile.TemporaryDirectory(prefix="product-pump-host-")as directory:
            directory=Path(directory);(directory/"PianoActualWaitAndGui.h").write_text(extracted)
            for enabled in(0,1):
                exe=directory/("pump-"+str(enabled))
                cmd=["cc","-std=gnu11","-Wall","-Wextra","-Werror","-Wno-unused-function","-g","-fshort-wchar","-fno-pie","-no-pie","-fsanitize=address,undefined","-DENABLE_UEFI",f"-DPIANO_PRODUCT_GUI_PUMP={enabled}",
                    "-I"+str(include),"-I"+str(include/"X64"),"-I"+str(directory),"-I"+str(ROOT/"upstream/simple-init/src/gui"),str(ROOT/"tests/PianoProductPumpTest.c"),str(BASE/"MdePkg/Library/PianoProductPumpLib/PianoProductPumpLib.c"),str(ROOT/"upstream/simple-init/src/gui/piano_product_runtime.c"),"-o",str(exe)]
                build=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(build.returncode,0,build.stdout+build.stderr)
                run=subprocess.run([str(exe)],capture_output=True,text=True,env={**os.environ,"ASAN_OPTIONS":"detect_leaks=1"});self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())
        for enabled in(0,1):
            subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc',
                '-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror',
                '-DENABLE_UEFI',f'-DPIANO_PRODUCT_GUI_PUMP={enabled}',
                '-I'+str(include),'-I'+str(include/'AArch64'),
                str(ROOT/'upstream/simple-init/src/gui/piano_product_runtime.c')],check=True)

    def test_default_null_has_no_boot_service_or_provider_dependency(self):
        include=BASE/"MdePkg/Include"
        source='''#include <Library/PianoProductPumpLib.h>
#include <assert.h>
EFI_TPL gEfiCurrentTpl=TPL_APPLICATION;
EFI_EVENT gIdleLoopEvent=(VOID *)9;
UINT32 Checks,Signals;
EFI_STATUS CoreCheckEvent(EFI_EVENT E){assert(E==(VOID *)1);Checks++;return EFI_SUCCESS;}
EFI_STATUS CoreSignalEvent(EFI_EVENT E){(VOID)E;Signals++;return EFI_SUCCESS;}
'''
        source+=function((BASE/'MdeModulePkg/Core/Dxe/Event/Event.c').read_text(),'CoreWaitForEvent')
        source+='''
int main(void){UINT32 A=99;UINT64 S=99;assert(PianoProductPumpApplication(1,1000)==EFI_UNSUPPORTED);assert(PianoProductGetPendingAction(&A,&S)==EFI_UNSUPPORTED&&A==0&&S==0);assert(PianoProductPumpBootServicesAlive()==TRUE);assert(!PianoProductReturnCoreRequested());assert(PianoProductRequestReboot()==EFI_UNSUPPORTED&&!PianoProductRebootManaged());EFI_EVENT E=(VOID *)1;UINTN I=99;assert(CoreWaitForEvent(1,&E,&I)==EFI_SUCCESS&&I==0&&Checks==1&&!Signals);gEfiCurrentTpl=TPL_CALLBACK;assert(CoreWaitForEvent(1,&E,&I)==EFI_SUCCESS&&Checks==2&&!Signals);return 0;}
'''
        with tempfile.TemporaryDirectory(prefix="null-pump-host-")as directory:
            directory=Path(directory);(directory/"null.c").write_text(source);exe=directory/"null"
            subprocess.run(["cc","-std=c11","-fshort-wchar","-I"+str(include),"-I"+str(include/"X64"),str(directory/"null.c"),str(BASE/"MdePkg/Library/PianoProductPumpLibNull/PianoProductPumpLibNull.c"),"-o",str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
            symbols=subprocess.run(["nm","-u",str(exe)],check=True,capture_output=True,text=True).stdout
            for symbol in("gBS","gST","LocateProtocol","CreateEvent","Pump"):self.assertNotIn(symbol,symbols)


if __name__=="__main__":unittest.main()
