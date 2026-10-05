from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
INC=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'


class KeysLifecycleTests(unittest.TestCase):
    def test_actual_gpio_driver_stop(self):
        with tempfile.TemporaryDirectory(prefix='keys-lifecycle-') as tmp:
            exe=str(Path(tmp)/'stop')
            subprocess.run(['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fsanitize=address,undefined','-fno-pie','-no-pie',
                '-I',str(INC),'-I',str(INC/'X64'),str(ROOT/'tests/PianoKeysLifecycleTest.c'),'-o',exe],check=True)
            subprocess.run([exe],check=True)


if __name__=='__main__':unittest.main()
