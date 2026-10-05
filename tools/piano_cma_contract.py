#!/usr/bin/env python3
"""Fixed two-pool CMA contract candidate, for an explicit derived target only.

It validates pinned board/native/test85/real Mu memory-helper inputs, appends
occupied LoaderData+WB-XP descriptors, and records a strict snapshot. It never
executes prepare/build/hardware or changes a production platform itself.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path

from plan_piano_dram import DTB_SHA, extract_dt, parse_native_c, parse_efi, interval, intersects, union, contains, semantics

ROOT=Path(__file__).resolve().parents[1]
PINS={
    "private/captures/2026-10-03-piano/live.dtb":DTB_SHA,
    "platforms/pianoProbePkg/Library/MemoryMapLib/MemoryMapLib.c":"b93f559dc4570b998a519fa0987d02b4b9390ee678772c641a615deb252a8634",
    "platforms/pianoProbePkg/native-memory-map.json":"b7f5e824e309e639e7cc9227cdca10396bb9e91054ebabd2f07172c0fe79ad9c",
    "private/analysis/ramlog-test-85/console.txt":"6c6c5de09aafb185c6fbd4c491b6e745a5559c263a64cd63e34e60fd20fcf83c",
    "upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Include/Library/MemoryMapLib.h":"c60c4b80e81388d40148cc014a25f22a20a89a1722c6dd0f1e784efa3268816b",
    "upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Library/MemoryInitPeiLib/MemoryInitPei.c":"dced274a5e31484b647e643fa255439a9a6f6eb5fc210671233614ed80ef2edb",
    "upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Core/Dxe/Gcd/Gcd.c":"3d82f7ff075dc491c8934979e8153dfb112fcab39148a4559a54c301c7dd034a",
    "upstream/Mu-Silicium/Mu_Basecore/MdeModulePkg/Core/Dxe/Mem/Page.c":"fa8880138742041263701de921c0c266662e24b53af4e6da8488ac0960a19e87",
    "upstream/Mu-Silicium/Mu_Basecore/EmbeddedPkg/Library/PrePiHobLib/Hob.c":"fa94f385a5f908407822ed7fddd536cb7e4feb06b6fe5fa86c3dd1fca6556f73",
    "upstream/Mu-Silicium/Mu_Basecore/UefiCpuPkg/Library/ArmMmuLib/AArch64/ArmMmuLibCore.c":"bf3be06245f00c6d1b24b94eb0068f607387780966297070836799fea2aa3521",
    "upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include/Library/ArmLib.h":"45b870a9f883a953281bde367b8bc40aa1c79783d786d437a1a45bfdcbb8d97c",
}
POOLS=(
    {"path":"/reserved-memory/qdss_apps_region@82800000","name":"Piano_CMA_QDSS","base":0x82800000,"size":0x02000000},
    {"path":"/reserved-memory/trust_ui_vm_region@f3800000","name":"Piano_CMA_TrustUI","base":0xf3800000,"size":0x05000000},
)


def digest(data):return hashlib.sha256(data).hexdigest()
def snapshot(root=ROOT,pins=PINS):
    inputs={}
    for relative,expected in pins.items():
        path=root/relative
        if path.is_symlink():raise ValueError("Candidate pinned input cannot be a symlink: "+relative)
        data=path.read_bytes()
        if digest(data)!=expected:raise ValueError("CMA candidate input SHA drift: "+relative)
        inputs[relative]=data
    return inputs


def validate_pools(dt,native,archived):
    memory=union(interval(r)for r in dt["memory"])
    result=[]
    for wanted in POOLS:
        matches=[r for r in dt["fixed"]if r["path"]==wanted["path"]]
        if len(matches)!=1:raise ValueError("Expected exactly one fixed CMA tuple: "+wanted["path"])
        row=matches[0];bounds=(wanted["base"],wanted["base"]+wanted["size"])
        if (interval(row)!=bounds or not row["enabled"]or not row["fixed_cma"]or row["no_map"]or
            not row["reusable"]or "shared-dma-pool"not in row["compatible"]):
            raise ValueError("Pinned fixed CMA attributes/range mismatch: "+wanted["path"])
        if (wanted["base"]|wanted["size"])&4095 or not any(contains(m,bounds)for m in memory):
            raise ValueError("Candidate CMA must be page aligned and entirely in captured DRAM")
        if any(intersects(bounds,interval(n))for n in native):raise ValueError("Candidate overlaps original native/cache row")
        if any(intersects(bounds,interval(e))for e in archived):raise ValueError("Candidate overlaps archived live EFI allocation")
        if any(intersects(bounds,interval(f))for f in dt["fixed"]if f is not row):raise ValueError("Candidate overlaps another fixed reservation")
        result.append({**wanted,"end":bounds[1],"hob":"AddMem","resource_type":"SYS_MEM",
            "resource_attributes":"SYS_MEM_CAP","memory_type":"EfiLoaderData","efi_type_value":2,
            "arm_attribute":"ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP",**semantics(2),
            "hardware_verified":False,"ownership_verified":False,"mmu_verified":False})
    if intersects((result[0]["base"],result[0]["end"]),(result[1]["base"],result[1]["end"])):
        raise ValueError("Candidate pools overlap")
    return result


def render_candidate(original,pools):
    text=original.decode();native=parse_native_c(text)
    if len(native)+len(pools)>=128:raise ValueError("Candidate exceeds real Mu memory table bound")
    marker="\n};\nVOID GetMemoryMap"
    if text.count(marker)!=1:raise ValueError("Unexpected native array boundary")
    prefix=text.split(marker)[0]
    entries=(""if prefix.rstrip().endswith(",")else",")+"\n  // Opt-in test85 CMA contract candidate: occupied before EBS, DT-reserved System RAM after EBS.\n"
    entries+="\n".join('  {"%s", 0x%X, 0x%X, AddMem, SYS_MEM, SYS_MEM_CAP, EfiLoaderData, ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP},'%
        (p["name"],p["base"],p["size"])for p in pools)
    source=text.replace(marker,entries+marker)
    parsed=parse_native_c(source)
    if parsed[:len(native)]!=native or len(parsed)!=len(native)+2:raise ValueError("Candidate altered original descriptor rows")
    if any(r["arm_attribute"]!="ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP"or r["memory_type_token"]!="EfiLoaderData"for r in parsed[len(native):]):
        raise ValueError("Candidate descriptor encoding drift")
    return source.encode()


def create(root=ROOT):
    inputs=snapshot(root)
    dt=extract_dt(inputs["private/captures/2026-10-03-piano/live.dtb"])
    path="platforms/pianoProbePkg/Library/MemoryMapLib/MemoryMapLib.c"
    original=inputs[path];native=parse_native_c(original.decode())
    archived=parse_efi(inputs["private/analysis/ramlog-test-85/console.txt"].decode(errors="replace"))
    pools=validate_pools(dt,native,archived)
    generated=render_candidate(original,pools)
    meta={"schema_version":1,"status":"HOST_CMA_CANDIDATE_NOT_HARDWARE_VERIFIED","hardware_verified":False,
        "ownership_verified":False,"mmu_verified":False,"default_enabled":False,"scope":"exactly_two_fixed_reusable_CMA_pools",
        "source_inputs":{rel:{"sha256":digest(raw),"bytes":len(raw)}for rel,raw in inputs.items()},
        "original_native_rows":len(native),"candidate_rows":pools,"candidate_total_rows":len(native)+2,
        "original_native_sha256":digest(original),"candidate_table_sha256":digest(generated),
        "native_cache_overrides":0,"preboot_conventional_bytes_added":0,
        "mu_semantics":{"AddMem":"resource HOB","SYS_MEM":"also allocation HOB retaining EfiLoaderData type",
            "DXE":"GCD AllocateAddress consumes allocation HOB then EFI descriptor type2; page allocator selects only type7",
            "WRITE_BACK_XN_macro":"aliases plain WB in this pinned tree; new rows use actual WRITE_BACK_XP enum"},
        "limitations":["Physical ownership, actual page-table permissions and successful DXE HOB consumption await hardware.",
            "This does not add full DRAM or a 1GiB download capability, delete CMA or change the kernel.",
            "Original descriptor/cache rows are preserved byte-for-byte; no production platform is edited."]}
    if snapshot(root)!=inputs:raise ValueError("Candidate source snapshot changed during generation")
    return generated,meta


def publish(directory,source,metadata):
    directory.mkdir(parents=True,exist_ok=True)
    (directory/"MemoryMapLib.c").write_bytes(source)
    (directory/"cma-contract-candidate.json").write_text(json.dumps(metadata,indent=2)+"\n")


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output-dir",type=Path,required=True);args=ap.parse_args()
    try:
        source,meta=create()
        if args.output_dir.resolve().is_relative_to((ROOT/"platforms").resolve())or args.output_dir.resolve().is_relative_to((ROOT/"upstream").resolve()):
            raise ValueError("Offline generator cannot publish into platform/staging source")
        publish(args.output_dir,source,meta)
    except(ValueError,OSError,KeyError)as exc:ap.exit(2,"CMA candidate: "+str(exc)+"\n")
    print(json.dumps({"output_dir":str(args.output_dir),"sha256":meta["candidate_table_sha256"],"status":meta["status"]},indent=2))


if __name__=="__main__":main()
