"""Actual limited clock collector and actual guard; never physical register IO."""
import os
import hashlib
import re
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2];INC=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include'
PRINT=ROOT/'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Library/BasePrintLib'
SRC=ROOT/'uefi/core/PianoDisplayClockObserve.c'
QCOM=ROOT/'upstream/Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include'
class DisplayClockObserveTests(unittest.TestCase):
    def test_actual_guard_and_required_unfinished_collector(self):
        with tempfile.TemporaryDirectory(prefix='display-clock-')as directory:
            exe=Path(directory)/'clock'
            subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-implicit-fallthrough',
                '-fshort-wchar','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                '-DNO_MSABI_VA_FUNCS','-D_PCD_GET_MODE_32_PcdMaximumAsciiStringLength=0','-D_PCD_GET_MODE_32_PcdMaximumUnicodeStringLength=0',
                '-I'+str(INC),'-I'+str(INC/'X64'),'-I'+str(QCOM),'-I'+str(ROOT/'uefi/components/guarded-read'),str(ROOT/'tests/native/PianoDisplayClockObserveTest.c'),str(PRINT/'PrintLib.c'),str(PRINT/'PrintLibInternal.c'),'-o',str(exe)],check=True)
            run=subprocess.run([str(exe)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});self.assertEqual(run.returncode,0,run.stdout+run.stderr);print(run.stdout.strip())
    def test_actual_aarch64_and_fixed_declarations(self):
        subprocess.run([str(ROOT/'build/host-tools/usr/bin/clang'),'--target=aarch64-windows-msvc','-ffreestanding','-fshort-wchar','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
            '-I'+str(INC),'-I'+str(INC/'AArch64'),'-I'+str(QCOM),'-I'+str(ROOT/'uefi/components/guarded-read'),str(SRC)],check=True)
        source=SRC.read_text()
        self.assertIn('required_unfinished=1',source);self.assertIn('hardware_ready=0',source)
        for forbidden in ('MmioWrite','SetMemory','WriteBack','MapRegion','SetMode(','PianoGuardedRead32(Token,mSpecs'):
            self.assertNotIn(forbidden,source)
    def test_actual_lease_reader_guard_and_held_observer(self):
        # Reuse the actual pipeline's machine-boundary fixture, adding the
        # collector as a real linked consumer. No Lease/Reader algorithm is
        # replaced and no rail proof or controller register load is invented.
        from test_display_clock_pipeline import function,object_comparison
        from piano_mu_map_fixture import write_mu_map_fixture
        owner=ROOT/'uefi/core/PianoProductDisplayOwner.c'
        fixture=(ROOT/'tests/native/PianoDisplayClockPipelineTest.c').read_text()
        def replace_once(old,new):
            nonlocal fixture
            self.assertEqual(fixture.count(old),1,'actual pipeline fixture anchor drift: '+old[:80])
            fixture=fixture.replace(old,new)
        for path in ('uefi/components/guarded-read/PianoGuardedRead.c','uefi/core/PianoDisplayClockRead.h','uefi/components/display-rail/PianoDisplayNonGdscClock.h'):
            replace_once('"../'+path+'"','"'+str(ROOT/path)+'"')
        replace_once('#include "ActualClockObjectComparison.h"','#include "ActualClockObjectComparison.h"\n#include "'+str(ROOT/'uefi/core/PianoDisplayClockObserve.h')+'"\n#include "'+str(ROOT/'uefi/core/PianoProductDisplayOwner.h')+'"')
        replace_once('static BOOLEAN GlobalAlive(VOID){return Services;}',
            'BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}\n'
            'BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(VOID)L;return TRUE;}\n'
            'VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *Fmt,...){(VOID)L;(VOID)Fmt;assert(Services);}\n'
            'static BOOLEAN GlobalAlive(VOID){return Services;}')
        replace_once('BOOLEAN Gcc=A==0x127000;', 'BOOLEAN Gcc=A==0x127000||A==0xaf08000||A==0xaf09000;')
        replace_once('(A==0x127000?0:0xffULL<<56)', '((A==0x127000||A==0xaf08000||A==0xaf09000)?0:0xffULL<<56)')
        replace_once('assert(InGcc);GccLoads++;', 'assert(InGcc||m.Config.Context==PianoDisplayClockGetReport());GccLoads++;')
        replace_once('  E=PianoDisplayClockLeaseRelease(&mDisplay.Lease);',
            '  EFI_STATUS Observed=PianoProductDisplayClockObserve("actual-product-held",GlobalAlive);\n'
            '  CONST PIANO_DISPLAY_CLOCK_SNAPSHOT *Snapshot=&PianoDisplayClockGetReport()->Snapshot[0];\n'
            '  assert(Observed==EFI_NOT_READY&&Snapshot->ClockReferenceHeld&&!Snapshot->ControllerBusHeld&&!Snapshot->DpuDomainClockHeld);\n'
            '  assert(Snapshot->HeldBorrow==EFI_SUCCESS&&Snapshot->HeldScope==EFI_SUCCESS&&Snapshot->HeldReturn==EFI_SUCCESS&&Snapshot->DispccMappingQualified);\n'
            '  assert(Snapshot->HeldBefore.Token==Snapshot->HeldAfter.Token&&Snapshot->HeldBefore.ClockId==FIXTURE_ID&&!mDisplay.Lease.BorrowToken);\n'
            '  assert(!mDisplay.Lease.Report.Retained&&EnableCalls==1&&!DisableCalls&&GccCalls==4&&GccLoads==20);\n'
            '  for(UINT32 I=2;I<16;++I)assert(!Snapshot->Register[I].Attempts&&Snapshot->Register[I].ReadStatus[0]==EFI_NOT_STARTED);\n'
            '  assert(Snapshot->Register[2].Skip==PianoClockControllerPowerUnproven);\n'
            '  E=PianoDisplayClockLeaseRelease(&mDisplay.Lease);')
        replace_once('DisableCalls==1&&GccCalls==3&&GccLoads==12','DisableCalls==1&&GccCalls==5&&GccLoads==24')
        # Only the two genuinely successful acquisition paths: newly allocated
        # native client entry and inherited refs. The original pipeline owns
        # its full 20-case failure matrix and remains byte-for-byte unchanged.
        replace_once('I<20;', 'I<2;')
        with tempfile.TemporaryDirectory(prefix='clock-held-joint-')as directory:
            d=Path(directory);text=owner.read_text()
            report=re.search(r'typedef struct \{(?:(?!typedef struct).)*?\} PIANO_PRODUCT_DISPLAY_STARTUP_REPORT;',
                (ROOT/'uefi/core/PianoProductOwners.h').read_text(),re.S)
            self.assertIsNotNone(report)
            (d/'ActualDisplayGcc.h').write_text('// Verbatim adapter '+hashlib.sha256(owner.read_bytes()).hexdigest()+'\n'+
                '\n'.join(function(text,name)for name in ('ReadAlive','LeaseAlive','ReadGcc','PianoProductDisplayOwnerRetained','PianoProductDisplayClockObserve','CopyAcquire'))+'\n')
            (d/'ActualClockObjectComparison.h').write_text(object_comparison((ROOT/'uefi/core/PianoDisplayClockRead.c').read_text()))
            write_mu_map_fixture(ROOT,d/'ActualMuMemoryMap.h')
            test=d/'ActualHeldConsumer.c';test.write_text(fixture);exe=d/'held'
            cmd=['cc','-std=gnu11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-g',
                '-DPIANO_DISPLAY_CLOCK_HOST_TEST=1','-fsanitize=undefined','-fno-sanitize-recover=all','-fno-pie','-no-pie']
            for path in (INC,INC/'X64',INC.parent.parent/'CryptoPkg/Include',QCOM,ROOT/'uefi/components/guarded-read',ROOT/'uefi/core',d):cmd+=['-I',str(path)]
            cmd+=[str(test),str(ROOT/'uefi/core/PianoDisplayClockRead.c'),str(ROOT/'uefi/core/PianoDisplayClockLease.c'),str(SRC),'-lcrypto','-o',str(exe)]
            result=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(exe),str(ROOT/'upstream/Mu-Silicium/Binaries/piano/ProductFoundation/ClockDxe/ClockDxe.efi')],capture_output=True,text=True,
                env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            print('Actual Lease+Reader+Guard+held collector: two acquisition paths, fresh borrow/return, ref unchanged until retirement; controller/DPU loads zero')
if __name__=='__main__':unittest.main()
