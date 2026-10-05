"""Execute exact patched upstream wait/prompt/menu/cleanup code on the host."""
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import prepare_product_ui as prepare
BASE=ROOT/prepare.BASE

def function(text,name):
    m=re.search(r'(?m)^(?:EFI_STATUS|VOID|UI_EVENT_TYPE)\s+(?:EFIAPI\s+)?'+name+r'\s*\(',text)
    if not m:raise ValueError('missing actual function '+name)
    begin=text.index('{',m.start());depth=1;end=begin+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[m.start():end]

def generated():
    display=(ROOT/(prepare.DISPLAY+'FormDisplay.c')).read_text()
    cdl=(ROOT/(prepare.CDL+'CustomizedDisplayLibInternal.c')).read_text()
    shell=(ROOT/(prepare.SHELL+'Shell.c')).read_text()
    reader=(ROOT/(prepare.SHELL+'FileHandleWrappers.c')).read_text()
    ui=(ROOT/(prepare.UI+'FrontPage.c')).read_text()
    browser=(ROOT/(prepare.BROWSER+'Presentation.c')).read_text()
    console=(ROOT/(prepare.UEFI_LIB+'Console.c')).read_text()
    simple=(ROOT/(prepare.SI+'src/lib/reboot.c')).read_text()
    boot=(ROOT/(prepare.SI+'src/boot/reboot_uefi.c')).read_text()
    reboot_types=simple[simple.index('typedef enum REBOOT_REASON_TYPE'):simple.index('int adv_reboot(')]
    reboot_begin=simple.index('int adv_reboot(');reboot_end=simple.index('\n}\n',reboot_begin)+2
    popup_begin=console.index('  if (Key != NULL) {\n',console.index('CreatePopUp ('))
    popup_end=console.index('\n}\n',popup_begin)
    popup='VOID ActualPopupWait(EFI_INPUT_KEY *Key) {EFI_STATUS Status;UINTN EventIndex;\n'+console[popup_begin:popup_end]+'\n}\n'
    read_begin=reader.index('    PianoProductPumpApplication (PIANO_PRODUCT_PUMP_APP, 1000);',reader.index('FileInterfaceStdInRead ('))
    read_end=reader.index('    //\n    // Press PageUp',read_begin)
    read_tail=reader[reader.index('  *BufferSize = StringLen * sizeof (CHAR16);',read_end):reader.index('\n}\n',read_end)]
    readline='''EFI_STATUS ActualConsoleRead(VOID *Unused,UINTN *BufferSize,VOID *Buffer) {
CHAR16 *CurrentString=Buffer; UINTN MaxStr=*BufferSize/sizeof(CHAR16),StringLen=0,EventIndex=0;
EFI_INPUT_KEY Key;EFI_STATUS Status=EFI_SUCCESS;EFI_SHELL_FILE_INFO *TabCompleteList=AllocateZeroPool(8);
(void)Unused;
do {
'''+reader[read_begin:read_end]+'''break;
} while (TRUE);
'''+read_tail+'\n}\n'
    main_cleanup=shell[shell.index('FreeResources:\n'):shell.index('\n}\n',shell.index('FreeResources:\n'))]
    main='EFI_STATUS ActualShellCleanup(VOID) { EFI_STATUS Status=EFI_ABORTED; SPLIT_LIST *Split;\n'+main_cleanup+'\n}\n'
    cfread=display[display.index('      case CfReadKey:\n'):display.index('        switch (Key.UnicodeChar)',display.index('      case CfReadKey:\n'))]
    cfexit=display[display.index('      case CfExit:\n'):display.index('      default:\n',display.index('      case CfExit:\n'))]
    menu='''EFI_STATUS ActualMenu(VOID) {
UINTN ControlFlag=CfReadKey; EFI_STATUS Status; UI_EVENT_TYPE EventType=UIEventNone; EFI_INPUT_KEY Key;
CHAR16 *HelpString=AllocateZeroPool(8),*HelpHeaderString=AllocateZeroPool(8),*HelpBottomString=AllocateZeroPool(8);
for (;;) {switch(ControlFlag){
'''+cfread+'''return EFI_NOT_READY;
'''+cfexit+'''default:abort();}}
}
'''
    browser_hook=browser[browser.index('    Status = DisplayForm ();\n',browser.index('SetupBrowser (')):browser.index('    //\n    // Check Selected Statement',browser.index('    Status = DisplayForm ();\n',browser.index('SetupBrowser (')))]
    close_begin=browser.index('    if ((ConfigAccess != NULL) &&\n',browser.index('// Before exit the form,'))
    close_end=browser.index('  } while (Selection->Action == UI_ACTION_REFRESH_FORM);',close_begin)
    done_begin=browser.index('Done:\n',close_end)
    done_end=browser.index('\n}\n',done_begin)
    close='EFI_STATUS ActualBrowserClose(UI_MENU_SELECTION *Selection) {EFI_STATUS Status;VOID *ConfigAccess=(VOID *)1;VOID *NotifyHandle=(VOID *)2;\n'+browser_hook+browser[close_begin:close_end]+browser[done_begin:done_end]+'\n}\n'
    input_begin=browser.index('  if (PianoProductReturnCoreRequested ()) {\n',browser.index('DisplayForm ('))
    input_end=browser.index('\n}\n',input_begin)
    input_branch='EFI_STATUS ActualInputBranch(USER_INPUT UserInput) {EFI_STATUS Status;\n'+browser[input_begin:input_end]+'\n}\n'
    reboot_boot=boot[boot.index('int run_boot_reboot('):boot.index('\n}\n',boot.index('int run_boot_reboot('))+2]
    return '\n\n'.join((function(display,'UiWaitForEvent'),function(cdl,'WaitForKeyStroke'),readline,function(shell,'DoShellPrompt'),menu,close,input_branch,popup,function(ui,'UiEntry'),function(ui,'SetupResetReminder'),main,reboot_types,simple[reboot_begin:reboot_end],reboot_boot))

class ProductUiTests(unittest.TestCase):
    def fixture(self,directory):
        root=Path(directory)
        for relative,_,_ in prepare.HOOKS:
            path=root/relative
            if path.exists():continue
            repo=BASE if relative.startswith(prepare.BASE)else ROOT/prepare.SI
            prefix=prepare.BASE if relative.startswith(prepare.BASE)else prepare.SI
            name=relative[len(prefix):]
            path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes(subprocess.check_output(['git','-C',str(repo),'show','HEAD:'+name]))
        return root
    def test_pinned_apply_is_idempotent_null_default(self):
        with tempfile.TemporaryDirectory()as out:
            root=self.fixture(out);first=prepare.prepare(root,apply=True,check_pins=False)
            self.assertEqual(first,prepare.prepare(root,apply=True,check_pins=False))
            self.assertEqual(first,prepare.prepare(root,apply=False,check_pins=False))
            self.assertEqual(len(first['files']),22)
            self.assertEqual(first['default_binding'],'PianoProductPumpLibNull')
    def test_modified_or_duplicate_patch_refused_before_mutations(self):
        for mutation in('modified','duplicate'):
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory()as out:
                root=self.fixture(out);prepare.prepare(root,apply=True,check_pins=False)
                path=root/(prepare.DISPLAY+'FormDisplay.c');text=path.read_text()
                if mutation=='modified':text=text.replace('return UIEventReturnCore;', 'return UIEventNone;')
                else:text+='\n'+next(new for relative,old,new in prepare.HOOKS if relative==prepare.DISPLAY+'FormDisplay.c' and 'return UIEventReturnCore;'in new)
                path.write_text(text)
                snapshot={p:p.read_bytes()for p in root.rglob('*')if p.is_file()}
                with self.assertRaises(ValueError):prepare.prepare(root,apply=True,check_pins=False)
                self.assertEqual(snapshot,{p:p.read_bytes()for p in root.rglob('*')if p.is_file()})
    def test_actual_source_normal_cleanup(self):
        prepare.prepare(apply=False)
        with tempfile.TemporaryDirectory(prefix='piano-product-ui-')as out:
            out=Path(out);(out/'PianoActualProductUi.h').write_text(generated())
            exe=out/'ui';include=BASE/'MdePkg/Include'
            build=subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-label','-fsanitize=address,undefined','-g','-fno-pie','-no-pie','-I'+str(include),'-I'+str(include/'X64'),'-I'+str(out),str(ROOT/'tests/PianoProductUiTest.c'),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stdout+build.stderr)
            run=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())
if __name__=='__main__':unittest.main()
