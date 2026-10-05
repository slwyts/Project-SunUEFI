#!/usr/bin/env python3
"""Offline high occupied LoaderData prototype; never applies/advertises RAM.

Default emits a review JSON only. Explicit --emit-prototype writes a separate
45-row C prototype preserving the pinned 88 CMA candidate's entire table.
Actual physical ownership, AT/page-table readback and DXE allocator behavior
remain unverified. No prepare/build/MMIO/devices/allocator/DMA operations.
"""
from __future__ import annotations
import argparse
import json
import re
from pathlib import Path

import piano_cma_contract as cma
from plan_piano_dram import extract_dt,parse_native_c,parse_efi,runtime_evidence,interval,intersects,contains,semantics

ROOT=Path(__file__).resolve().parents[1]
BASE_SHA="70aaab84738198b18a78b0f11f715f15fa4c11295f30befe8ab82a98e693b7cc"
TEST88="private/analysis/ramlog-test-88/console.txt"
ANDROID86="private/analysis/dram-runtime-android-after-test-86.txt"
BASE_TABLE="artifacts/dram/cma-contract-candidate/MemoryMapLib.c"
PINS={**cma.PINS,BASE_TABLE:BASE_SHA,
      TEST88:"cfea56f5677aae593c099bf9c0ba90f5c0ee17be6ea73e1eff97bb400962ff16",
      ANDROID86:"9c55977daafd669d1e1b0a9e565bf240e9df9080127a6cff531cd2738d29333b"}
BASE=0xA00000000
SIZE=0x40000000
END=BASE+SIZE
NAME="Piano_HighRam_Occupied_1GiB"


def inspect_test88(text):
    archived=parse_efi(text)
    pools=[]
    for pool in cma.POOLS:
        bounds=(pool["base"],pool["base"]+pool["size"])
        matches=[r for r in archived if interval(r)==bounds]
        if len(matches)!=1 or matches[0]["type"]!=2 or not matches[0]["attributes"]&8:
            raise ValueError("88 lacks exact occupied Type2/WB CMA descriptor")
        pools.append(matches[0])
    if not re.search(r'EBS_RETURN.*status=0x0 success=1',text):raise ValueError("88 EBS success absent")
    if "PIANO_EFI_ENTER_KERNEL" not in text or "PIANO_PRINTK_PANIC_BEGIN" not in text:
        raise ValueError("88 mainline diagnostic provenance absent")
    mainline=text.split("PIANO_EFI_ENTER_KERNEL",1)[1]
    panic=mainline.rsplit("PIANO_PRINTK_PANIC_BEGIN",1)[1]
    if "System is deadlocked on memory" not in panic or "ptp_classifier_init" not in panic or "bpf_int_jit_compile" not in panic:
        raise ValueError("88 expected OOM allocation trace absent")
    if re.search(r'WARNING.*mm/cma.c|CMA area .*could not be activated|Unable to handle kernel paging request',mainline):
        raise ValueError("88 includes unexpected CMA activation/paging failure")
    mem=re.search(r'Memory: (\d+)K/(\d+)K available .*?, (\d+)K reserved, (\d+)K cma-reserved\)',panic)
    free=re.search(r'free:(\d+) free_pcp:(\d+) free_cma:(\d+)',panic)
    managed=re.search(r'Node 0 DMA free:(\d+)kB .*?present:(\d+)kB managed:(\d+)kB .*?free_cma:(\d+)kB',panic)
    gfp=re.search(r'invoked oom-killer: gfp_mask=(0x[0-9a-f]+)\(([^)]+)\), order=(\d+)',panic)
    if not all((mem,free,managed,gfp)):raise ValueError("88 complete memory/OOM accounting absent")
    available,total,reserved,cma_kib=map(int,mem.groups());free_pages,pcp,cma_pages=map(int,free.groups())
    if free_pages!=cma_pages or cma_pages*4!=cma_kib or int(managed[4])!=cma_kib:
        raise ValueError("88 free/CMA accounting differs")
    if sum(r["size"]for r in archived if r["is_memory"])!=total*1024 or int(managed[2])!=total:
        raise ValueError("88 EFI/Linux total RAM mismatch")
    if "__GFP_MOVABLE" in gfp[2] or int(gfp[3])!=0 or "Out of memory and no killable processes" not in panic:
        raise ValueError("88 unmovable/no-victim evidence differs")
    return {"descriptors":len(archived),"cma_descriptors":pools,
        "efi_is_memory_bytes":total*1024,"efi_usable_wb_bytes":sum(r["size"]for r in archived if r["usable_wb"]),
        "linux_available_kib":available,"linux_total_kib":total,"linux_reserved_kib":reserved,
        "linux_cma_kib":cma_kib,"free_pages":free_pages,"free_cma_pages":cma_pages,"pcp_pages":pcp,
        "managed_kib":int(managed[3]),"non_cma_managed_kib":int(managed[3])-cma_kib,
        "gfp_mask":gfp[1],"gfp_named_flags":gfp[2],"order":int(gfp[3]),
        "cma_activation_completed_inference":True,
        "cma_activation_evidence":"Reached later subsys initcall; all 412MiB CMA pages free; no CMA activation/page fault error",
        "cma_activated_bit_directly_observed":False,
        "actual_xp_pte_readback_verified":False,"pid1_userspace_reached":False,
        "panic":"System is deadlocked on memory"},archived


def validate_window(dt,native,efi_snapshots,android,bounds=(BASE,END)):
    begin,end=bounds
    if type(begin)is not int or type(end)is not int or begin<0 or end<=begin or end>1<<64:
        raise ValueError("Invalid high physical interval")
    if (begin,end)!=(BASE,END) or (begin|end)&4095 or begin%(1<<30):
        raise ValueError("Only fixed aligned A00000000..A40000000 prototype is allowed")
    banks=[r for r in dt["memory"]if contains(interval(r),bounds)]
    if len(banks)!=1:raise ValueError("High prototype must lie in one complete captured DRAM tuple")
    exclusions=[]
    for kind,rows in (("fixed_DT",dt["fixed"]),("FDT_memreserve",dt["fdt_memreserve"]),
                      ("native_or_CMA",native),("android_runtime",android)):
        for row in rows:
            if intersects(bounds,interval(row)):exclusions.append((kind,row))
    for phase,rows in efi_snapshots.items():
        for row in rows:
            if intersects(bounds,interval(row)):exclusions.append((phase,row))
    if exclusions:raise ValueError("High prototype intersects excluded interval: "+str(exclusions))
    dynamic=[r for r in dt["dynamic"]if any(intersects(bounds,interval(a))for a in r["alloc_ranges"])]
    return {"base":begin,"end":end,"size":end-begin,"name":NAME,
        "dt_bank":banks[0],"fixed_or_observed_conflicts":[],
        "dynamic_possible_placement_constraints":[{"path":r["path"],"requested_size":r["requested_size"],
            "alloc_ranges":r["alloc_ranges"],"uefi_phase_placement":"unknown",
            "alloc_ranges_are_occupied":False}for r in dynamic],
        "hob":"AddMem","resource_type":"SYS_MEM","resource_attributes":"SYS_MEM_CAP",
        "memory_type":"EfiLoaderData","efi_type_value":2,
        "arm_attribute":"ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP",**semantics(2),
        "preboot_allocator_free":False,"ownership_verified":False,"mmu_verified":False,
        "actual_at_verified":False,"actual_dxe_reservation_verified":False,"safe_to_apply":False,
        "post_ebs_non_cma_system_ram_expected":True,
        "post_ebs_expectation_condition":"Actual owned DRAM and Type2 WB EFI contract; DT reserves must still be honored",
        "android_evidence_phase":"android_runtime_test86_only_not_uefi_ownership"}


def render_prototype(original,row):
    if cma.digest(original)!=BASE_SHA:raise ValueError("88 base table drift")
    text=original.decode();before=parse_native_c(text);marker="\n};\nVOID GetMemoryMap"
    if len(before)!=44 or text.count(marker)!=1:raise ValueError("88 table layout/row count drift")
    entry=('\n  // Offline only: high occupied System RAM prototype; AT/ownership not verified.\n'
           '  {"%s", 0x%X, 0x%X, AddMem, SYS_MEM, SYS_MEM_CAP, EfiLoaderData, ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP},'%
           (row["name"],row["base"],row["size"]))
    result=text.replace(marker,entry+marker).encode();after=parse_native_c(result.decode())
    if len(after)!=45 or after[:44]!=before:raise ValueError("High prototype altered 88 native/cache/CMA rows")
    if after[-1]["memory_type_token"]!="EfiLoaderData" or after[-1]["arm_attribute"]!="ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP":
        raise ValueError("High prototype encoding drift")
    return result


def create(root=ROOT):
    inputs=cma.snapshot(root,PINS)
    recreated,_=cma.create(root)
    if recreated!=inputs[BASE_TABLE]:raise ValueError("88 base does not match strict two-CMA generator")
    dt=extract_dt(inputs["private/captures/2026-10-03-piano/live.dtb"])
    native=parse_native_c(inputs[BASE_TABLE].decode())
    evidence88,efi88=inspect_test88(inputs[TEST88].decode(errors="replace"))
    efi85=parse_efi(inputs["private/analysis/ramlog-test-85/console.txt"].decode(errors="replace"))
    android=runtime_evidence(inputs[ANDROID86].decode(errors="replace"))
    if {(r["name"],r["base"],r["size"])for r in android}!={
        ("debug_kinfo_region",0xbfffff000,4096),("dump_mem_region",0x880000000,94371840)}:
        raise ValueError("Expected exact Android86 phase observations")
    row=validate_window(dt,native,{"archived_efi85":efi85,"archived_efi88":efi88},android)
    source=render_prototype(inputs[BASE_TABLE],row)
    metadata={"schema_version":1,"kind":"piano-high-occupied-offline-prototype",
        "status":"OFFLINE_PROTOTYPE_REQUIRES_AT_OWNERSHIP_AND_DXE_REVIEW",
        "default_enabled":False,"hardware_verified":False,"safe_to_apply":False,"ready_for_hardware":False,
        "prepare_support_implemented":False,"preboot_conventional_bytes_added":0,
        "fastboot_download_advertisement_modified":False,"native_cache_overrides":0,
        "original_88_table_sha256":BASE_SHA,"prototype_table_sha256":cma.digest(source),
        "original_rows":44,"prototype_rows":45,"candidate_row":row,"test88":evidence88,
        "source_inputs":{path:{"sha256":cma.digest(data),"bytes":len(data)}for path,data in inputs.items()},
        "required_before_hardware":["Phase ownership proof including dynamic debug_kinfo/dump_mem placement",
            "Read actual TCR/TTBR/MAIR and page-table entries; bounded AT S1E1R/S1E1W translation audit",
            "Identity PA/cache/permissions readback for boundary/middle pages, existing low heap/native rows unchanged",
            "Confirm real DXE/GCD consumes full Type2 allocation HOB; no preboot Conventional allocation over candidate",
            "Review low-memory page-table allocations and persistent DMA/SMMU owners before any RAM use"],
        "limitations":["Host HOB/attribute tests do not establish owned/usable physical DDR or actual ARM mappings.",
            "Absence of fixed/observed conflict leaves dynamic UEFI placement unknown.",
            "Occupied LoaderData is not a free preboot 1GiB fastboot download pool.",
            "No existing 88 target, map, kernel, pins, prepare or download cap is changed."]}
    if cma.snapshot(root,PINS)!=inputs:raise ValueError("High prototype input drift during generation")
    return source,metadata


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("--output-dir",type=Path,required=True)
    p.add_argument("--emit-prototype",action="store_true",help="Emit separate offline C only; does not prepare/apply")
    args=p.parse_args()
    try:
        source,meta=create();directory=args.output_dir.resolve()
        protected=[ROOT/name for name in("platforms","upstream","build","private","bootprofiles","configs")]
        if any(directory.is_relative_to(path.resolve())for path in protected):raise ValueError("Prototype cannot publish to protected source/staging/evidence")
        if (directory/"MemoryMapLib.c").resolve()==(ROOT/BASE_TABLE).resolve():raise ValueError("Cannot overwrite 88 base artifact")
        directory.mkdir(parents=True,exist_ok=True)
        if (directory/"high-occupied-prototype.json").is_symlink()or(directory/"MemoryMapLib.c").is_symlink():
            raise ValueError("Prototype output cannot be a symlink")
        meta["prototype_c_emitted"]=args.emit_prototype
        if args.emit_prototype:(directory/"MemoryMapLib.c").write_bytes(source)
        (directory/"high-occupied-prototype.json").write_text(json.dumps(meta,indent=2)+"\n")
    except(ValueError,OSError,KeyError)as e:p.exit(2,"High occupied prototype: "+str(e)+"\n")
    print(json.dumps({"output_dir":str(directory),"status":meta["status"],"safe_to_apply":False,
                      "prototype_c_emitted":args.emit_prototype,"table_sha256":meta["prototype_table_sha256"]},indent=2))


if __name__=="__main__":main()
