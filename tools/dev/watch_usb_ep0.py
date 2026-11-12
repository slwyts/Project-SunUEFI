#!/usr/bin/env python3
"""Observe SunUEFI EP0 enumeration and read its diagnostic control request."""
import argparse
import ctypes
import ctypes.util
import json
from pathlib import Path
import time

def diagnostic():
    name=ctypes.util.find_library('usb-1.0')
    if not name:return {'error':'libusb unavailable'}
    lib=ctypes.CDLL(name);ctx=ctypes.c_void_p()
    lib.libusb_init.argtypes=[ctypes.POINTER(ctypes.c_void_p)]
    lib.libusb_open_device_with_vid_pid.argtypes=[ctypes.c_void_p,ctypes.c_uint16,ctypes.c_uint16]
    lib.libusb_open_device_with_vid_pid.restype=ctypes.c_void_p
    lib.libusb_control_transfer.argtypes=[ctypes.c_void_p,ctypes.c_uint8,ctypes.c_uint8,ctypes.c_uint16,
        ctypes.c_uint16,ctypes.c_void_p,ctypes.c_uint16,ctypes.c_uint]
    lib.libusb_close.argtypes=[ctypes.c_void_p];lib.libusb_exit.argtypes=[ctypes.c_void_p]
    if lib.libusb_init(ctypes.byref(ctx)):return {'error':'libusb initialization failed'}
    handle=lib.libusb_open_device_with_vid_pid(ctx,0x1209,0x8750)
    if not handle:lib.libusb_exit(ctx);return {'error':'enumerated device could not be opened'}
    results={}
    try:
        for label,kind,request,value,length in [('device_descriptor',0x80,6,0x100,18),('configuration',0x80,8,0,1),('debug_info',0xc0,0x5a,0,64)]:
            data=ctypes.create_string_buffer(length)
            count=lib.libusb_control_transfer(handle,kind,request,value,0,data,length,1000)
            results[label]={'result':count,'hex':data.raw[:max(count,0)].hex()}
        results['debug_verified']=results['debug_info']['result']==12 and bytes.fromhex(results['debug_info']['hex']).startswith(b'SUNUEFI1')
    finally:lib.libusb_close(handle);lib.libusb_exit(ctx)
    return results

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--test-id',type=int,required=True)
    ap.add_argument('--seconds',type=int,default=170);args=ap.parse_args()
    root=Path(__file__).resolve().parent.parent;out=root/f'private/analysis/usb-ep0-host-test-{args.test_id}'
    out.mkdir(exist_ok=False);deadline=time.monotonic()+args.seconds;events=[];last=None;queried=False
    while time.monotonic()<deadline:
        found=[]
        for path in Path('/sys/bus/usb/devices').iterdir():
            try:
                if (path/'idVendor').read_text().strip()!='1209' or (path/'idProduct').read_text().strip()!='8750':continue
                record={'path':path.name,'speed':(path/'speed').read_text().strip(),
                        'configuration':(path/'bConfigurationValue').read_text().strip()}
                if (path/'serial').exists():record['serial']=(path/'serial').read_text().strip()
                found.append(record)
                if not queried and record['configuration']=='1':
                    events.append({'control':diagnostic()});queried=True
                    (out/'descriptors.bin').write_bytes((path/'descriptors').read_bytes())
            except (FileNotFoundError,OSError):continue
        if found!=last:
            event={'elapsed_seconds':round(args.seconds-(deadline-time.monotonic()),2),'devices':found}
            events.append(event);print(json.dumps(event),flush=True);last=found
        time.sleep(0.2)
    result={'test_id':args.test_id,'enumerated':any(e.get('devices') for e in events),
            'debug_verified':any(e.get('control',{}).get('debug_verified') for e in events),'events':events}
    (out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))

if __name__=='__main__':main()
