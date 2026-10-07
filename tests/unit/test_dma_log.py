import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
#!/usr/bin/env python3
import unittest
import zlib
from check_dma_log import check

def trace():
    lines=[];sequence=0
    commands=['NOP']+['QUERY_READ_DEVICE_DESCRIPTOR']*5+['QUERY_READ_CURRENT_POWER_MODE']*2
    commands+=['RESUME_DEVICE_ACTIVE_NO_DATA','REPORT_LUNS']+['READ_CAPACITY_16']*6+['READ_LBA_10']*21
    for phase,count in {'allocate':7,'map':6,'submit':102,'complete':102,'unmap':3}.items():
        for index in range(count):
            iova=0x40000000 if phase=='submit' and index<37 else 0x40001000
            cmd=commands[index] if phase=='submit' and index<37 else '-'
            body=(f'phase={phase} seq={sequence} dev=ufs sid=60 pa=D3000000 iova={iova:X} bytes=1024 '
                  f'reserved=4096 dir=bidirectional align=4096 attrs=8 cache=none status=Success cmd={cmd}')
            sequence+=1
            for prefix in ('SUNUEFI_DMA','SUNUEFI_DMA_COPY'):
                lines.append(f'{prefix} {body} crc32={zlib.crc32(body.encode()):08X}')
    return '\n'.join(lines)

class Integrity(unittest.TestCase):
    def test_complete(self):self.assertEqual(check(trace())['records_checked'],220)
    def test_changed_payload_recovers(self):
        r=check(trace().replace('phase=allocate','phase=alloc`te',1))
        self.assertEqual(r['damaged_copies_rejected'],1);self.assertEqual(r['records_with_one_valid_copy'],[0])
    def test_missing_prefix_recovers(self):
        self.assertEqual(check(trace().replace('SUNUEFI_DMA phase=allocate','SUNUEFI_DMB phase=allocate',1))['records_with_one_valid_copy'],[0])
    def test_both_copies_invalid(self):
        with self.assertRaises(ValueError):check(trace().replace('phase=allocate seq=0 ','phase=alloc`te seq=0 '))
    def test_missing_both_copies(self):
        with self.assertRaises(ValueError):check('\n'.join(trace().splitlines()[2:]))

if __name__=='__main__':unittest.main()
