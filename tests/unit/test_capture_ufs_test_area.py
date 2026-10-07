"""Offline GPT/backup refusal tests. Every adb subprocess is replaced by a mock."""
import contextlib
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import struct
import tempfile
import unittest
from unittest import mock
import zlib

ROOT=Path(__file__).resolve().parents[2]
SPEC=importlib.util.spec_from_file_location('capture_ufs_test_area',ROOT/'tools/capture_ufs_test_area.py')
tool=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(tool)
LAST_LBA=tool.CAPACITY//tool.BLOCK-1
FIRST_USABLE=6
LAST_USABLE=378872
ARRAY_BLOCKS=3
BACKUP_ARRAY=378873
ZERO_GAP=bytes((tool.LAST-tool.FIRST+1)*tool.BLOCK)

def make_entries(active=79,width=128):
    data=bytearray(96*width)
    for i in range(active):
        offset=i*width;data[offset:offset+16]=(i+1).to_bytes(16,'little')
        data[offset+16:offset+32]=(1000+i).to_bytes(16,'little')
        struct.pack_into('<QQ',data,offset+32,10+i*10,14+i*10)
    # A Qualcomm-style inactive last_parti marker is not an active partition.
    struct.pack_into('<QQ',data,80*width+32,tool.FIRST,tool.LAST)
    data[80*width+56:80*width+78]='last_parti'.encode('utf-16le')+b'\0\0'
    return bytes(data)

def make_header(entries,where=1,**changes):
    values=dict(revision=0x10000,size=92,reserved=0,current=where,
        alternate=LAST_LBA if where==1 else 1,first=FIRST_USABLE,final=LAST_USABLE,
        entry_lba=2 if where==1 else BACKUP_ARRAY,count=96,width=128,
        array_crc=zlib.crc32(entries),disk_guid=(321).to_bytes(16,'little'))
    values.update(changes);data=bytearray(tool.BLOCK);data[:8]=b'EFI PART'
    struct.pack_into('<IIII',data,8,values['revision'],values['size'],0,values['reserved'])
    struct.pack_into('<QQQQ',data,24,values['current'],values['alternate'],values['first'],values['final'])
    data[56:72]=values['disk_guid']
    struct.pack_into('<QIII',data,72,values['entry_lba'],values['count'],values['width'],values['array_crc'])
    if 0<=values['size']<=len(data):struct.pack_into('<I',data,16,zlib.crc32(data[:values['size']]))
    return bytes(data)

def pair(entries):
    return tool.header(make_header(entries),1,LAST_LBA),tool.header(make_header(entries,LAST_LBA),LAST_LBA,LAST_LBA)

class GptValidationTests(unittest.TestCase):
    def setUp(self):self.entries=make_entries();self.primary,self.backup=pair(self.entries)
    def verify(self,entries):
        primary,backup=pair(entries);return tool.validate_gap(primary,backup,entries,entries)
    def test_real_geometry_and_inactive_marker(self):
        self.assertEqual(79,tool.validate_gap(self.primary,self.backup,self.entries,self.entries))
        self.assertEqual(3,self.primary['blocks']);self.assertEqual(12288,self.primary['bytes'])
        self.assertEqual(2,self.primary['entry_lba']);self.assertEqual(BACKUP_ARRAY,self.backup['entry_lba'])
        self.assertEqual(14*1024*1024,len(ZERO_GAP))
    def test_truncated_or_wrong_signature(self):
        valid=make_header(self.entries)
        for data in (valid[:-1],valid+bytes(1),b'BAD PART'+valid[8:]):
            with self.subTest(size=len(data)),self.assertRaisesRegex(ValueError,'Incomplete'):tool.header(data,1,LAST_LBA)
    def test_header_crc_or_location_mismatch(self):
        data=bytearray(make_header(self.entries));data[40]^=1
        with self.assertRaisesRegex(ValueError,'CRC/location'):tool.header(data,1,LAST_LBA)
        for changes in ({'current':2},{'alternate':LAST_LBA-1}):
            with self.subTest(changes=changes),self.assertRaisesRegex(ValueError,'CRC/location'):
                tool.header(make_header(self.entries,**changes),1,LAST_LBA)
        with self.assertRaisesRegex(ValueError,'CRC/location'):
            tool.header(make_header(self.entries,LAST_LBA,alternate=2),LAST_LBA,LAST_LBA)
    def test_revision_reserved_and_header_size(self):
        for changes in ({'revision':0x20000},{'reserved':1},{'size':91},{'size':tool.BLOCK+1}):
            with self.subTest(changes=changes),self.assertRaisesRegex(ValueError,'Unsupported'):
                tool.header(make_header(self.entries,**changes),1,LAST_LBA)
    def test_array_shape_usable_bounds_and_uefi_entry_width(self):
        for changes in ({'first':1},{'first':LAST_USABLE+1},{'final':LAST_LBA},{'count':0},
                        {'width':120},{'width':136},{'width':192},{'count':513,'width':128}):
            with self.subTest(changes=changes),self.assertRaisesRegex(ValueError,'array bounds'):
                tool.header(make_header(self.entries,**changes),1,LAST_LBA)
        large=make_entries(width=256)
        result=tool.header(make_header(large,width=256,first=8),1,LAST_LBA)
        self.assertEqual(256,result['width']);self.assertEqual(6,result['blocks'])
    def test_primary_and_backup_array_placement(self):
        for changes in ({'entry_lba':1},{'entry_lba':FIRST_USABLE}):
            with self.subTest(changes=changes),self.assertRaisesRegex(ValueError,'overlaps usable'):
                tool.header(make_header(self.entries,**changes),1,LAST_LBA)
        for changes in ({'entry_lba':LAST_USABLE},{'entry_lba':LAST_LBA-1}):
            with self.subTest(changes=changes),self.assertRaisesRegex(ValueError,'overlaps usable'):
                tool.header(make_header(self.entries,LAST_LBA,**changes),LAST_LBA,LAST_LBA)
    def test_every_shared_primary_backup_field_is_checked(self):
        for key in ('first','last','entries','width','bytes','array_crc','disk_guid'):
            backup=copy.deepcopy(self.backup)
            backup[key]=('bad-guid' if key=='disk_guid' else backup[key]+1)
            with self.subTest(key=key),self.assertRaisesRegex(ValueError,'Primary/backup'):
                tool.validate_gap(self.primary,backup,self.entries,self.entries)
    def test_array_identity_length_and_crc(self):
        other=bytearray(self.entries);other[-1]^=1
        with self.assertRaisesRegex(ValueError,'Primary/backup'):
            tool.validate_gap(self.primary,self.backup,self.entries,bytes(other))
        for entries in (self.entries[:-1],bytes(other)):
            with self.subTest(length=len(entries)),self.assertRaisesRegex(ValueError,'array CRC'):
                tool.validate_gap(self.primary,self.backup,entries,entries)
    def test_active_partition_ranges_are_rejected(self):
        for begin,end in ((5,10),(10,LAST_USABLE+1),(14,10)):
            entries=bytearray(self.entries);struct.pack_into('<QQ',entries,32,begin,end)
            with self.subTest(begin=begin,end=end),self.assertRaisesRegex(ValueError,'active partition range'):self.verify(bytes(entries))
    def test_closed_gap_boundaries_are_protected(self):
        for begin,end in ((tool.FIRST,tool.FIRST),(tool.LAST,tool.LAST),(tool.FIRST-1,tool.FIRST),
                          (tool.LAST,tool.LAST+1),(tool.FIRST-1,tool.LAST+1)):
            entries=bytearray(self.entries);struct.pack_into('<QQ',entries,32,begin,end)
            with self.subTest(begin=begin,end=end),self.assertRaisesRegex(ValueError,'test area overlaps'):self.verify(bytes(entries))
    def test_gap_must_be_wholly_inside_usable_area(self):
        for key,value in (('FIRST',FIRST_USABLE-1),('LAST',LAST_USABLE+1),('FIRST',tool.LAST+1)):
            with self.subTest(key=key),mock.patch.object(tool,key,value),self.assertRaisesRegex(ValueError,'outside usable'):
                tool.validate_gap(self.primary,self.backup,self.entries,self.entries)
    def test_interpartition_overlap_and_duplicate_ranges_rejected(self):
        for begin,end in ((14,24),(10,14)):
            entries=bytearray(self.entries);struct.pack_into('<QQ',entries,128+32,begin,end)
            with self.subTest(begin=begin),self.assertRaisesRegex(ValueError,'overlap each other'):self.verify(bytes(entries))
        entries=bytearray(self.entries);struct.pack_into('<QQ',entries,128+32,15,19)
        self.assertEqual(79,self.verify(bytes(entries)))
    def test_active_count_drift_rejected(self):
        for count in (78,80):
            with self.subTest(count=count),self.assertRaisesRegex(ValueError,'active partition count'):self.verify(make_entries(count))
    def test_unsorted_nonoverlapping_entries_are_valid(self):
        rows=[self.entries[i*128:(i+1)*128] for i in range(96)]
        rows[:79]=reversed(rows[:79]);self.assertEqual(79,self.verify(b''.join(rows)))

class DurableSaveTests(unittest.TestCase):
    def test_fsync_hash_size_and_independent_readback(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'backup';data=b'original block bytes'
            with mock.patch.object(tool.os,'fsync',wraps=os.fsync) as sync:
                result=tool.durable_save(path,data)
            self.assertEqual(data,path.read_bytes());self.assertEqual(len(data),result['bytes'])
            self.assertEqual(tool.sha(data),result['sha256']);sync.assert_called_once()
    def test_existing_backup_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'backup';path.write_bytes(b'baseline')
            with self.assertRaises(FileExistsError):tool.durable_save(path,b'replacement')
            self.assertEqual(b'baseline',path.read_bytes())
    def test_bad_readback_or_fsync_failure_cannot_report_success(self):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(Path,'read_bytes',return_value=b'corrupt'),self.assertRaisesRegex(ValueError,'readback'):
                tool.durable_save(Path(directory)/'bad-readback',b'original')
            with mock.patch.object(tool.os,'fsync',side_effect=OSError('fixture fsync failure')),self.assertRaises(OSError):
                tool.durable_save(Path(directory)/'bad-sync',b'original')

class CaptureMainTests(unittest.TestCase):
    def setUp(self):
        temporary=tempfile.TemporaryDirectory();self.addCleanup(temporary.cleanup);self.root=Path(temporary.name)
        self.entries=make_entries();self.primary=make_header(self.entries);self.backup=make_header(self.entries,LAST_LBA)
        self.mbr=bytes(tool.BLOCK);self.gap=ZERO_GAP;self.gap_reads=0;self.reads=[];self.commands=[]
        self.product=b'piano';self.boot_complete=b'1';self.mapping=b'sde';self.capacity=tool.CAPACITY;self.logical=tool.BLOCK
        self.partial=None;self.changed_second_gap=False
        baseline=self.root/'private/analysis/ufs-gpt-android-test-57';baseline.mkdir(parents=True)
        (baseline/'lun-4-entries.bin').write_bytes(self.entries)
        (baseline/'lun-4-mbr-header.bin').write_bytes(self.mbr+self.primary)
        (self.root/'private/captures').mkdir(parents=True)
        self.out=self.root/'private/captures/ufs-test-area-91'
    def adb_mock(self,argv,timeout):
        self.assertEqual(['adb','-s','fixture-serial','exec-out'],argv[:4]);self.assertEqual(5,len(argv))
        command=argv[4];self.commands.append(command);self.assertIn(timeout,(30,90))
        if command=='getprop ro.product.device':return self.product
        if command=='getprop sys.boot_completed':return self.boot_complete
        if command=="su -c 'ls /sys/class/scsi_device/0:0:0:4/device/block'":return self.mapping
        if command=="su -c 'cat /sys/block/sde/size'":return str(self.capacity//512).encode()
        if command=="su -c 'cat /sys/block/sde/queue/logical_block_size'":return str(self.logical).encode()
        match=re.fullmatch(r"su -c 'dd if=/dev/block/sde bs=4096 skip=(\d+) count=(\d+) 2>/dev/null'",command)
        self.assertIsNotNone(match,'unexpected or non-read-only device command: '+command)
        lba,count=map(int,match.groups());self.reads.append((lba,count));self.assertEqual(90,timeout)
        self.assertLessEqual(lba+count,LAST_LBA+1)
        if (lba,count)==(1,1):data=self.primary
        elif (lba,count)==(LAST_LBA,1):data=self.backup
        elif (lba,count) in ((2,3),(BACKUP_ARRAY,3)):data=self.entries
        elif lba==tool.FIRST and count==tool.LAST-tool.FIRST+1:
            self.gap_reads+=1;data=self.gap
            if self.changed_second_gap and self.gap_reads==2:data=b'x'+data[1:]
        elif (lba,count) in ((0,1),(tool.FIRST-1,1),(tool.LAST+1,1)):data=self.mbr
        else:self.fail('unexpected block read: '+str((lba,count)))
        return data[:-1] if self.partial==(lba,count) else data
    def capture(self):
        with (mock.patch.object(tool,'__file__',str(self.root/'tools/capture_ufs_test_area.py')),
            mock.patch('sys.argv',['capture','--serial','fixture-serial','--capture-id','91']),
            mock.patch.object(tool.subprocess,'check_output',side_effect=self.adb_mock),contextlib.redirect_stdout(io.StringIO())):
            tool.main()
        return json.loads((self.out/'manifest.json').read_text())
    def test_complete_capture_is_read_only_and_checks_durable_directory_names(self):
        opened=[];native_open=os.open
        def record_open(path,flags,*args,**kwargs):
            if flags&os.O_DIRECTORY:opened.append(Path(path))
            return native_open(path,flags,*args,**kwargs)
        with mock.patch.object(tool.os,'open',side_effect=record_open):result=self.capture()
        self.assertEqual('READ_ONLY_BACKUP_NO_WRITE_TEST',result['status']);self.assertFalse(result['device_writes'])
        self.assertTrue(result['all_zero'] and result['original_reads_match']);self.assertEqual(79,result['active_partitions'])
        self.assertEqual([self.out,self.out.parent],opened);self.assertEqual(2,self.gap_reads)
        self.assertEqual((tool.FIRST,3584),self.reads[4]);self.assertEqual((tool.FIRST,3584),self.reads[5])
        for name,record in result['files'].items():
            data=(self.out/name).read_bytes();self.assertEqual(record,dict(bytes=len(data),sha256=tool.sha(data)))
        self.assertEqual(self.gap[:tool.BLOCK],(self.out/'first-block-original.bin').read_bytes())
        self.assertTrue(all(' of=' not in command for command in self.commands))
    def test_wrong_product_boot_state_and_mapping_rejected(self):
        for attr,value in (('product',b'other'),('boot_complete',b'0'),('mapping',b'sde; id'),('mapping',b'sde\nsdf')):
            old=getattr(self,attr);setattr(self,attr,value)
            with self.subTest(attr=attr),self.assertRaises(SystemExit):self.capture()
            setattr(self,attr,old);self.assertFalse(self.out.exists())
    def test_capacity_or_logical_block_geometry_drift_rejected(self):
        for attr,value in (('capacity',tool.CAPACITY+512),('logical',512)):
            old=getattr(self,attr);setattr(self,attr,value)
            with self.subTest(attr=attr),self.assertRaises(SystemExit):self.capture()
            setattr(self,attr,old);self.assertFalse(self.out.exists())
    def test_partial_raw_read_rejected_before_backup(self):
        self.partial=(1,1)
        with self.assertRaisesRegex(ValueError,'Incomplete raw block'):self.capture()
        self.assertFalse(self.out.exists())
    def test_live_gpt_differs_from_test57_baseline_rejected(self):
        baseline=self.root/'private/analysis/ufs-gpt-android-test-57/lun-4-entries.bin';baseline.write_bytes(b'changed baseline')
        with self.assertRaisesRegex(ValueError,'changed from audited'):self.capture()
        self.assertEqual(0,self.gap_reads);self.assertFalse(self.out.exists())
    def test_gap_changes_between_reads_rejected(self):
        self.changed_second_gap=True
        with self.assertRaisesRegex(ValueError,'changed between independent'):self.capture()
        self.assertEqual(2,self.gap_reads);self.assertFalse(self.out.exists())
    def test_nonzero_gap_is_backed_up_but_never_authorizes_writes(self):
        self.gap=b'original nonzero'+self.gap[16:];self.assertEqual(len(ZERO_GAP),len(self.gap))
        result=self.capture();self.assertFalse(result['all_zero']);self.assertFalse(result['device_writes'])
        self.assertEqual('READ_ONLY_BACKUP_NO_WRITE_TEST',result['status'])
        self.assertEqual(self.gap,(self.out/'gap-original.bin').read_bytes())
    def test_existing_capture_directory_preserved(self):
        self.out.mkdir();marker=self.out/'keep';marker.write_bytes(b'baseline')
        with self.assertRaises(FileExistsError):self.capture()
        self.assertEqual(b'baseline',marker.read_bytes())

class CapturedBackupReadbackTests(unittest.TestCase):
    @unittest.skipUnless((ROOT/'private/captures/ufs-test-area-1/manifest.json').is_file(),'private real capture absent')
    def test_actual_external_capture_hashes_and_both_gpts(self):
        folder=ROOT/'private/captures/ufs-test-area-1';manifest=json.loads((folder/'manifest.json').read_text())
        for name,record in manifest['files'].items():
            data=(folder/name).read_bytes();self.assertEqual(record,dict(bytes=len(data),sha256=tool.sha(data)))
        primary=tool.header((folder/'primary-header.bin').read_bytes(),1,LAST_LBA)
        backup=tool.header((folder/'backup-header.bin').read_bytes(),LAST_LBA,LAST_LBA)
        self.assertEqual(79,tool.validate_gap(primary,backup,(folder/'primary-entries.bin').read_bytes(),(folder/'backup-entries.bin').read_bytes()))
        gap=(folder/'gap-original.bin').read_bytes();self.assertEqual(len(ZERO_GAP),len(gap));self.assertFalse(any(gap))
        self.assertEqual(gap[:tool.BLOCK],(folder/'first-block-original.bin').read_bytes())
        self.assertEqual('ad7facb2586fc6e966c004d7d1d16b024f5805ff7cb47c7a85dabd8b48892ca7',tool.sha(gap[:tool.BLOCK]))
        self.assertFalse(manifest['device_writes']);self.assertEqual('READ_ONLY_BACKUP_NO_WRITE_TEST',manifest['status'])

if __name__=='__main__':unittest.main()
