#!/usr/bin/env python3
"""Install bounded product-only cooperative exits in pinned UiApp and Shell.

All checkpoints use PianoProductPumpLib; legacy DSCs retain its Null binding.
No reset, owner stop, ExitImage, synthetic key or console-cache operation is
introduced. Full upstream modules continue through their existing cleanup.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from prepare_product_pump import ROOT, BASE, SI, transform

PIN = 'bb557081f80f4883ed832e34ab36bdca6ede1e10'
SIMPLEINIT_PIN = '3d66a6e78d519dd050fbebde4db6c5ac933f9aa4'
DISPLAY = BASE + 'MdeModulePkg/Universal/DisplayEngineDxe/'
BROWSER = BASE + 'MdeModulePkg/Universal/SetupBrowserDxe/'
UI = BASE + 'MdeModulePkg/Application/UiApp/'
CDL = BASE + 'MdeModulePkg/Library/CustomizedDisplayLib/'
SHELL = BASE + 'ShellPkg/Application/Shell/'
UEFI_LIB = BASE + 'MdePkg/Library/UefiLib/'
HOOKS = []
def hook(path, old, new):
    HOOKS.append((path, old, new))
def include(path, anchor):
    hook(path, anchor, anchor + '#include <Library/PianoProductPumpLib.h>\n')
for path, anchor in ((DISPLAY+'FormDisplay.c', '#include "FormDisplay.h"\n'),
                     (DISPLAY+'InputHandler.c', '#include "FormDisplay.h"\n'),
                     (DISPLAY+'Popup.c', '#include "FormDisplay.h"\n'),
                     (BROWSER+'Presentation.c', '#include "Setup.h"\n'),
                     (BROWSER+'Setup.c', '#include "Setup.h"\n'),
                     (UI+'FrontPage.c', '#include "FrontPage.h"\n'),
                     (CDL+'CustomizedDisplayLib.c', '#include "CustomizedDisplayLibInternal.h"\n'),
                     (CDL+'CustomizedDisplayLibInternal.c', '#include "CustomizedDisplayLibInternal.h"\n'),
                     (SHELL+'Shell.c', '#include "Shell.h"\n'),
                     (SHELL+'FileHandleWrappers.c', '#include "Shell.h"\n'),
                     (SHELL+'ConsoleLogger.c', '#include "Shell.h"\n'),
                     (UEFI_LIB+'Console.c', '#include "UefiLibInternal.h"\n')):
    include(path, anchor)
for path in (DISPLAY+'DisplayEngineDxe.inf', BROWSER+'SetupBrowserDxe.inf',
             UI+'UiApp.inf', CDL+'CustomizedDisplayLib.inf', SHELL+'Shell.inf', UEFI_LIB+'UefiLib.inf'):
    hook(path, '[LibraryClasses]\n', '[LibraryClasses]\n  PianoProductPumpLib\n')

# Wait abort is a protocol state, never an invented keyboard event.
hook(DISPLAY+'FormDisplay.h', '  UIEventDriver\n', '  UIEventDriver,\n  UIEventReturnCore\n')
hook(DISPLAY+'FormDisplay.c',
     '  Status = gBS->WaitForEvent (EventNum, WaitList, &Index);\n  ASSERT_EFI_ERROR (Status);\n',
     '  Status = gBS->WaitForEvent (EventNum, WaitList, &Index);\n'
     '  if (PianoProductReturnCoreRequested ()) {\n'
     '    if (TimerEvent != NULL) {\n      gBS->CloseEvent (TimerEvent);\n    }\n'
     '    return UIEventReturnCore;\n  }\n  ASSERT_EFI_ERROR (Status);\n')
hook(DISPLAY+'FormDisplay.c', '      case CfReadKey:\n        ControlFlag = CfScreenOperation;\n',
     '      case CfReadKey:\n'
     '        PianoProductPumpApplication (PIANO_PRODUCT_PUMP_APP, 1000);\n'
     '        if (PianoProductReturnCoreRequested ()) {\n'
     '          gUserInput->Action = BROWSER_ACTION_NONE;\n'
     '          ControlFlag = CfExit;\n          break;\n        }\n'
     '        ControlFlag = CfScreenOperation;\n')
hook(DISPLAY+'FormDisplay.c', '        if (EventType == UIEventDriver) {\n',
     '        if (EventType == UIEventReturnCore) {\n'
     '          gUserInput->Action = BROWSER_ACTION_NONE;\n'
     '          ControlFlag = CfExit;\n          break;\n        }\n\n'
     '        if (EventType == UIEventDriver) {\n')
# Browser-level exit bypasses UI action processing/ExitHandler, discards only
# uncommitted browser data, then follows FORM_CLOSE/unregister normal cleanup.
hook(BROWSER+'Presentation.c',
     '  CheckConfigAccess (gCurrentSelection->FormSet);\n\n  Status = ProcessUserInput (&UserInput);',
     '  if (PianoProductReturnCoreRequested ()) {\n'
     '    if (UserInput.InputValue.Buffer != NULL &&\n'
     '        (UserInput.SelectedStatement == NULL ||\n'
     '         UserInput.InputValue.Buffer != UserInput.SelectedStatement->CurrentValue.Buffer)) {\n'
     '      ZeroMem (UserInput.InputValue.Buffer, UserInput.InputValue.BufferLen);\n'
     '      FreePool (UserInput.InputValue.Buffer);\n    }\n'
     '    FreeDisplayFormData ();\n    return EFI_SUCCESS;\n  }\n\n'
     '  CheckConfigAccess (gCurrentSelection->FormSet);\n\n  Status = ProcessUserInput (&UserInput);')
hook(BROWSER+'Presentation.c',
     '    Status = DisplayForm ();\n    if (EFI_ERROR (Status)) {\n      goto Done;\n    }\n',
     '    Status = DisplayForm ();\n'
     '    if (PianoProductReturnCoreRequested ()) {\n'
     '      DiscardForm (Selection->FormSet, Selection->Form, SystemLevel);\n'
     '      Selection->Statement = NULL;\n      Selection->Action = UI_ACTION_EXIT;\n'
     '      gExitRequired = FALSE;\n      Status = EFI_SUCCESS;\n'
     '    } else if (EFI_ERROR (Status)) {\n      goto Done;\n    }\n')
hook(BROWSER+'Setup.c', '    FreePool (Selection);\n  }\n\n  if (ActionRequest != NULL)',
     '    FreePool (Selection);\n'
     '    if (PianoProductReturnCoreRequested ()) {\n      break;\n    }\n'
     '  }\n\n  if (ActionRequest != NULL)')
hook(UI+'FrontPage.c', '  FreeFrontPage ();\n\n  //\n  // Will leave browser,',
     '  FreeFrontPage ();\n'
     '  if (PianoProductReturnCoreRequested ()) {\n    return;\n  }\n\n'
     '  //\n  // Will leave browser,')
hook(UI+'FrontPage.c', '    } while (Key.UnicodeChar != CHAR_CARRIAGE_RETURN);\n\n    FreePool (StringBuffer1);',
     '    } while (!PianoProductReturnCoreRequested () && Key.UnicodeChar != CHAR_CARRIAGE_RETURN);\n\n'
     '    FreePool (StringBuffer1);')
hook(UI+'FrontPage.c', '    FreePool (StringBuffer2);\n\n    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);',
     '    FreePool (StringBuffer2);\n'
     '    if (PianoProductReturnCoreRequested ()) {\n      return;\n    }\n\n'
     '    if (PianoProductRebootManaged ()) {\n'
     '      EFI_STATUS RequestStatus = PianoProductRequestReboot ();\n'
     '      if (RequestStatus != EFI_SUCCESS) {\n'
     '        DEBUG ((DEBUG_ERROR, "Piano product reboot request failed: %r\\n", RequestStatus));\n'
     '      }\n      return;\n    }\n'
     '    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);')
hook(UEFI_LIB+'Console.c',
     '    while (TRUE) {\n      Status = gST->ConIn->ReadKeyStroke (gST->ConIn, Key);',
     '    while (TRUE) {\n'
     '      PianoProductPumpApplication (PIANO_PRODUCT_PUMP_APP, 1000);\n'
     '      if (PianoProductReturnCoreRequested ()) {\n        break;\n      }\n'
     '      Status = gST->ConIn->ReadKeyStroke (gST->ConIn, Key);')

# Shared input helper can unwind editing/popups without synthesizing ESC.
hook(CDL+'CustomizedDisplayLibInternal.c',
     '  while (TRUE) {\n    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, Key);',
     '  while (TRUE) {\n'
     '    PianoProductPumpApplication (PIANO_PRODUCT_PUMP_APP, 1000);\n'
     '    if (PianoProductReturnCoreRequested ()) {\n      return EFI_ABORTED;\n    }\n'
     '    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, Key);')
hook(CDL+'CustomizedDisplayLib.c',
     '    Status = WaitForKeyStroke (&KeyValue);\n    ASSERT_EFI_ERROR (Status);\n    CopyMem (Key, &KeyValue, sizeof (EFI_INPUT_KEY));',
     '    Status = WaitForKeyStroke (&KeyValue);\n'
     '    if (PianoProductReturnCoreRequested ()) {\n'
     '      gST->ConOut->SetAttribute (gST->ConOut, CurrentAttribute);\n'
     '      gST->ConOut->EnableCursor (gST->ConOut, CursorVisible);\n      return;\n    }\n'
     '    ASSERT_EFI_ERROR (Status);\n    CopyMem (Key, &KeyValue, sizeof (EFI_INPUT_KEY));')
hook(CDL+'CustomizedDisplayLib.c', '   (Key.ScanCode != SCAN_ESC) &&\n',
     '   !PianoProductReturnCoreRequested () &&\n   (Key.ScanCode != SCAN_ESC) &&\n')
hook(CDL+'CustomizedDisplayLib.c', '  if (Key.ScanCode == SCAN_ESC) {\n    return BROWSER_ACTION_NONE;',
     '  if (PianoProductReturnCoreRequested ()) {\n    return BROWSER_ACTION_DISCARD;\n  }\n'
     '  if (Key.ScanCode == SCAN_ESC) {\n    return BROWSER_ACTION_NONE;')
hook(DISPLAY+'InputHandler.c', '    Status = WaitForKeyStroke (&Key);\n    ASSERT_EFI_ERROR (Status);',
     '    Status = WaitForKeyStroke (&Key);\n'
     '    if (PianoProductReturnCoreRequested ()) {\n'
     '      FreePool (TempString);\n      FreePool (BufferedString);\n'
     '      gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));\n'
     '      gST->ConOut->EnableCursor (gST->ConOut, CursorVisible);\n      return EFI_ABORTED;\n    }\n'
     '    ASSERT_EFI_ERROR (Status);')
hook(DISPLAY+'InputHandler.c', '          } while (Key.UnicodeChar != CHAR_CARRIAGE_RETURN);',
     '          } while (!PianoProductReturnCoreRequested () && Key.UnicodeChar != CHAR_CARRIAGE_RETURN);')
hook(DISPLAY+'InputHandler.c', '    WaitForKeyStroke (&Key);\n\nTheKey2:',
     '    WaitForKeyStroke (&Key);\n'
     '    if (PianoProductReturnCoreRequested ()) {\n      return EFI_ABORTED;\n    }\n\nTheKey2:')
hook(DISPLAY+'InputHandler.c', '    WaitForKeyStroke (&Key);\n\nTheKey:',
     '    WaitForKeyStroke (&Key);\n'
     '    if (PianoProductReturnCoreRequested ()) {\n      goto PianoProductCancelOrderedList;\n    }\n\nTheKey:')
hook(DISPLAY+'InputHandler.c', '          case SCAN_ESC:\n            gST->ConOut->SetAttribute (gST->ConOut, SavedAttribute);',
     '          case SCAN_ESC:\nPianoProductCancelOrderedList:\n'
     '            gST->ConOut->SetAttribute (gST->ConOut, SavedAttribute);')
hook(DISPLAY+'Popup.c', '    Status = WaitForKeyStroke (&KeyValue);\n    ASSERT_EFI_ERROR (Status);',
     '    Status = WaitForKeyStroke (&KeyValue);\n'
     '    if (PianoProductReturnCoreRequested ()) {\n      return;\n    }\n'
     '    ASSERT_EFI_ERROR (Status);')
hook(DISPLAY+'FormDisplay.c',
     '  } while (((Key.UnicodeChar | UPPER_LOWER_CASE_OFFSET) != (gConfirmOptYes[0] | UPPER_LOWER_CASE_OFFSET)) &&',
     '  } while (!PianoProductReturnCoreRequested () &&\n'
     '           ((Key.UnicodeChar | UPPER_LOWER_CASE_OFFSET) != (gConfirmOptYes[0] | UPPER_LOWER_CASE_OFFSET)) &&')
hook(DISPLAY+'FormDisplay.c',
     '  if ((Key.UnicodeChar | UPPER_LOWER_CASE_OFFSET) == (gConfirmOptYes[0] | UPPER_LOWER_CASE_OFFSET)) {',
     '  if (!PianoProductReturnCoreRequested () &&\n'
     '      (Key.UnicodeChar | UPPER_LOWER_CASE_OFFSET) == (gConfirmOptYes[0] | UPPER_LOWER_CASE_OFFSET)) {')
hook(DISPLAY+'FormDisplay.c',
     '      } while (((Key.UnicodeChar | UPPER_LOWER_CASE_OFFSET) != (DiscardChange | UPPER_LOWER_CASE_OFFSET)) &&',
     '      } while (!PianoProductReturnCoreRequested () &&\n'
     '               ((Key.UnicodeChar | UPPER_LOWER_CASE_OFFSET) != (DiscardChange | UPPER_LOWER_CASE_OFFSET)) &&')
hook(DISPLAY+'FormDisplay.c',
     '      if ((Key.UnicodeChar | UPPER_LOWER_CASE_OFFSET) == (DiscardChange | UPPER_LOWER_CASE_OFFSET)) {',
     '      if (PianoProductReturnCoreRequested ()) {\n        gUserInput->Action = BROWSER_ACTION_NONE;\n'
     '      } else if ((Key.UnicodeChar | UPPER_LOWER_CASE_OFFSET) == (DiscardChange | UPPER_LOWER_CASE_OFFSET)) {')
hook(DISPLAY+'FormDisplay.c', '        } while (Key.UnicodeChar != CHAR_CARRIAGE_RETURN);',
     '        } while (!PianoProductReturnCoreRequested () && Key.UnicodeChar != CHAR_CARRIAGE_RETURN);')
hook(DISPLAY+'FormDisplay.c',
     '          Status = gBS->WaitForEvent (2, WaitList, &Index);\n          ASSERT_EFI_ERROR (Status);',
     '          Status = gBS->WaitForEvent (2, WaitList, &Index);\n'
     '          if (PianoProductReturnCoreRequested ()) {\n            break;\n          }\n'
     '          ASSERT_EFI_ERROR (Status);')

# Shell read abort falls through the existing tab-completion cleanup; prompt
# restores its temporary buffer list and frees the line before main teardown.
hook(SHELL+'FileHandleWrappers.c',
     '    gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &EventIndex);\n    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);',
     '    PianoProductPumpApplication (PIANO_PRODUCT_PUMP_APP, 1000);\n'
     '    if (!PianoProductReturnCoreRequested ()) {\n'
     '      gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &EventIndex);\n    }\n'
     '    if (PianoProductReturnCoreRequested ()) {\n      Status = EFI_ABORTED;\n'
     '    } else {\n      Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);\n    }')
hook(SHELL+'Shell.c', '        } while (!ShellCommandGetExit ());',
     '        } while (!ShellCommandGetExit () && !PianoProductReturnCoreRequested ());')
hook(SHELL+'Shell.c', '  if (ShellCommandGetExit ()) {\n    return ((EFI_STATUS)ShellCommandGetExitCode ());',
     '  if (PianoProductReturnCoreRequested ()) {\n    return EFI_SUCCESS;\n  }\n'
     '  if (ShellCommandGetExit ()) {\n    return ((EFI_STATUS)ShellCommandGetExitCode ());')
hook(SHELL+'Shell.c', '  if (!EFI_ERROR (Status)) {\n    //\n    // Reset the CTRL-C event just before running the command',
     '  if (!EFI_ERROR (Status) && !PianoProductReturnCoreRequested ()) {\n    //\n'
     '    // Reset the CTRL-C event just before running the command')
hook(SHELL+'ConsoleLogger.c',
     '      Status = gBS->WaitForEvent (1, &TxtInEx->WaitForKeyEx, &EventIndex);\n      ASSERT_EFI_ERROR (Status);',
     '      Status = gBS->WaitForEvent (1, &TxtInEx->WaitForKeyEx, &EventIndex);\n'
     '      if (PianoProductReturnCoreRequested ()) {\n        return EFI_ABORTED;\n      }\n'
     '      ASSERT_EFI_ERROR (Status);')

# UEFI SimpleInit's reboot menu already exits its GUI and calls adv_reboot from
# the normal after_exit boot dispatcher. Keep Linux/native Null behavior intact.
include(SI+'src/lib/reboot.c', '#include<Library/BaseLib.h>\n')
hook(SI+'src/lib/SimpleInitLib.inf', '[LibraryClasses]\n', '[LibraryClasses]\n  PianoProductPumpLib\n')
hook(SI+'src/lib/reboot.c', 'int adv_reboot(enum reboot_cmd cmd,char*data){\n\tUINTN s=0;',
     'int adv_reboot(enum reboot_cmd cmd,char*data){\n'
     '\tif(PianoProductRebootManaged()){\n'
     '\t\tif(cmd!=REBOOT_RESTART&&cmd!=REBOOT_COLD)ERET(EOPNOTSUPP);\n'
     '\t\tEFI_STATUS status=PianoProductRequestReboot();\n'
     '\t\tif(status!=EFI_SUCCESS)ERET(EIO);\n'
     '\t\treturn 0;\n\t}\n\tUINTN s=0;')
hook(SI+'src/boot/reboot_uefi.c', '#include<string.h>\n', '#include<string.h>\n#include<stdlib.h>\n')
hook(SI+'src/boot/reboot_uefi.c', '\tadv_reboot(cmd,data);\n\ttlog_warn("reset system failed");',
     '\tif(adv_reboot(cmd,data)==0){\n\t\tif(data)free(data);\n\t\treturn 0;\n\t}\n'
     '\ttlog_warn("reset system failed");')

def prepare(root=ROOT, apply=False, check_pins=True):
    if check_pins:
        actual = subprocess.check_output(['git', '-C', str(root/BASE), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != PIN:
            raise ValueError('unexpected Mu_Basecore pin')
        actual = subprocess.check_output(['git', '-C', str(root/SI), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != SIMPLEINIT_PIN:
            raise ValueError('unexpected SimpleInit pin')
    desired, newline = {}, {}
    for relative, old, new in HOOKS:
        path = root/relative
        if path not in desired:
            raw=path.read_bytes()
            newline[path]=b'\r\n' if b'\r\n' in raw else b'\n'
            desired[path]=raw.decode().replace('\r\n','\n')
        desired[path]=transform(desired[path],old,new,relative)
    outputs={path:value.replace('\n',newline[path].decode()).encode() for path,value in desired.items()}
    if not apply:
        for path,value in outputs.items():
            if path.read_bytes()!=value:
                raise ValueError('product UI hook not prepared: '+str(path.relative_to(root)))
    else:
        for path,value in outputs.items():
            if path.read_bytes()!=value:
                path.write_bytes(value)
    hashes={str(path.relative_to(root)):hashlib.sha256(value).hexdigest() for path,value in sorted(outputs.items())}
    return {'pin':PIN,'simpleinit_pin':SIMPLEINIT_PIN,'files':hashes,'sha256':hashlib.sha256(json.dumps(hashes,sort_keys=True).encode()).hexdigest(),
            'default_binding':'PianoProductPumpLibNull','product_requires_real_library_override':True}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('operation',choices=('apply','verify'))
    p.add_argument('--manifest',type=Path)
    args=p.parse_args()
    result=json.dumps(prepare(apply=args.operation=='apply'),indent=2)+'\n'
    if args.manifest:
        args.manifest.parent.mkdir(parents=True,exist_ok=True)
        args.manifest.write_text(result)
    print(result,end='')
if __name__=='__main__':main()
