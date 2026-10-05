#!/usr/bin/env python3
"""Pinned, read-only OEM I2C binary and same-device Pogo DT audit.

Output is review evidence, never transport configuration or hardware approval.
No native protocol execution, MMIO, prepare, DMA mapping or device access.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import struct
import uuid
from pathlib import Path

from compose_piano_dtb import read_fdt

ROOT = Path(__file__).resolve().parents[1]
NATIVE_SHA = "7edc3c4cc51825530f53e0bd616b046abad66c740b34eafad38e14a25fcf57b0"
LIVE_SHA = "a4b55dd3b77e69be451aaf2263c76f5496c93325767e49f748ee49570611e8d7"
RESOURCE_DT_SHA = "82b2404bc872c1f60eb4c54c62eb9f2094b97586bdc2aeccae9765f5f258aff1"
GUID_RVA = 0xc058
INTERFACE_RVA = 0xc0e8
GUID = "b27ae8b1-3e10-4d07-ab5c-eb9a6dc6fa8f"
METHOD_RVAS = [0x2720, 0x27d8, 0x2c60, 0x28d4, 0x2764]
BUS = "/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a98000"
CHILD = BUS + "/nanosic@4c"
WRAPPER = BUS.rsplit("/", 1)[0]


def sha(data): return hashlib.sha256(data).hexdigest()


def pe_sections(data):
    """Bound every mapped RVA by raw-backed bytes; never assume RVA=file offset."""
    if len(data) < 0x40 or data[:2] != b"MZ": raise ValueError("Missing DOS header")
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    if pe < 0x40 or pe + 24 > len(data) or data[pe:pe+4] != b"PE\0\0":
        raise ValueError("Invalid/truncated PE header")
    machine, count = struct.unpack_from("<HH", data, pe+4)
    optional_size = struct.unpack_from("<H", data, pe+20)[0]
    optional = pe+24
    if machine != 0xaa64 or not 1 <= count <= 8 or optional_size < 112 or optional+optional_size > len(data):
        raise ValueError("Unexpected ARM64 PE layout")
    if struct.unpack_from("<H", data, optional)[0] != 0x20b:
        raise ValueError("PE32+ required")
    image_base = struct.unpack_from("<Q", data, optional+24)[0]
    size_image = struct.unpack_from("<I", data, optional+56)[0]
    subsystem = struct.unpack_from("<H", data, optional+68)[0]
    section_alignment, file_alignment = struct.unpack_from("<II", data, optional+32)
    directory_count = struct.unpack_from("<I", data, optional+108)[0]
    if directory_count > 16 or optional_size < 112+directory_count*8 or subsystem != 11:
        raise ValueError("Invalid EFI boot driver optional header")
    if image_base != 0 or size_image != 0xf000 or section_alignment != 4096 or file_alignment != 4096:
        raise ValueError("Pinned image sizing/alignment drift")
    start = optional+optional_size
    if start+count*40 > len(data): raise ValueError("Truncated section table")
    sections = []
    for offset in range(start, start+count*40, 40):
        name = data[offset:offset+8].rstrip(b"\0").decode("ascii")
        virtual_size, rva, raw_size, raw = struct.unpack_from("<IIII", data, offset+8)
        flags = struct.unpack_from("<I", data, offset+36)[0]
        if rva % 4096 or raw % 4096 or rva+max(virtual_size,raw_size) > size_image or raw+raw_size > len(data):
            raise ValueError("PE section bounds/alignment")
        sections.append({"name":name,"rva":rva,"virtual_size":virtual_size,
                         "raw_size":raw_size,"raw_offset":raw,"flags":flags})
    for field in ("rva", "raw_offset"):
        ordered = sorted(sections,key=lambda s:s[field])
        for a,b in zip(ordered,ordered[1:]):
            a_size = max(a["virtual_size"],a["raw_size"]) if field=="rva" else a["raw_size"]
            if a[field]+a_size>b[field]: raise ValueError("Overlapping PE sections")
    return sections


def rva_bytes(data, sections, rva, size, executable=False):
    if type(rva) is not int or type(size) is not int or rva < 0 or size <= 0:
        raise ValueError("Invalid RVA interval")
    matched = [s for s in sections if s["rva"] <= rva and rva+size <= s["rva"]+min(s["raw_size"],s["virtual_size"])]
    if len(matched) != 1 or (executable and not matched[0]["flags"] & 0x20000000):
        raise ValueError("RVA outside one raw-backed executable/data section")
    s=matched[0]; offset=s["raw_offset"]+rva-s["rva"]
    return data[offset:offset+size]


def inspect_image(data):
    sections=pe_sections(data)
    guid=str(uuid.UUID(bytes_le=bytes(rva_bytes(data,sections,GUID_RVA,16))))
    table=list(struct.unpack("<6Q",rva_bytes(data,sections,INTERFACE_RVA,48)))
    if guid!=GUID or table != [0x10000,*METHOD_RVAS]: raise ValueError("OEM GUID/vtable drift")
    # Exact AARCH64 installation dataflow. BS slot 0x148 is
    # InstallMultipleProtocolInterfaces, x1 GUID, x2 interface, x3 terminator.
    words={0x1478:0x90000068,0x147c:0xf0000041,0x1480:0xf0000042,
           0x1484:0x91016021,0x1488:0x9103a042,0x148c:0x910023e0,
           0x1490:0xf9411508,0x1494:0xaa1f03e3,0x1498:0xf940a508,0x149c:0xd63f0100}
    for rva,word in words.items():
        if struct.unpack("<I",rva_bytes(data,sections,rva,4,True))[0] != word:
            raise ValueError("Protocol-install instruction drift at "+hex(rva))
    for rva in METHOD_RVAS: rva_bytes(data,sections,rva,4,True)
    return {"guid":guid,"guid_rva":GUID_RVA,"interface_rva":INTERFACE_RVA,
            "revision":table[0],"method_rvas":table[1:],"sections":sections,
            "pointer_layout_confirmed":True,"complete_callable_abi_verified":False,
            "pi_i2c_abi":False,"hardware_protocol_installed_observed":False,
            "method_roles_inferred_from_dataflow":["open","read","write","transfer","close"],
            "inference_boundaries":["No OEM header for this binary version",
                "Native bus enumeration and mutable slave config fields remain unknown",
                "Method return is 32-bit vendor status, not EFI_STATUS"]}


def values(data, count=None):
    if len(data)%4: raise ValueError("Non-cell-aligned DT property")
    result=list(struct.unpack(">"+"I"*(len(data)//4),data))
    if count is not None and len(result)!=count: raise ValueError("Unexpected DT cell count")
    return result


def dt_facts(data):
    d=read_fdt(data);t=d["tree"];h=d["phandles"]
    if BUS not in t or CHILD not in t or WRAPPER not in t: raise ValueError("Missing Piano Pogo nodes")
    b,c,w=t[BUS],t[CHILD],t[WRAPPER]
    if b.get("compatible")!=b"qcom,i2c-geni\0" or c.get("compatible")!=b"nanosic,803\0":
        raise ValueError("Pogo compatible drift")
    if b.get("status") not in (b"ok\0",b"okay\0") or w.get("status") not in (b"ok\0",b"okay\0"):
        raise ValueError("SE/wrapper disabled")
    for parent in ("/soc",WRAPPER):
        if values(t[parent].get("#address-cells",b""),1)!=[1] or values(t[parent].get("#size-cells",b""),1)!=[1]:
            raise ValueError("Expected explicit one/one SE parent cells")
    if values(b["reg"],2)!=[0xa98000,0x4000] or values(w["reg"],2)!=[0xac0000,0x2000] or values(c["reg"],1)!=[0x4c]:
        raise ValueError("SE/wrapper/slave address drift")
    if values(b["interrupts"],3)!=[0,363,4] or values(b["qcom,clk-freq-out"],1)!=[1000000]:
        raise ValueError("SE interrupt/output clock drift")
    clock=values(b["clocks"],2); wrapper_clocks=values(w["clocks"],4)
    if clock!=[0x35,0x64] or wrapper_clocks!=[0x35,0x7d,0x35,0x7e]: raise ValueError("GCC clock specifier drift")
    if clock[0] not in h or values(t[h[clock[0]]].get("#clock-cells",b""),1)!=[1]:
        raise ValueError("Clock provider/cell count mismatch")
    gpio={}
    for key,number,flags in (("irq_pin",97,0x2001),("reset_pin",188,0),("status_pin",95,0),("sleep_pin",3,0)):
        v=values(c[key],3)
        if v!=[0x27,number,flags] or v[0] not in h or values(t[h[v[0]]].get("#gpio-cells",b""),1)!=[2]:
            raise ValueError("GPIO role/provider drift")
        gpio[key]={"number":number,"stock_flags":flags,"provider":h[v[0]],"assert_polarity_verified":False}
    pins=[]
    for handle,number,function in zip(values(b["pinctrl-0"],2),(56,57),("qup1_se6_l0","qup1_se6_l1")):
        if handle not in h: raise ValueError("Unresolved pinctrl")
        path=h[handle];mux=t[path+"/mux"];config=t[path+"/config"]
        if mux.get("pins")!=("gpio"+str(number)+"\0").encode() or mux.get("function")!=(function+"\0").encode():
            raise ValueError("SDA/SCL mux drift")
        if config.get("pins")!=mux["pins"] or values(config.get("drive-strength",b""),1)!=[2] or "bias-disable" not in config:
            raise ValueError("Active Pogo pinctrl electrical DT drift")
        pins.append({"path":path,"gpio":number,"function":function,"drive":2,"bias_disable":True})
    supplies={}
    for key,name,voltage in (("vdd-supply","pm_humu_l14",3300000),("dvdd-supply","pm_humu_l11",1800000)):
        handle=values(c[key],1)[0]
        if handle not in h: raise ValueError("Unresolved supply")
        p=t[h[handle]]
        if p.get("regulator-name")!=(name+"\0").encode() or any(values(p.get(n,b""),1)!=[voltage] for n in ("regulator-min-microvolt","regulator-max-microvolt")):
            raise ValueError("Supply identity/voltage drift")
        supplies[key]={"path":h[handle],"name":name,"dt_constraint_microvolts":voltage,
                       "stock_init_microvolts":values(p["qcom,init-voltage"],1)[0],"actual_native_state_verified":False}
    return {"bus_path":BUS,"device_path":CHILD,"se_base":0xa98000,"se_bytes":0x4000,
            "wrapper_base":0xac0000,"wrapper_bytes":0x2000,"slave":0x4c,
            "output_hz":1000000,"controller_irq":{"spi":363,"flags":4},
            "gcc_clock_provider":h[clock[0]],"se_clock_id":100,"wrapper_clock_ids":[125,126],
            "gpio":gpio,"active_pins":pins,"supplies":supplies,
            "android_bus4_is_native_bus_number":False,
            "native_gpio_clock_supply_contract_verified":False}


def audit(native_path, live_path, resource_path):
    files=[native_path,live_path,resource_path];before=[p.read_bytes() for p in files]
    for data,pin in zip(before,(NATIVE_SHA,LIVE_SHA,RESOURCE_DT_SHA)):
        if sha(data)!=pin: raise ValueError("Fixed source SHA mismatch")
    abi=inspect_image(before[0]);facts=dt_facts(before[1]);resource=read_fdt(before[2])
    configurations=[path for path,props in resource["tree"].items() if any(key in props for key in ("num_se","core_base_addr","qup_id"))]
    if [p.read_bytes() for p in files]!=before: raise ValueError("Input changed during audit")
    return {"schema":1,"kind":"piano-pogo-native-i2c-review","hardware_verified":False,
            "transport_enabled":False,"transport_backend_implemented":False,
            "inputs":[{"path":str(p.resolve()),"bytes":len(b),"sha256":sha(b)} for p,b in zip(files,before)],
            "native_interface":abi,"captured_board":facts,
            "resource_dtb_native_qup_configuration_nodes":configurations,
            "resource_dtb_finds_configuration":bool(configurations),
            "read_policy":{"slave":76,"register":76,"read_bytes":68,"writes":"register-select only",
                "repeated_start":True,"open_close":False,"firmware_or_auth":False,"dma_mapping":False},
            "unknown":["Native TOP_QUP/I2C_HUB enumeration to QUP1_SE6",
                "DTBExtn runtime data source and generated devcfg QUP properties",
                "Full slave-config field semantics and 32-bit status mapping",
                "Existing runtime firmware/session, native power/mux/supplies and IRQ electrical state",
                "Hard deadline for vendor transfer; its observed poll path can wait about 400ms"],
            "required_before_opt_in":["Fixed binary/DT hashes and relocated interface/RVA snapshot",
                "Verified native bus binding, config, power/GPIO contract and already-running MCU",
                "A real bounded synchronous FIFO/PIO backend, not a cast to PI ABI",
                "Independent hardware acceptance and readonly recovery path"]}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native",type=Path,default=ROOT/"upstream/Mu-Silicium/Binaries/piano/Bringup/I2C/I2C.efi")
    parser.add_argument("--dtb",type=Path,default=ROOT/"private/captures/2026-10-03-piano/live.dtb")
    parser.add_argument("--resource-dtb",type=Path,default=ROOT/"upstream/Mu-Silicium/Resources/DTBs/piano.dtb")
    parser.add_argument("--output",type=Path)
    args=parser.parse_args()
    try: result=audit(args.native,args.dtb,args.resource_dtb)
    except (ValueError,KeyError,OSError,struct.error,UnicodeError) as e: parser.error(str(e))
    content=json.dumps(result,indent=2)+"\n"
    if args.output:
        if args.output.resolve() in {p.resolve()for p in (args.native,args.dtb,args.resource_dtb)}:
            parser.error("Output would overwrite input")
        args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(content)
        print(str(args.output.resolve())+" sha256="+sha(content.encode())+" hardware_verified=false")
    else: print(content,end="")


if __name__=="__main__": main()
