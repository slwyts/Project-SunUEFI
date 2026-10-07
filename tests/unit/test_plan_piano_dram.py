"""Offline physical intervals and EFI semantics, with real captured integration."""
import copy
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
SPEC=importlib.util.spec_from_file_location("piano_dram",ROOT/"tools/plan_piano_dram.py")
tool=importlib.util.module_from_spec(SPEC)
with patch.object(sys,"path",[str(ROOT/"tools"),*sys.path]):SPEC.loader.exec_module(tool)


def minimal():
    return {"memory":[tool.region(0x1000,0xa000,path="/memory",tuple=0)],
        "fixed":[],"dynamic":[],"zero_placeholders":[],"fdt_memreserve":[]}


class DramPlanTests(unittest.TestCase):
    def test_reserve_wb_and_loader_wb_have_different_linux_contracts(self):
        reserved=tool.semantics(0);loader=tool.semantics(2)
        self.assertTrue(reserved["linux_is_memory"])
        self.assertFalse(reserved["post_ebs_system_ram"])
        self.assertTrue(reserved["efi_rebuild_nomap"])
        self.assertFalse(loader["pre_ebs_allocator_candidate"])
        self.assertTrue(loader["post_ebs_system_ram"])
        self.assertFalse(loader["efi_rebuild_nomap"])

    def test_cma_is_not_preboot_free_and_requires_complete_post_ebs_coverage(self):
        dt=minimal();dt["fixed"]=[tool.region(0x4000,0x7000,path="/reserved-memory/cma",enabled=True,no_map=False,
            reusable=True,fixed_cma=True,page_base=0x4000,page_end=0x7000)]
        result=tool.plan(dt,[],[])
        cma=result["fixed_CMA_audit"][0]
        self.assertTrue(cma["candidate_post_ebs_system_ram_complete"])
        self.assertTrue(cma["pre_ebs_new_allocator_excluded"])
        segments=[r for r in result["suggested_descriptors"]if r["category"]=="fixed_reusable_CMA"]
        self.assertEqual(2,segments[0]["efi_type"])
        dt["fixed"][0].update(base=0xb000,end=0xc000,size=4096,page_base=0xb000,page_end=0xc000)
        cma=tool.plan(dt,[],[])["fixed_CMA_audit"][0]
        self.assertFalse(cma["candidate_post_ebs_system_ram_complete"])
        self.assertEqual(0,cma["candidate_coverage_bytes"])

    def test_no_map_and_native_cache_take_precedence_with_conflict_preserved(self):
        dt=minimal();dt["fixed"]=[tool.region(0x3000,0x5000,path="/reserved-memory/fw",enabled=True,no_map=True,reusable=False)]
        native=[tool.region(0x2000,0x6000,name="heap",memory_type=7,arm_attribute="WRITE_THROUGH_XN",hob="AddMem")]
        original=copy.deepcopy(native)
        result=tool.plan(dt,native,[])
        self.assertEqual(original,result["native_preserved"])
        self.assertTrue(result["conflicts"])
        self.assertTrue(all(r["arm_attribute"]=="WRITE_THROUGH_XN"for r in result["suggested_descriptors"]if r["native_provenance"]))
        self.assertFalse(any(r["pre_ebs_new_allocator_candidate"]for r in result["suggested_descriptors"]if r["native_provenance"]))
        self.assertTrue(all(r["post_ebs_system_ram"]is None for r in result["suggested_descriptors"]if r["native_provenance"]))

    def test_non_native_archived_allocations_are_not_new_free_memory(self):
        for kind in (0,1,2,3,4,5,6):
            archived=[tool.region(0x3000,0x7000,index=0,type=kind,is_memory=True,usable_wb=kind in tool.USABLE_TYPES)]
            result=tool.plan(minimal(),[],archived)
            owned=[r for r in result["suggested_descriptors"]if r["archived_efi_indexes"]]
            with self.subTest(kind=kind):
                self.assertTrue(owned)
                self.assertFalse(any(r["pre_ebs_new_allocator_candidate"]for r in owned))
                self.assertEqual("archived_preboot_ownership_protected",owned[0]["category"])

    def test_partial_page_is_explicitly_protected_never_freed(self):
        dt=minimal();dt["memory"][0].update(base=0x1800,size=0x8800)
        dt["fixed"]=[tool.region(0x1000,0x1800,path="/reserved-memory/half-page",enabled=True,no_map=True,reusable=False)]
        result=tool.plan(dt,[],[])
        self.assertEqual(0x1000,result["suggested_descriptors"][0]["base"])
        self.assertFalse(result["suggested_descriptors"][0]["pre_ebs_new_allocator_candidate"])
        self.assertEqual(1,len(result["page_fringe_exceptions"]))
        self.assertTrue(result["invariants"]["complete_page_coverage"])

    def test_dynamic_envelope_does_not_reserve_the_whole_range(self):
        dt=minimal();dt["dynamic"]=[{"path":"/reserved-memory/dynamic","alloc_ranges":[tool.region(0,1<<64)],
            "requested_size":4096,"phase":"dt_constraint_not_allocation","occupied_interval":False}]
        result=tool.plan(dt,[],[])
        self.assertTrue(all(r["pre_ebs_new_allocator_candidate"]for r in result["suggested_descriptors"]))
        self.assertFalse(result["dynamic_constraints"][0]["occupied_interval"])

    def test_android_phase_annotates_but_does_not_claim_uefi_reservation(self):
        dt=minimal();dt["memory"]=[tool.region(0x100000000,0x180000000,path="/memory",tuple=0)]
        runtime=tool.runtime_evidence("OF: reserved mem: 0x100000000..0x100000fff (4 KiB) nomap non-reusable debug_kinfo_region")
        result=tool.plan(dt,[],[],android=runtime)
        self.assertFalse(result["android_runtime_evidence"][0]["uefi_ownership_proven"])
        self.assertTrue(result["free_high_DRAM_candidates"][0]["android_runtime_overlaps"])
        self.assertTrue(result["suggested_descriptors"][0]["pre_ebs_new_allocator_candidate"])

    def test_overflow_overlap_and_incomplete_trace_are_rejected(self):
        with self.assertRaises(ValueError):tool.span((1<<64)-1,2)
        with self.assertRaises(ValueError):tool.no_overlap([tool.region(1,5),tool.region(4,7)],"fixture")
        bad=minimal();bad["memory"].append(tool.region(0x4000,0xb000))
        with self.assertRaises(ValueError):tool.plan(bad,[],[])
        text="PIANO_EFI_TRACE EBS_ENTER known=1 bytes=48 stride=48 version=1\nPIANO_EFI_TRACE MAP index=0 type=7 phys=0x1000 virt=0x0 pages=0x1 attr=0x8\n"
        self.assertEqual(1,len(tool.parse_efi(text)))
        for bad in(text.replace("index=0","index=1"),text.replace("bytes=48","bytes=96"),text.replace("version=1","version=2"),
                   text.replace("bytes=48 stride=48","bytes=1 stride=1")):
            with self.subTest(bad=bad),self.assertRaises(ValueError):tool.parse_efi(bad)

    def test_native_rows_cannot_be_silently_skipped(self):
        text='{"one", 0x1000, 0x1000, AddMem, 0, 0x0, 7, WRITE_BACK_XN}'
        self.assertEqual(1,len(tool.parse_native_c(text)))
        with self.assertRaisesRegex(ValueError,"skipped rows"):
            tool.parse_native_c(text+'\n{"unsupported", SYMBOL, 0x1000, AddMem, 0, 0x0, 7, WRITE_BACK_XN}')

    def test_current_real_board_coverage_fixed_cma_and_native_differences(self):
        dtb=(ROOT/"private/captures/2026-10-03-piano/live.dtb").read_bytes()
        self.assertEqual(tool.DTB_SHA,tool.sha(dtb))
        dt=tool.extract_dt(dtb)
        native=tool.parse_native_c((ROOT/"uefi/platforms/pianoProbePkg/Library/MemoryMapLib/MemoryMapLib.c").read_text())
        efi=tool.parse_efi((ROOT/"private/analysis/ramlog-test-85/console.txt").read_bytes().decode(errors="replace"))
        reference=json.loads((ROOT/"uefi/platforms/pianoProbePkg/native-memory-map.json").read_text())
        result=tool.plan(dt,native,efi,reference)
        self.assertEqual(16470685696,result["raw_dram_bytes"])
        self.assertEqual(16,len(result["raw_memory_tuples"]))
        self.assertEqual(17,result["memory_tuple_count_including_zero"])
        self.assertEqual(1,sum(r.get("path")=="/memory"for r in result["zero_placeholders"]))
        self.assertEqual(803008*1024,result["archived_efi_is_memory_bytes"])
        self.assertTrue(all(r["candidate_post_ebs_system_ram_complete"]for r in result["fixed_CMA_audit"]))
        self.assertTrue(all(r["pre_ebs_new_allocator_excluded"]for r in result["fixed_CMA_audit"]))
        self.assertTrue(all(r["archived_efi_usable_coverage_bytes"]==0 for r in result["fixed_CMA_audit"]))
        self.assertIn("UEFI_FD",{r["name"]for r in result["native_json_C_differences"]})
        self.assertTrue(any(r["size"]>=1<<30 for r in result["free_high_DRAM_candidates"]))
        self.assertEqual(0,result["invariants"]["native_cache_overrides"])
        self.assertFalse(result["hardware_verified"])

    def test_cli_rejects_dtb_pin_drift_before_publishing(self):
        with tempfile.TemporaryDirectory()as name:
            directory=Path(name);dtb=directory/"changed.dtb";output=directory/"review.json"
            raw=bytearray((ROOT/"private/captures/2026-10-03-piano/live.dtb").read_bytes());raw[-1]^=1
            dtb.write_bytes(raw)
            with patch.object(sys,"argv",["plan_piano_dram.py","--dtb",str(dtb),"--output",str(output)]),self.assertRaises(SystemExit):tool.main()
            self.assertFalse(output.exists())


if __name__=="__main__":unittest.main()
