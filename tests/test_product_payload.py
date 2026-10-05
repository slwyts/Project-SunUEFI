import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('product_digest',ROOT/'tools/product_payload_digest.py')
digest=importlib.util.module_from_spec(spec);spec.loader.exec_module(digest)


def fixture():
    raw=bytearray(4096)
    def h(offset,value):struct.pack_into('<H',raw,offset,value)
    def w(offset,value):struct.pack_into('<I',raw,offset,value)
    h(0,0x5a4d);w(60,128);w(128,0x4550);h(132,0xaa64);h(134,1);h(148,240);h(150,2)
    h(152,0x20b);w(168,4096);w(184,4096);w(188,512);w(208,8192);w(212,512);h(220,10);w(260,16)
    raw[392:397]=b'.text';w(400,512);w(404,4096);w(408,512);w(412,512);w(428,0x60000020)
    return raw


class ProductPayloadTests(unittest.TestCase):
    def test_actual_c_bounded_registry_and_arm64_compile(self):
        inc=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
        extra=[ROOT/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include',ROOT/'upstream/Mu-Silicium/Mu_Basecore/CryptoPkg/Include']
        with tempfile.TemporaryDirectory(prefix='product-payload-') as tmp:
            folder=Path(tmp);app=folder/'app.efi';app.write_bytes(fixture())
            header,info=digest.digest_header(app);self.assertEqual(info['app_bytes'],4096)
            (folder/'PianoProductSimpleInitDigest.h').write_text(header)
            executable=folder/'test'
            flags=['-I',str(folder),'-I',str(inc),'-I',str(inc/'X64')]
            for path in extra:flags+=['-I',str(path)]
            subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',*flags,str(ROOT/'tests/PianoProductPayloadTest.c'),'-lcrypto','-o',str(executable)],check=True)
            subprocess.run([str(executable),str(app)],check=True)
            flags[flags.index(str(inc/'X64'))]=str(inc/'AArch64')
            subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror',*flags,str(ROOT/'bootprofiles/uefi-app/PianoProductPayload.c')],check=True)


if __name__=='__main__':unittest.main()
