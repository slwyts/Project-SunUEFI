"""Pinned opt-in two-pool generation and temp-only derived prepare checks."""
import copy
import importlib.util
import json
import pathlib
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=pathlib.Path(__file__).resolve().parents[1]
with patch.object(sys,"path",[str(ROOT/"tools"),*sys.path]):
    import piano_cma_contract as tool
    spec=importlib.util.spec_from_file_location("candidate_linux_prepare",ROOT/"tools/prepare_linux_profile.py")
    prepare=importlib.util.module_from_spec(spec);spec.loader.exec_module(prepare)


class CmaCandidateTests(unittest.TestCase):
    def setUp(self):
        self.inputs=tool.snapshot()
        self.original=self.inputs["platforms/pianoProbePkg/Library/MemoryMapLib/MemoryMapLib.c"]
        self.dt=tool.extract_dt(self.inputs["private/captures/2026-10-03-piano/live.dtb"])
        self.native=tool.parse_native_c(self.original.decode())
        self.efi=tool.parse_efi(self.inputs["private/analysis/ramlog-test-85/console.txt"].decode(errors="replace"))

    def test_real_candidate_has_only_two_occupied_systemram_rows_and_preserves_native(self):
        source,meta=tool.create()
        parsed=tool.parse_native_c(source.decode())
        self.assertEqual(self.native,parsed[:len(self.native)])
        self.assertEqual(len(self.native)+2,len(parsed))
        self.assertEqual(0,meta["native_cache_overrides"])
        self.assertEqual(0,meta["preboot_conventional_bytes_added"])
        for row in meta["candidate_rows"]:
            self.assertEqual(2,row["efi_type_value"])
            self.assertFalse(row["pre_ebs_allocator_candidate"])
            self.assertTrue(row["post_ebs_system_ram"])
            self.assertFalse(row["efi_rebuild_nomap"])
            self.assertFalse(row["hardware_verified"])
        self.assertIn(b"ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP",source)
        self.assertNotIn(b"ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XN",source)

    def test_pool_range_enabled_reusable_nomap_or_extra_tuple_drift_is_rejected(self):
        path=tool.POOLS[0]["path"]
        for field,value in (("enabled",False),("reusable",False),("no_map",True),("fixed_cma",False),("base",0x82801000)):
            dt=copy.deepcopy(self.dt);row=next(r for r in dt["fixed"]if r["path"]==path);row[field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):tool.validate_pools(dt,self.native,self.efi)
        dt=copy.deepcopy(self.dt);dt["fixed"].append(copy.deepcopy(next(r for r in dt["fixed"]if r["path"]==path)))
        with self.assertRaises(ValueError):tool.validate_pools(dt,self.native,self.efi)

    def test_native_efi_or_other_reservation_overlap_is_rejected(self):
        overlap=tool.POOLS[0];record={"base":overlap["base"],"end":overlap["base"]+4096}
        for kind in("native","efi","fixed"):
            dt=copy.deepcopy(self.dt);native=copy.deepcopy(self.native);efi=copy.deepcopy(self.efi)
            if kind=="native":native.append(record)
            elif kind=="efi":efi.append(record)
            else:dt["fixed"].append({**record,"path":"/reserved-memory/conflict"})
            with self.subTest(kind=kind),self.assertRaises(ValueError):tool.validate_pools(dt,native,efi)

    def test_pinned_source_and_mid_generation_snapshot_drift_are_rejected(self):
        with tempfile.TemporaryDirectory()as name:
            root=pathlib.Path(name);pins={"input":"0"*64};(root/"input").write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError,"SHA drift"):tool.snapshot(root,pins)
        changed=dict(self.inputs);changed[next(iter(changed))]+=b"changed"
        with patch.object(tool,"snapshot",side_effect=[self.inputs,changed]),self.assertRaisesRegex(ValueError,"snapshot changed"):tool.create()

    def fixture(self,root):
        src=root/"platforms/pianoProbePkg";src.mkdir(parents=True)
        for suffix in("dsc","dec","fdf"):(src/('pianoProbe.'+suffix)).write_text('pianoProbe\n!include SiliciumPkg/Common.fdf.inc\n')
        lib=src/"Library/Stage0BootManagerLib";lib.mkdir(parents=True)
        (lib/"Stage0BootManagerLib.inf").write_text('  FdtLib\n')
        (lib/"Stage0BootManagerLib.c").write_text('#include <Library/DebugLib.h>\nVOID EFIAPI DeviceBootManagerUnableToBoot (VOID) { }\n')
        table=src/"Library/MemoryMapLib";table.mkdir();(table/"MemoryMapLib.c").write_bytes(self.original)
        for dirname,names in (("linux-ram",("LinuxRamBoot.c","LinuxRamBoot.inf","PianoEfiHandoffTrace.c","PianoEfiHandoffTrace.h")),("uefi-app",("PianoFaultRecovery.c",))):
            dest=root/"bootprofiles"/dirname;dest.mkdir(parents=True)
            for name in names:(dest/name).write_text('fixture\n')
        (root/"tools").mkdir()

    def invoke_prepare(self,root,*args,candidate=None):
        with patch.object(prepare,"__file__",str(root/"tools/prepare_linux_profile.py")), \
             patch.object(sys,"argv",["prepare_linux_profile.py",*args]), \
             patch.object(prepare,"create_cma_candidate",return_value=candidate)as generator:
            prepare.main();return generator.call_count

    def test_prepare_default_retains_native_and_flag_changes_only_derived_table(self):
        source,meta=tool.create()
        with tempfile.TemporaryDirectory()as name:
            root=pathlib.Path(name);self.fixture(root)
            self.assertEqual(0,self.invoke_prepare(root))
            dst=root/"platforms/pianoLinuxPkg";table=dst/"Library/MemoryMapLib/MemoryMapLib.c"
            self.assertEqual(self.original,table.read_bytes());self.assertFalse((dst/"cma-contract-candidate.json").exists())
            self.assertEqual(1,self.invoke_prepare(root,"--cma-contract-candidate",candidate=(source,meta)))
            self.assertEqual(source,table.read_bytes())
            self.assertEqual(self.original,(root/"platforms/pianoProbePkg/Library/MemoryMapLib/MemoryMapLib.c").read_bytes())
            self.assertEqual(meta["candidate_table_sha256"],json.loads((dst/"cma-contract-candidate.json").read_text())["candidate_table_sha256"])
            staged=root/"upstream/Mu-Silicium/Platforms/Xiaomi/pianoLinuxPkg/Library/MemoryMapLib/MemoryMapLib.c"
            self.assertEqual(source,staged.read_bytes())
            self.assertEqual(0,self.invoke_prepare(root))
            self.assertEqual(self.original,table.read_bytes())
            self.assertFalse((dst/"cma-contract-candidate.json").exists())
            self.assertFalse((staged.parents[2]/"cma-contract-candidate.json").exists())

    def test_raw_candidate_combination_rejected_before_derived_target(self):
        with tempfile.TemporaryDirectory()as name:
            root=pathlib.Path(name);self.fixture(root)
            with self.assertRaises(SystemExit):self.invoke_prepare(root,"--raw","--cma-contract-candidate")
            self.assertFalse((root/"platforms/pianoLinuxPkg").exists())


if __name__=="__main__":unittest.main()
