#!/usr/bin/env python3
"""Review-only Piano DRAM/EFI contract planner; never emits or applies C tables.

Fixed captured DTB facts, current native C/cache descriptors and archived EFI
allocations are separate evidence. Suggestions require board/MMU/ownership
validation. No hardware, refs, kernel, allocator, staging or downloads.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

from compose_piano_dtb import read_fdt

ROOT = Path(__file__).resolve().parents[1]
DTB_SHA = "a4b55dd3b77e69be451aaf2263c76f5496c93325767e49f748ee49570611e8d7"
PAGE = 4096
U64_END = 1 << 64
USABLE_TYPES = {1, 2, 3, 4, 7, 9, 14}
EFI_NAMES = {0:"EfiReservedMemoryType", 1:"EfiLoaderCode", 2:"EfiLoaderData",
             3:"EfiBootServicesCode", 4:"EfiBootServicesData", 5:"EfiRuntimeServicesCode",
             6:"EfiRuntimeServicesData", 7:"EfiConventionalMemory", 9:"EfiACPIReclaimMemory", 11:"EfiMemoryMappedIO"}


def sha(data): return hashlib.sha256(data).hexdigest()
def down(value): return value & -PAGE
def up(value): return (value + PAGE - 1) & -PAGE
def span(base, size):
    if type(base)is not int or type(size)is not int or base < 0 or size < 0 or base + size > U64_END:
        raise ValueError("Invalid/overflowing physical interval")
    return base, base + size
def intersects(a, b): return a[0] < b[1] and b[0] < a[1]
def contains(a, b): return a[0] <= b[0] and b[1] <= a[1]
def region(base, end, **fields): return {"base":base,"end":end,"size":end-base,**fields}
def interval(record): return record["base"],record["end"]


def union(ranges):
    result=[]
    for base,end in sorted(ranges):
        if end<=base: continue
        if result and base<=result[-1][1]: result[-1]=(result[-1][0],max(end,result[-1][1]))
        else: result.append((base,end))
    return result


def no_overlap(records, label):
    ordered=sorted(records,key=lambda r:(r["base"],r["end"]))
    for left,right in zip(ordered,ordered[1:]):
        if left["end"]>right["base"]: raise ValueError(label+" overlapping intervals: "+str((left,right)))


def cells(data, width):
    if not data or len(data)%width: raise ValueError("Malformed DT numeric cells")
    return [int.from_bytes(data[i:i+width],"big")for i in range(0,len(data),width)]


def pairs(data):
    if len(data)%16: raise ValueError("Expected DT 2-address/2-size-cell tuples")
    return [span(*struct.unpack_from(">QQ",data,i))for i in range(0,len(data),16)]


def extract_dt(data):
    parsed=read_fdt(data);tree=parsed["tree"]
    for path in ("/","/reserved-memory"):
        if path in tree and any(int.from_bytes(tree[path].get(key,b""),"big")!=2 for key in("#address-cells","#size-cells")):
            raise ValueError("Piano planner requires explicit 2/2 DT address/size cells")
    memory=[];zeros=[];fixed=[];dynamic=[]
    for path,props in tree.items():
        if props.get("device_type")==b"memory\0" or path=="/memory":
            for index,(base,end) in enumerate(pairs(props.get("reg",b""))):
                row=region(base,end,path=path,tuple=index)
                (memory if end>base else zeros).append(row)
        if not path.startswith("/reserved-memory/") or path.count("/")!=2: continue
        enabled=props.get("status",b"okay\0") in (b"okay\0",b"ok\0")
        compat=props.get("compatible",b"").decode().rstrip("\0").split("\0")
        common={"path":path,"enabled":enabled,"no_map":"no-map"in props,
                "reusable":"reusable"in props,"compatible":compat}
        if "reg"in props:
            for index,(base,end) in enumerate(pairs(props["reg"])):
                row=region(base,end,tuple=index,**common)
                if end==base: zeros.append(row);continue
                row["fixed_cma"]=enabled and common["reusable"] and not common["no_map"] and "shared-dma-pool"in compat
                row["page_base"]=down(base);row["page_end"]=up(end)
                fixed.append(row)
        elif "size"in props:
            size=cells(props["size"],8)
            align=cells(props.get("alignment",(0).to_bytes(8,"big")),8)
            if len(size)!=1 or len(align)!=1:raise ValueError("Unexpected dynamic size/alignment")
            ranges=pairs(props["alloc-ranges"])if "alloc-ranges"in props else[(0,U64_END)]
            dynamic.append({**common,"requested_size":size[0],"alignment":align[0],
                "alloc_ranges":[region(a,b)for a,b in ranges],"phase":"dt_constraint_not_allocation",
                "placement":"unknown","occupied_interval":False})
    no_overlap(memory,"DT memory")
    return {"memory":memory,"fixed":fixed,"dynamic":dynamic,"zero_placeholders":zeros,
            "fdt_memreserve":[region(*span(a,n),path="FDT reserve map")for a,n in parsed["reservations"]]}


def parse_native_c(text):
    pattern=r'\{"([^"\n]+)",\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*([^,]+),\s*([^,]+),\s*([^,]+),\s*([^,]+),\s*([^}]+)\}'
    rows=[]
    for m in re.finditer(pattern,text):
        name,base,size,hob,resource,resource_attr,memory_type,arm_attr=m.groups()
        base,end=span(int(base,16),int(size,16))
        value=memory_type.strip();numeric={"BsData":4}.get(value)
        if numeric is None and re.fullmatch(r"\d+",value):numeric=int(value)
        rows.append(region(base,end,name=name,hob=hob.strip(),resource_type=resource.strip(),
            resource_attribute=resource_attr.strip(),memory_type_token=value,memory_type=numeric,
            arm_attribute=arm_attr.strip(),preserve_exactly=True))
    if not rows:raise ValueError("No current native C descriptors parsed")
    declared_rows=len(re.findall(r'\{"[^"\n]+",',text))
    if len(rows)!=declared_rows:raise ValueError("Unsupported native C descriptor syntax; refusing skipped rows")
    no_overlap(rows,"Native C")
    if any((r["base"]|r["size"])%PAGE for r in rows):raise ValueError("Native C is not page aligned")
    return rows


def parse_efi(text):
    rows=[]
    pattern=r'PIANO_EFI_TRACE MAP index=(\d+) type=(\d+) phys=0x([0-9A-Fa-f]+) virt=0x([0-9A-Fa-f]+) pages=0x([0-9A-Fa-f]+) attr=0x([0-9A-Fa-f]+)'
    for m in re.finditer(pattern,text):
        index,kind,base,virtual,pages,attr=m.groups();base,end=span(int(base,16),int(pages,16)*PAGE)
        at=int(attr,16);ty=int(kind)
        rows.append(region(base,end,index=int(index),type=ty,virtual=int(virtual,16),attributes=at,
            is_memory=bool(at&0xe),usable_wb=ty in USABLE_TYPES and bool(at&8),phase="archived_efi_pre_ebs"))
    if not rows or [r["index"]for r in rows]!=list(range(len(rows))):raise ValueError("Incomplete/duplicate EFI snapshot indexes")
    no_overlap(rows,"EFI snapshot")
    if any(r["base"]%PAGE or not r["size"] for r in rows):raise ValueError("EFI page interval invalid")
    enter=re.search(r'EBS_ENTER.*known=1.*bytes=(\d+) stride=(\d+) version=(\d+)',text)
    if not enter or not 40<=int(enter[2])<=256 or int(enter[2])%8 or int(enter[1])!=len(rows)*int(enter[2])or int(enter[3])!=1:
        raise ValueError("EFI descriptor snapshot metadata mismatch")
    return rows


def semantics(kind, wb=True):
    usable=kind in USABLE_TYPES and wb
    return {"pre_ebs_allocator_candidate":kind==7,"linux_is_memory":wb,
            "post_ebs_system_ram":usable,"efi_rebuild_nomap":wb and not usable}


def runtime_evidence(text):
    result=[]
    pattern=r'(?:OF:\s*reserved mem:\s*)?(0x[0-9a-fA-F]+)\.\.(0x[0-9a-fA-F]+)\s*\((\d+) KiB\)\s*(nomap|map)\s*(reusable|non-reusable)\s*(\S+)'
    for m in re.finditer(pattern,text):
        base,end,size,maptype,reuse,name=m.groups();base=int(base,16);end=int(end,16)+1
        if end-base!=int(size)*1024:raise ValueError("Runtime evidence size mismatch")
        result.append(region(base,end,name=name,no_map=maptype=="nomap",reusable=reuse=="reusable",
            phase="android_runtime_observation",uefi_ownership_proven=False))
    return list({(r["base"],r["end"],r["name"]):r for r in result}.values())


def plan(dt, native, archived, reference_json=None, android=None):
    no_overlap(dt["memory"],"DT memory")
    no_overlap(native,"Native preserved")
    no_overlap(archived,"Archived EFI")
    raw=union(interval(r)for r in dt["memory"])
    envelopes=union((down(a),up(b))for a,b in raw)
    fixed=dt["fixed"]+dt["fdt_memreserve"]
    proposals=[];conflicts=[];fringes=[]
    # Conflicts can live outside DT /memory: firmware carve-outs are often holes.
    for n in native:
        for f in fixed:
            overlap=(max(n["base"],down(f["base"])),min(n["end"],up(f["end"])))
            if overlap[0]>=overlap[1]:continue
            runtime=[e["index"]for e in archived if e["type"]in(5,6)and intersects(interval(e),overlap)]
            if n["memory_type"]==7 or runtime:
                conflicts.append(region(*overlap,kind="native_declared_conventional_or_archived_runtime_vs_DT_reservation",
                    native=[n["name"]],fixed=[f["path"]],archived_runtime_indexes=runtime,
                    native_hob=n.get("hob"),actual_allocator_ownership="unknown",
                    resolution="unknown_preserve_cache_and_review_ownership"))
    for a,b in raw:
        if a%PAGE:fringes.append(region(down(a),up(a),reason="partial DRAM start page"))
        if b%PAGE:fringes.append(region(down(b),up(b),reason="partial DRAM end page"))
    boundaries=set(v for a,b in envelopes for v in(a,b))
    for record in [*native,*archived,*fixed,*fringes,*(android or[])]:
        boundaries.update((down(record["base"]),up(record["end"])))
    points=sorted(boundaries)
    for a,b in zip(points,points[1:]):
        if b<=a or not any(contains(e,(a,b))for e in envelopes):continue
        n=[r for r in native if intersects(interval(r),(a,b))]
        f=[r for r in fixed if intersects((r.get("page_base",down(r["base"])),r.get("page_end",up(r["end"]))),(a,b))]
        e=[r for r in archived if intersects(interval(r),(a,b))]
        fringe=any(intersects(interval(r),(a,b))for r in fringes)
        cma=[r for r in f if r.get("fixed_cma")]
        blocked=[r for r in f if not r.get("fixed_cma")]
        kind=None;category="unknown";arm="unknown";alloc=False;post=False;linux_nomap=None
        if n:
            category="preserve_native";kind=n[0]["memory_type"];arm=n[0]["arm_attribute"]
            # Archived EFI capabilities and actual ARM cache type are not interchangeable.
            coverage=sum(min(b,x["end"])-max(a,x["base"])for x in e)
            post=all(x["usable_wb"]for x in e)if coverage==b-a else None
            linux_nomap=all(x["is_memory"]and not x["usable_wb"]for x in e)if coverage==b-a else None
            if cma:
                conflicts.append(region(a,b,kind="CMA_intersects_native_protected_window",
                    fixed=[x["path"]for x in cma],resolution="unknown_no_native_retype"))
        elif fringe:
            category="protect_partial_page";kind=0;linux_nomap=True
        elif cma and not blocked:
            category="fixed_reusable_CMA";kind=2;arm="WRITE_BACK_XN_candidate"
            post=True;linux_nomap=False
        elif f:
            category="fixed_firmware_or_no_map"if all(r.get("enabled",True)for r in f)else"disabled_DT_reserved_ownership_unknown"
            kind=0;linux_nomap=True
            if cma and blocked:conflicts.append(region(a,b,kind="contradictory_fixed_DT_roles",resolution="unknown"))
        elif e and any(x["type"]!=7 or not x["usable_wb"]for x in e):
            category="archived_preboot_ownership_protected"
            kind=e[0]["type"]if len({x["type"]for x in e})==1 else None
            post=all(x["usable_wb"]for x in e);linux_nomap=all(x["is_memory"]and not x["usable_wb"]for x in e)
        else:
            category="free_DRAM_candidate";kind=7;arm="WRITE_BACK_XN_candidate";alloc=True;post=True;linux_nomap=False
        proposals.append(region(a,b,category=category,efi_type=kind,efi_type_name=EFI_NAMES.get(kind,"unknown"),
            arm_attribute=arm,pre_ebs_new_allocator_candidate=alloc,post_ebs_system_ram=post,
            efi_rebuild_nomap=linux_nomap,hardware_verified=False,mmu_verified=False,
            native_provenance=[r["name"]for r in n],dt_fixed_provenance=[r["path"]for r in f],
            archived_efi_indexes=[r["index"]for r in e],dynamic_alloc_constraints=[r["path"]for r in dt["dynamic"]
                if any(intersects(interval(x),(a,b))for x in r["alloc_ranges"])],
            android_runtime_evidence=[r["name"]for r in(android or[])if intersects(interval(r),(a,b))]))
    no_overlap(proposals,"Review proposals")
    if union(interval(r)for r in proposals)!=envelopes:raise ValueError("Review plan does not cover full DRAM page envelopes")
    if any((r["base"]|r["size"])%PAGE for r in proposals):raise ValueError("Review proposal not page aligned")
    free=union(interval(r)for r in proposals if r["pre_ebs_new_allocator_candidate"])
    for candidate in free:
        if any(intersects(candidate,interval(r))for r in native+fixed):raise ValueError("Free candidate consumes reserved/native region")
        if any(intersects(candidate,interval(r))for r in archived if r["type"]!=7 or not r["usable_wb"]):
            raise ValueError("Free candidate consumes archived active/reserved EFI region")
    cma_audit=[]
    for cma in (r for r in fixed if r.get("fixed_cma")):
        covering=[r for r in proposals if intersects(interval(r),interval(cma))]
        covered_bytes=sum(max(0,min(r["end"],cma["end"])-max(r["base"],cma["base"]))for r in covering)
        cma_audit.append({**cma,"candidate_coverage_bytes":covered_bytes,
            "candidate_post_ebs_system_ram_complete":covered_bytes==cma["size"]and all(r["post_ebs_system_ram"]and not r["efi_rebuild_nomap"]for r in covering),
            "pre_ebs_new_allocator_excluded":all(not r["pre_ebs_new_allocator_candidate"]for r in covering),
            "archived_efi_usable_coverage_bytes":sum(max(0,min(cma["end"],r["end"])-max(cma["base"],r["base"]))for r in archived if r["usable_wb"]),
            "ownership_verified":False,"required_action":"review physical ownership + preboot reservation/HOB + WB SystemRAM descriptor + full PFN/linear coverage"})
    high=[region(max(a,1<<32),b,phase="offline_DT_candidate",hardware_verified=False,
        allocator_verified=False,mmu_verified=False,android_runtime_overlaps=[r["name"]for r in(android or[])if intersects((max(a,1<<32),b),interval(r))])
        for a,b in free if b>1<<32 and b-max(a,1<<32)>=PAGE]
    differences=[]
    if reference_json is not None:
        old={r["name"]:(r["base"],r["size"],r["arm_attribute"])for r in reference_json}
        new={r["name"]:(r["base"],r["size"],r["arm_attribute"])for r in native}
        differences=[{"name":name,"reference_json":old.get(name),"current_C":new.get(name)}for name in sorted(old.keys()|new.keys())if old.get(name)!=new.get(name)]
    return {"schema_version":1,"status":"REVIEW_ONLY_NOT_APPLIED","hardware_verified":False,
        "memory_tuple_count_including_zero":len(dt["memory"])+sum(r.get("path")=="/memory"for r in dt["zero_placeholders"]),
        "raw_memory_tuples":dt["memory"],"zero_placeholders":dt["zero_placeholders"],
        "raw_dram_bytes":sum(b-a for a,b in raw),"page_envelope_bytes":sum(b-a for a,b in envelopes),
        "page_fringe_exceptions":fringes,"fixed_reserved":fixed,"dynamic_constraints":dt["dynamic"],
        "native_preserved":native,"native_json_C_differences":differences,"archived_efi":archived,
        "archived_efi_is_memory_bytes":sum(r["size"]for r in archived if r["is_memory"]),
        "archived_efi_usable_bytes":sum(r["size"]for r in archived if r["usable_wb"]),
        "android_runtime_evidence":android or[],"suggested_descriptors":proposals,"conflicts":conflicts,
        "new_descriptor_candidates":[r for r in proposals if not r["native_provenance"]],
        "fixed_CMA_audit":cma_audit,"free_high_DRAM_candidates":high,
        "invariants":{"complete_page_coverage":True,"nonoverlapping":True,"page_aligned":True,
            "native_cache_overrides":0,"new_free_excludes_fixed_native":True},
        "semantic_examples":{"Reserved_WB":semantics(0),"BootServicesData_WB":semantics(4),"LoaderData_WB":semantics(2)},
        "unknowns":["Native/SoC MMU high DRAM mappings and physical ownership are unverified.",
            "Preboot non-conventional descriptor requires actual HOB/allocator reservation validation.",
            "EFI attributes are capabilities, not actual ARM cache page-table proof.",
            "Dynamic alloc-ranges are future constraints; whole envelopes are not allocations.",
            "Android observations are a different phase and do not establish UEFI ownership.",
            "No generated C table, allocator/MMU operation, or hardware acceptance is emitted."]}


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dtb",type=Path,default=ROOT/"private/captures/2026-10-03-piano/live.dtb")
    ap.add_argument("--native-c",type=Path,default=ROOT/"uefi/platforms/pianoProbePkg/Library/MemoryMapLib/MemoryMapLib.c")
    ap.add_argument("--native-json",type=Path,default=ROOT/"uefi/platforms/pianoProbePkg/native-memory-map.json")
    ap.add_argument("--efi-trace",type=Path,default=ROOT/"private/analysis/ramlog-test-85/console.txt")
    ap.add_argument("--runtime-evidence",type=Path)
    ap.add_argument("--output",type=Path,required=True)
    args=ap.parse_args();paths=[args.dtb,args.native_c,args.native_json,args.efi_trace]+([args.runtime_evidence]if args.runtime_evidence else[])
    try:
        inputs={p:p.read_bytes()for p in paths}
        if sha(inputs[args.dtb])!=DTB_SHA:raise ValueError("Captured live DTB SHA mismatch")
        data=plan(extract_dt(inputs[args.dtb]),parse_native_c(inputs[args.native_c].decode()),
            parse_efi(inputs[args.efi_trace].decode(errors="replace")),json.loads(inputs[args.native_json]),
            runtime_evidence(inputs[args.runtime_evidence].decode(errors="replace"))if args.runtime_evidence else None)
        data["inputs"]={str(p.resolve()):{"sha256":sha(raw),"bytes":len(raw)}for p,raw in inputs.items()}
        for p,raw in inputs.items():
            if p.read_bytes()!=raw:raise ValueError("Input changed during planning: "+str(p))
        if args.output.resolve()in[p.resolve()for p in paths]:raise ValueError("Output cannot overwrite source input")
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_text(json.dumps(data,indent=2)+"\n")
    except (ValueError,OSError,KeyError,struct.error)as exc:ap.exit(2,"DRAM review: "+str(exc)+"\n")
    print(json.dumps({"output":str(args.output),"raw_dram_bytes":data["raw_dram_bytes"],
        "segments":len(data["suggested_descriptors"]),"conflicts":len(data["conflicts"]),
        "high_candidates":len(data["free_high_DRAM_candidates"]),"status":data["status"]},indent=2))


if __name__=="__main__":main()
