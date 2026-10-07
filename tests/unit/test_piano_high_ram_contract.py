"""Fixed 88 evidence, default-off prototype, actual Mu HOB and MMU helpers."""
import copy
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/"tools"))
import piano_high_ram_contract as tool
BASE=ROOT/"upstream/Mu-Silicium/Mu_Basecore"


class HighRamTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source,cls.meta=tool.create();cls.inputs=tool.cma.snapshot(ROOT,tool.PINS)
        cls.dt=tool.extract_dt(cls.inputs["private/captures/2026-10-03-piano/live.dtb"])
        cls.native=tool.parse_native_c(cls.inputs[tool.BASE_TABLE].decode())
        cls.efi=tool.parse_efi(cls.inputs[tool.TEST88].decode(errors="replace"))
        cls.android=tool.runtime_evidence(cls.inputs[tool.ANDROID86].decode(errors="replace"))

    def test_exact_high_occupied_preserves_prior_44_rows_unknown_phase_gate(self):
        rows=tool.parse_native_c(self.source.decode());self.assertEqual(rows[:44],self.native)
        self.assertEqual(len(rows),45);self.assertEqual(rows[-1]["base"],0xA00000000)
        self.assertEqual(rows[-1]["size"],1<<30);self.assertEqual(rows[-1]["memory_type_token"],"EfiLoaderData")
        row=self.meta["candidate_row"]
        for field in("ownership_verified","mmu_verified","safe_to_apply","actual_at_verified","preboot_allocator_free"):
            self.assertFalse(row[field])
        self.assertTrue(row["post_ebs_system_ram"]);self.assertFalse(row["efi_rebuild_nomap"])
        self.assertEqual({r["path"]for r in row["dynamic_possible_placement_constraints"]},
            {"/reserved-memory/debug_kinfo_region","/reserved-memory/dump_mem_region"})
        self.assertFalse(self.meta["ready_for_hardware"]);self.assertFalse(self.meta["prepare_support_implemented"])
        self.assertEqual(self.meta["preboot_conventional_bytes_added"],0)

    def test_88_real_type2_and_complete_cma_nonmovable_oom_evidence(self):
        evidence=self.meta["test88"]
        self.assertEqual(evidence["descriptors"],52)
        self.assertEqual(evidence["efi_is_memory_bytes"],917696*1024)
        self.assertEqual(evidence["free_pages"],105472);self.assertEqual(evidence["free_cma_pages"],105472)
        self.assertEqual(evidence["non_cma_managed_kib"],14596)
        self.assertTrue(evidence["cma_activation_completed_inference"])
        self.assertFalse(evidence["actual_xp_pte_readback_verified"])
        text=self.inputs[tool.TEST88].decode(errors="replace")
        for before,after in (("type=2 phys=0x82800000","type=0 phys=0x82800000"),
                             ("free:105472 free_pcp:18 free_cma:105472","free:105472 free_pcp:18 free_cma:105471"),
                             ("Memory: 9396K/917696K available","Memory: 9396K/917692K available")):
            with self.subTest(change=before),self.assertRaises(ValueError):tool.inspect_test88(text.replace(before,after))

    def test_range_and_all_fixed_observed_owner_collisions_rejected(self):
        for bounds in ((True,tool.END),(tool.BASE+4096,tool.END),(tool.BASE,tool.END+4096),(tool.BASE,1<<65)):
            with self.subTest(bounds=bounds),self.assertRaises(ValueError):tool.validate_window(self.dt,self.native,{"88":self.efi},self.android,bounds)
        overlap={"base":tool.BASE,"end":tool.BASE+4096}
        for kind in("fixed","native","efi","android","fdt"):
            dt=copy.deepcopy(self.dt);native=copy.deepcopy(self.native);efi=copy.deepcopy(self.efi);android=copy.deepcopy(self.android)
            if kind=="fixed":dt["fixed"].append(overlap)
            elif kind=="fdt":dt["fdt_memreserve"].append(overlap)
            elif kind=="native":native.append(overlap)
            elif kind=="efi":efi.append(overlap)
            else:android.append(overlap)
            with self.subTest(owner=kind),self.assertRaises(ValueError):tool.validate_window(dt,native,{"88":efi},android)
        dt=copy.deepcopy(self.dt);dt["memory"]=[r for r in dt["memory"]if r["tuple"]!=9]
        with self.assertRaises(ValueError):tool.validate_window(dt,self.native,{"88":self.efi},self.android)

    def test_source_and_snapshot_drift_rejected(self):
        with self.assertRaisesRegex(ValueError,"base table drift"):tool.render_prototype(self.inputs[tool.BASE_TABLE]+b"\n",self.meta["candidate_row"])
        # Real source snapshot still runs through the CMA generator; final change
        # must be rejected rather than publishing stale evidence.
        real=tool.cma.snapshot;calls=[]
        def snapshot(root=ROOT,pins=tool.cma.PINS):
            data=real(root,pins)
            if pins is tool.PINS:
                calls.append(1)
                if len(calls)>1:data[tool.TEST88]+=b"changed"
            return data
        with patch.object(tool.cma,"snapshot",side_effect=snapshot),self.assertRaisesRegex(ValueError,"input drift"):
            tool.create()

    def test_cli_default_no_table_and_no_prepare_target_changes(self):
        protected=ROOT/"uefi/platforms/pianoLinuxPkg/Library/MemoryMapLib/MemoryMapLib.c"
        before=protected.read_bytes()
        with tempfile.TemporaryDirectory(prefix="high-review-only-")as directory:
            subprocess.run([sys.executable,str(ROOT/"tools/piano_high_ram_contract.py"),"--output-dir",directory],check=True,capture_output=True)
            self.assertFalse((Path(directory)/"MemoryMapLib.c").exists())
            self.assertTrue((Path(directory)/"high-occupied-prototype.json").exists())
        self.assertEqual(protected.read_bytes(),before)

    def test_real_mu_hob_64bit_occupied_contract(self):
        artifact=ROOT/"artifacts/dram/high-occupied-prototype/MemoryMapLib.c"
        self.assertEqual(artifact.read_bytes(),self.source)
        includes=[BASE/"MdePkg/Include",BASE/"MdePkg/Include/X64",BASE/"MdeModulePkg/Include",BASE/"UefiCpuPkg/Include",BASE/"EmbeddedPkg/Include",ROOT/"upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include"]
        with tempfile.TemporaryDirectory(prefix="high-real-hob-")as directory:
            exe=Path(directory)/"high-hob"
            command=["cc","-std=gnu11","-fshort-wchar","-g","-fsanitize=address,undefined","-fno-pie","-no-pie","-ffunction-sections","-fdata-sections","-include",str(ROOT/"tests/native/PianoCmaPcdShim.h")]
            for include in includes:command += ["-I",str(include)]
            command += [str(ROOT/"tests/native/PianoHighRamHobTest.c"),str(BASE/"EmbeddedPkg/Library/PrePiHobLib/Hob.c"),"-Wl,--gc-sections","-o",str(exe)]
            build=subprocess.run(command,capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stdout+build.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,env={**os.environ,"ASAN_OPTIONS":"detect_leaks=1"})
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn("High offline prototype actual Mu HOB",result.stdout);print(result.stdout.strip())

    def test_actual_pinned_arm_attribute_and_address_functions(self):
        path=BASE/"UefiCpuPkg/Library/ArmMmuLib/AArch64/ArmMmuLibCore.c"
        raw=path.read_bytes();self.assertEqual(tool.cma.digest(raw),tool.cma.PINS[str(path.relative_to(ROOT))])
        text=raw.decode();bodies=[]
        for name in("TranslationRegimeIsDual","ArmMemoryAttributeToPageAttribute","SetOutputAddress","GetOutputAddress"):
            m=re.search(r'STATIC\s+(?:BOOLEAN|UINT64|VOID)\s+'+name+r'\s*\(',text)
            self.assertIsNotNone(m);start=text.index("{",m.start());balance=1;end=start+1
            while balance:
                if text[end]=="{":balance+=1
                if text[end]=="}":balance-=1
                end+=1
            bodies.append(text[m.start():end])
        include=BASE/"MdePkg/Include"
        with tempfile.TemporaryDirectory(prefix="high-actual-arm-attrs-")as directory:
            directory=Path(directory);(directory/"PianoActualArmMmuFunctions.h").write_text("\n\n".join(bodies))
            exe=directory/"arm-attrs"
            # The verbatim upstream switch intentionally falls through after
            # ASSERT(0). Do not edit extracted real source to silence GCC.
            build=subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-Wno-implicit-fallthrough","-g","-fshort-wchar","-fsanitize=address,undefined","-I"+str(include),"-I"+str(include/"X64"),"-I"+str(directory),str(ROOT/"tests/native/PianoArmMmuAttributesTest.c"),"-o",str(exe)],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stdout+build.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn("Actual pinned ArmMmuLibCore helpers",result.stdout);print(result.stdout.strip())


if __name__=="__main__":unittest.main()
