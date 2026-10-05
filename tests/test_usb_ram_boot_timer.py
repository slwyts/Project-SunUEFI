"""Compile the actual generated RAM-boot timer; temporary prepare, no device ops.

PIANO_RAM_BOOT_TIMER_SOURCE may select a newly generated Stage0 source. The
default uses real prepare in a temporary repository, never current staging.
"""
import hashlib
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = ROOT / "upstream/Mu-Silicium/Platforms/Xiaomi/pianoGuiPkg/Library/Stage0BootManagerLib/Stage0BootManagerLib.c"


def timer_function(source):
    """Retain the production C body, including its exact reset continuation."""
    marker = "STATIC VOID EFIAPI ReturnToAndroid"
    if source.count(marker) != 1:
        raise ValueError("Expected exactly one generated ReturnToAndroid function")
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 0
    quoted = None
    escaped = False
    for index in range(brace, len(source)):
        char = source[index]
        if quoted:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quoted:
                quoted = None
            continue
        if char in ('"', "'"):
            quoted = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if not depth:
                function = source[start:index + 1]
                if "PIANO_USB_SHUTDOWN_GUID" not in function or "Usb->Halt()" not in function:
                    raise ValueError("Source is not the generated RAM-boot halt-only timer")
                return function
    raise ValueError("Unterminated timer function")


PREFIX = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#undef NULL
#include <Uefi.h>
#include "PianoUsbStorageExperiment.h"
#define DEBUG(Arguments) ((void)0)
static EFI_BOOT_SERVICES Bs;
static EFI_RUNTIME_SERVICES Rt;
static EFI_BOOT_SERVICES *gBS=&Bs;
static EFI_RUNTIME_SERVICES *gRT=&Rt;
static jmp_buf Terminal;
static unsigned Case,HaltCalls,ResetCalls,DeadLoops,PollResumed;
static EFI_STATUS Halt(void) {
  HaltCalls++;
  return Case==1?EFI_TIMEOUT:Case==2?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
static PIANO_USB_SHUTDOWN Usb={1,Halt};
static EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,void *Registration,void **Out) {
  EFI_GUID Expected=PIANO_USB_SHUTDOWN_GUID;
  assert(Registration==NULL && Guid->Data1==Expected.Data1);
  *Out=&Usb;
  if(Case==3){*Out=NULL;return EFI_NOT_FOUND;}
  if(Case==4){*Out=NULL;return EFI_WARN_STALE_DATA;}
  if(Case==5)*Out=NULL;
  return EFI_SUCCESS;
}
static void EFIAPI Reset(EFI_RESET_TYPE Type,EFI_STATUS Status,UINTN Bytes,void *Data) {
  assert(Type==EfiResetCold && Status==EFI_SUCCESS && !Bytes && Data==NULL);
  assert(Case==0 || Case==3);
  assert(HaltCalls==(Case==0?1U:0U));
  ResetCalls++; // Deliberately return; the actual timer must fail-stop.
}
void EFIAPI CpuDeadLoop(void) {
  DeadLoops++;
  longjmp(Terminal,1);
}
'''

SUFFIX = r'''
int main(void) {
  Bs.LocateProtocol=Locate;Rt.ResetSystem=Reset;
  for(Case=0;Case<8;Case++) {
    HaltCalls=ResetCalls=DeadLoops=PollResumed=0;
    Usb.Revision=Case==6?2:1;Usb.Halt=Case==7?NULL:Halt;
    if(!setjmp(Terminal)) {
      ReturnToAndroid(NULL,NULL);
      PollResumed++; // This is the suspended DWC/application continuation.
    }
    assert(DeadLoops==1 && PollResumed==0);
    assert(ResetCalls==((Case==0 || Case==3)?1U:0U));
    assert(HaltCalls==(Case<=2?1U:0U));
  }
  puts("Actual generated RAM-boot timer: Halt success then returned Reset fail-stops; Halt error/warning and malformed/unknown protocol never reset; polling never resumes.");
  return 0;
}
'''


class UsbRamBootTimerTests(unittest.TestCase):
    def test_actual_generated_timer_reset_return_and_halt_failures(self):
        with tempfile.TemporaryDirectory(prefix="ram-boot-timer-host-") as directory:
            folder = Path(directory)
            if "PIANO_RAM_BOOT_TIMER_SOURCE" in os.environ:
                source_path=Path(os.environ["PIANO_RAM_BOOT_TIMER_SOURCE"])
            else:
                repo=folder/'repo';(repo/'tools').mkdir(parents=True);(repo/'build').mkdir()
                (repo/'build/native-foundation.fdf.inc').write_text('// host fixture only\n')
                shutil.copytree(ROOT/'platforms/pianoProbePkg',repo/'platforms/pianoProbePkg')
                shutil.copytree(ROOT/'bootprofiles/uefi-app',repo/'bootprofiles/uefi-app')
                spec=importlib.util.spec_from_file_location('timer_prepare',ROOT/'tools/prepare_gui_profile.py')
                profile=importlib.util.module_from_spec(spec);spec.loader.exec_module(profile)
                with patch.object(profile,'__file__',str(repo/'tools/prepare_gui_profile.py')),patch.object(sys,'argv',['prepare','--usb-ram-boot','--return-seconds','120']),contextlib.redirect_stdout(io.StringIO()):
                    profile.main()
                source_path=repo/'platforms/pianoGuiPkg/Library/Stage0BootManagerLib/Stage0BootManagerLib.c'
            source=source_path.read_text();function=timer_function(source)
            c_file = folder / "timer.c"
            binary = folder / "timer"
            c_file.write_text(PREFIX + "\n" + function + "\n" + SUFFIX)
            command = [os.environ.get("CC", "cc"), "-std=gnu11", "-fshort-wchar", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                       "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie"]
            for include in (ROOT / "bootprofiles/uefi-app", ROOT / "upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include",
                            ROOT / "upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/X64"):
                command += ["-I", str(include)]
            command += [str(c_file), "-o", str(binary)]
            build = subprocess.run(command, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True,
                                 env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"})
            self.assertEqual(run.returncode, 0, str(source_path) + "\n" + run.stdout + run.stderr)
            print(run.stdout.strip())
            print("Actual timer source SHA256:", hashlib.sha256(source.encode()).hexdigest())


if __name__ == "__main__":
    unittest.main()
