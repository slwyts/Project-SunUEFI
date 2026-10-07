"""Execute exact stock-fastboot log-check source with file/command boundaries."""
import ast
import hashlib
from pathlib import Path
import tempfile
import unittest
import zlib
ROOT=Path(__file__).resolve().parents[2]
class CheckFailed(RuntimeError):pass

def actual_log_code(name):
    tree=ast.parse((ROOT/'tools'/name).read_text())
    if name=='check_product_fastboot.py':
        node=next(n for n in ast.walk(tree)if isinstance(n,ast.FunctionDef)and n.name=='ramlog')
        return compile(ast.fix_missing_locations(ast.Module(body=[node],type_ignores=[])),name,'exec'),True
    for node in ast.walk(tree):
        if not isinstance(node,ast.Try):continue
        starts=[i for i,n in enumerate(node.body)if isinstance(n,ast.Expr)and isinstance(n.value,ast.Call)and isinstance(n.value.func,ast.Name)and n.value.func.id=='command'and [getattr(x,'value',None)for x in n.value.args]==['oem','ramlog']]
        if starts:
            start=starts[0];end=next(i for i in range(start+1,len(node.body))if isinstance(node.body[i],ast.Expr)and isinstance(node.body[i].value,ast.Call)and [getattr(x,'value',None)for x in node.body[i].value.args]==['oem','discard'])
            return compile(ast.fix_missing_locations(ast.Module(body=node.body[start:end+1],type_ignores=[])),name,'exec'),False
    raise AssertionError('Actual log checker absent')

class UsbLogBoundTests(unittest.TestCase):
    def test_both_real_stockfastboot_checkers_accept_256k_and_reject_larger(self):
        for tool in ('check_product_fastboot.py','check_fastboot_debug.py'):
            code,function=actual_log_code(tool)
            for size in (65537,262144,262145):
                with self.subTest(tool=tool,size=size),tempfile.TemporaryDirectory(prefix='log-bound-')as directory:
                    data=bytes((i*17)&255 for i in range(size));meta={'SunUEFI:log-size':size,'SunUEFI:log-crc32':zlib.crc32(data),'SunUEFI:log-generation':9};calls=[]
                    def command(*arguments):
                        calls.append(arguments)
                        if arguments[0]=='get_staged':Path(arguments[1]).write_bytes(data)
                        return ''
                    namespace={'command':command,'hex_variable':lambda n:meta[n],'variable':lambda n:f'{meta[n]:x}','out':Path(directory),'result':{},'CheckFailed':CheckFailed,'zlib':zlib,'hashlib':hashlib}
                    if size>262144:
                        with self.assertRaisesRegex(CheckFailed,'metadata'):
                            exec(code,namespace)
                            if function:namespace['ramlog']('test')
                        self.assertFalse(any(x[0]=='get_staged'for x in calls))
                    else:
                        exec(code,namespace);record=namespace['ramlog']('test')if function else namespace['result']['ram_log']
                        self.assertEqual(record['bytes'],size);self.assertEqual(record['sha256'],hashlib.sha256(data).hexdigest())
                        self.assertTrue(any(x[0]=='get_staged'for x in calls))
    def test_ufs_fetch_limit_is_unchanged_in_actual_firmware_and_checker(self):
        header=(ROOT/'uefi/core/PianoFastboot.h').read_text();firmware=(ROOT/'uefi/core/PianoFastboot.c').read_text();checker=(ROOT/'tools/check_product_fastboot.py').read_text()
        self.assertIn('#define PIANO_FASTBOOT_MAX_FETCH 65536U',header)
        self.assertIn('max-fetch-size',firmware);self.assertIn('OKAY0x00010000',firmware)
        self.assertIn('fetch_limit != 65536',checker)
if __name__=='__main__':unittest.main()
