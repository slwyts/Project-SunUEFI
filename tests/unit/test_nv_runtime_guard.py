from pathlib import Path
import sys,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
import prepare_nv_runtime_guard as guard
class RuntimeNvGuardTests(unittest.TestCase):
 def test_exact_standard_entry_before_mor_or_auth_mutation(self):
  guard.prepare(apply=False)
  text=(ROOT/guard.FILE).read_text();begin=text.index('#if PIANO_NV_BOOT_ONLY\n  // The UFS owner');end=text.index('#endif',begin)+len('#endif');fragment=text[begin:end]
  inc=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
  with tempfile.TemporaryDirectory(prefix='nv-guard-')as d:
   d=Path(d);source=d/'guard.c';source.write_text('''#include <assert.h>
#undef NULL
#include <Uefi.h>
static BOOLEAN runtime,existing;static UINTN finds,mor;
typedef struct{UINT32 Attributes;}HEADER;typedef struct{HEADER*CurrPtr;}VARIABLE_POINTER_TRACK;
static HEADER header;static struct{UINTN VariableGlobal;}global;static typeof(global)*mVariableModuleGlobal=&global;
static BOOLEAN AtRuntime(void){return runtime;}
static EFI_STATUS FindVariable(CHAR16*n,EFI_GUID*g,VARIABLE_POINTER_TRACK*v,VOID*c,BOOLEAN all){finds++;v->CurrPtr=&header;return existing?EFI_SUCCESS:EFI_NOT_FOUND;}
static EFI_STATUS Check(UINT32 Attributes){CHAR16*VariableName=L"NV";EFI_GUID Guid={0};EFI_GUID*VendorGuid=&Guid;
'''+fragment+'''
mor++;return EFI_SUCCESS;}
int main(void){runtime=TRUE;existing=TRUE;header.Attributes=EFI_VARIABLE_NON_VOLATILE|EFI_VARIABLE_RUNTIME_ACCESS;
#if PIANO_NV_BOOT_ONLY
assert(Check(EFI_VARIABLE_NON_VOLATILE)==EFI_UNSUPPORTED&&!finds&&!mor);assert(Check(0)==EFI_UNSUPPORTED&&finds==1&&!mor);
header.Attributes=EFI_VARIABLE_RUNTIME_ACCESS;assert(Check(0)==EFI_SUCCESS&&mor==1);runtime=FALSE;assert(Check(EFI_VARIABLE_NON_VOLATILE)==EFI_SUCCESS&&mor==2);
#else
assert(Check(EFI_VARIABLE_NON_VOLATILE)==EFI_SUCCESS&&mor==1);assert(Check(0)==EFI_SUCCESS&&mor==2);
#endif
return 0;}
''')
   for mode in(0,1):
    exe=d/f'guard{mode}';subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-Wno-unused-variable',f'-DPIANO_NV_BOOT_ONLY={mode}','-I'+str(inc),'-I'+str(inc/'X64'),str(source),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
 def test_pinned_and_idempotent(self):
  with tempfile.TemporaryDirectory()as d:
   root=Path(d);path=root/guard.FILE;path.parent.mkdir(parents=True);relative=guard.FILE[len(guard.BASE)+1:]
   path.write_bytes(subprocess.check_output(['git','-C',str(ROOT/guard.BASE),'show','HEAD:'+relative]));first=guard.prepare(root,apply=True,check_pin=False);self.assertEqual(first,guard.prepare(root,apply=True,check_pin=False))
   text=path.read_text().replace('return EFI_UNSUPPORTED;\n    }\n    VARIABLE_POINTER_TRACK NvExisting;','return EFI_SUCCESS;\n    }\n    VARIABLE_POINTER_TRACK NvExisting;');path.write_text(text)
   with self.assertRaises(ValueError):guard.prepare(root,apply=True,check_pin=False)
if __name__=='__main__':unittest.main()
