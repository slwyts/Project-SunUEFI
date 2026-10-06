#!/usr/bin/env python3
"""Read SunUEFI status and a CRC-checked console snapshot over vendor IN EP0."""
import argparse
import ctypes
import ctypes.util
import json
from pathlib import Path
import struct
import time
import zlib

VID, PID, SERIAL = 0x1209, 0x8750, "SunUEFI-piano"
MAX_LOG_BYTES, PAGE_BYTES, PAGE_HEADER = 262144, 512, 24


class ProtocolError(RuntimeError):
    pass


class UsbError(RuntimeError):
    def __init__(self, code, operation):
        self.code = code
        names = {-1: "IO", -3: "ACCESS", -4: "NO_DEVICE", -6: "BUSY", -7: "TIMEOUT", -9: "PIPE"}
        super().__init__(f"{operation}: libusb {names.get(code, 'ERROR')} ({code})")


class DeviceDescriptor(ctypes.Structure):
    _fields_ = [("length", ctypes.c_uint8), ("kind", ctypes.c_uint8), ("usb", ctypes.c_uint16),
                ("device_class", ctypes.c_uint8), ("subclass", ctypes.c_uint8),
                ("protocol", ctypes.c_uint8), ("mps", ctypes.c_uint8),
                ("vendor", ctypes.c_uint16), ("product", ctypes.c_uint16), ("device", ctypes.c_uint16),
                ("manufacturer", ctypes.c_uint8), ("product_string", ctypes.c_uint8),
                ("serial", ctypes.c_uint8), ("configurations", ctypes.c_uint8)]


class Libusb:
    def __init__(self):
        name = ctypes.util.find_library("usb-1.0")
        if not name:
            raise RuntimeError("libusb-1.0 is unavailable")
        self.lib = ctypes.CDLL(name)
        self.context, self.handle = ctypes.c_void_p(), ctypes.c_void_p()
        p, i = ctypes.c_void_p, ctypes.c_int
        signatures = {
            "libusb_init": ([ctypes.POINTER(p)], i),
            "libusb_exit": ([p], None),
            "libusb_get_device_list": ([p, ctypes.POINTER(ctypes.POINTER(p))], ctypes.c_ssize_t),
            "libusb_free_device_list": ([ctypes.POINTER(p), i], None),
            "libusb_get_device_descriptor": ([p, ctypes.POINTER(DeviceDescriptor)], i),
            "libusb_open": ([p, ctypes.POINTER(p)], i),
            "libusb_close": ([p], None),
            "libusb_get_string_descriptor_ascii": ([p, ctypes.c_uint8, p, i], i),
            "libusb_control_transfer": ([p, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint16,
                                          ctypes.c_uint16, p, ctypes.c_uint16, ctypes.c_uint], i),
        }
        for name, (args, result) in signatures.items():
            function = getattr(self.lib, name)
            function.argtypes, function.restype = args, result
        result = self.lib.libusb_init(ctypes.byref(self.context))
        if result < 0:
            raise UsbError(result, "initialize")

    def open(self):
        self.close()
        devices = ctypes.POINTER(ctypes.c_void_p)()
        count = self.lib.libusb_get_device_list(self.context, ctypes.byref(devices))
        if count < 0:
            raise UsbError(count, "list devices")
        error = None
        try:
            for index in range(count):
                descriptor = DeviceDescriptor()
                if self.lib.libusb_get_device_descriptor(devices[index], ctypes.byref(descriptor)) < 0:
                    continue
                if (descriptor.vendor, descriptor.product) != (VID, PID):
                    continue
                handle = ctypes.c_void_p()
                result = self.lib.libusb_open(devices[index], ctypes.byref(handle))
                if result < 0:
                    error = UsbError(result, "open SunUEFI")
                    continue
                serial = ctypes.create_string_buffer(128)
                result = self.lib.libusb_get_string_descriptor_ascii(handle, descriptor.serial, serial, len(serial))
                if result > 0 and serial.raw[:result].decode("ascii", "replace") == SERIAL:
                    self.handle = handle
                    return True
                if result < 0:
                    error = UsbError(result, "read SunUEFI serial")
                self.lib.libusb_close(handle)
            if error:
                raise error
            return False
        finally:
            self.lib.libusb_free_device_list(devices, 1)

    def control(self, kind, request, value, index, length):
        if kind != 0xC0 or request not in (0x5A, 0x5B) or not 0 < length <= 536:
            raise ProtocolError("reader permits only bounded diagnostic vendor IN transfers")
        data = ctypes.create_string_buffer(length)
        result = self.lib.libusb_control_transfer(self.handle, kind, request, value, index, data, length, 1000)
        if result < 0:
            raise UsbError(result, f"IN {request:02x} op={value} page={index}")
        return data.raw[:result]

    def close(self):
        if self.handle:
            self.lib.libusb_close(self.handle)
            self.handle = ctypes.c_void_p()

    def shutdown(self):
        self.close()
        if self.context:
            self.lib.libusb_exit(self.context)
            self.context = ctypes.c_void_p()


def parse_status(data):
    if len(data) != 48:
        raise ProtocolError(f"status length {len(data)}, expected 48")
    magic, address, configuration, super_speed, phase, *values = struct.unpack("<8s4B9I", data)
    if magic != b"SUNDBG01" or configuration != 1 or not 1 <= address <= 127:
        raise ProtocolError("status magic/address/configuration mismatch")
    fields = dict(zip(("dcfg", "dsts", "gctl", "session_hs", "session_ss", "device_events",
                       "setup_events", "accepted_replies", "flags"), values))
    if (fields["dcfg"] >> 3) & 127 != address:
        raise ProtocolError("software and hardware addresses disagree")
    if fields["session_hs"] & 0x10100000 != 0x10100000 or fields["session_ss"] & 0x01000000 == 0:
        raise ProtocolError("status session-valid/power-present missing")
    return dict(address=address, configuration=configuration, super_speed=bool(super_speed), phase=phase, **fields)


def read_roundtrip(control, deadline=None):
    attempts = 0

    def transfer(request, value, page, length):
        nonlocal attempts
        for retry in range(3):
            if deadline is not None and time.monotonic() >= deadline:
                raise ProtocolError("diagnostic deadline expired")
            attempts += 1
            try:
                return control(0xC0, request, value, page, length)
            except UsbError as error:
                if error.code not in (-1, -7) or retry == 2:
                    raise
        raise AssertionError("unreachable")

    legacy = transfer(0x5A, 0, 0, 12)
    if len(legacy) != 12 or legacy[:8] != b"SUNUEFI1" or legacy[9:] != b"\x01\x01\x40":
        raise ProtocolError("legacy 5A diagnostic mismatch")
    status_raw = transfer(0x5B, 0, 0, 48)
    before = parse_status(status_raw)
    if legacy[8] != before["address"]:
        raise ProtocolError("legacy and extended addresses disagree")
    info_raw = transfer(0x5B, 1, 0, 24)
    if len(info_raw) != 24:
        raise ProtocolError("snapshot metadata truncated")
    magic, generation, total, checksum, page_size, flags = struct.unpack("<8sIIIHH", info_raw)
    if magic != b"SUNLOG01" or not generation or total > MAX_LOG_BYTES or page_size != PAGE_BYTES or flags & ~3:
        raise ProtocolError("snapshot metadata bounds/version mismatch")
    log = bytearray()
    pages = 0
    for offset in range(0, total, page_size):
        payload = min(page_size, total - offset)
        data = transfer(0x5B, 2, pages, PAGE_HEADER + payload)
        if len(data) != PAGE_HEADER + payload:
            raise ProtocolError(f"page {pages} truncated")
        page_magic, token, location, size, header_size, crc = struct.unpack("<8sIIHHI", data[:PAGE_HEADER])
        if (page_magic, token, location, size, header_size) != (b"SUNPAGE1", generation, offset, payload, PAGE_HEADER):
            raise ProtocolError(f"page {pages} generation/offset/header mismatch")
        if zlib.crc32(data[PAGE_HEADER:]) != crc:
            raise ProtocolError(f"page {pages} CRC mismatch")
        log.extend(data[PAGE_HEADER:])
        pages += 1
    if len(log) != total or zlib.crc32(log) != checksum:
        raise ProtocolError("assembled console size/CRC mismatch")
    after_raw = transfer(0x5B, 0, 0, 48)
    after = parse_status(after_raw)
    if after["address"] != before["address"] or after["accepted_replies"] < before["accepted_replies"] + pages + 2:
        raise ProtocolError("diagnostic replies did not advance on the same configured device")
    result = dict(debug_verified=True, log_verified=True, generation=generation, log_bytes=total,
                  log_crc32=f"{checksum:08X}", pages=pages, request_attempts=attempts,
                  tail_truncated=bool(flags & 1), current_session_marker=bool(flags & 2),
                  status_before=before, status_after=after, legacy_hex=legacy.hex(),
                  status_before_hex=status_raw.hex(), status_after_hex=after_raw.hex(), snapshot_info_hex=info_raw.hex())
    return result, bytes(log)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-id", type=int, required=True)
    parser.add_argument("--seconds", type=int, default=170, help="start before RAM boot; retry until this deadline")
    args = parser.parse_args()
    if args.test_id < 1 or args.seconds < 1:
        parser.error("test-id and seconds must be positive")
    out = Path(__file__).resolve().parent.parent / f"private/analysis/usb-diagnostic-host-test-{args.test_id}"
    out.mkdir(exist_ok=False)
    result = dict(test_id=args.test_id, debug_verified=False, log_verified=False, events=[])
    deadline = time.monotonic() + args.seconds
    usb = None
    try:
        usb = Libusb()
        while time.monotonic() < deadline:
            try:
                if not usb.open():
                    time.sleep(0.2)
                    continue
                verified, log = read_roundtrip(usb.control, deadline)
                (out / "ramlog.bin").write_bytes(log)
                (out / "ramlog.txt").write_text(log.decode("utf-8", "replace"))
                result.update(verified)
                print(json.dumps(verified), flush=True)
                break
            except (UsbError, ProtocolError) as error:
                event = dict(elapsed_seconds=round(args.seconds - max(0, deadline-time.monotonic()), 2), error=str(error))
                result["events"].append(event)
                print(json.dumps(event), flush=True)
                if isinstance(error, UsbError) and error.code == -3:
                    result["permission_action"] = "Run this read-only tool with sudo, or use an existing USB access permission."
                    break
                time.sleep(0.5)
            finally:
                usb.close()
    except RuntimeError as error:
        result["events"].append(dict(error=str(error)))
    finally:
        if usb is not None:
            usb.shutdown()
        (out / "manifest.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(dict(output=str(out), debug_verified=result["debug_verified"], log_verified=result["log_verified"])))
    return 0 if result["debug_verified"] and result["log_verified"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
