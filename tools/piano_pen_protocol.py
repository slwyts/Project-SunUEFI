#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Read P81 raw THP metadata and external factory ini calibration.

This module never runs vendor code or creates an input device. Raw input starts
at MiCode's frame_data_packet: omit the Linux stream record header and the
257-byte SPI/event prefix. It must be the original SPI payload, not Android's
HAL mmap frame, whose additive checksums may have been rewritten as CRC32.

The isolated tilt function reconstructs calculate_tilt, not the complete pen
algorithm. Coordinate calibration, raw-matrix preprocessing, final report
semantics and hardware validation remain incomplete.
"""

import argparse
import json
import math
import re
import struct
from pathlib import Path


SOURCE = (
    "https://github.com/MiCode/vendor_xiaomi_proprietary_touch-driver/blob/"
    "6957f6b646d1c919e175e6f9000eb50c8635273c/p81/nt36532/nt36xxx.c"
)
ALGORITHM_SHA256 = "26f86f74781e70d03958271b100b31865d3eb80b69f30774ce1b1e25cb24f1c9"
HAL_SHA256 = "0cc145e5bc55b7c1fa6d59a9995e6cf315c625a1f029b800f04d075346a075ca"
KERNEL_TOUCH_BTF_SHA256 = "b7a88957d3d5237770d014ad609075ea5da5a787fb9342181389d4278fe4c308"
STATE_ACTION = (2, 0, 1, 3)  # actual alg ELF rodata0x11240, not raw SPI states
ACTION_NAMES = ("down", "move", "up", "hover")
CONFIG_SECTIONS = {"project_infor", "hw", "super_resolution", "input_device", "stylus"}
# Current NT36532e Linux capture: 8 KiB rbuf minus SPI/event prefix and dummy.
MAX_PAYLOAD_SIZE = 8192 - 257 - 1


def _u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def parse_runtime_hw_header(data):
    """Read the exact9-byte config prefix of kernel hardware_param_t.

    The remaining214-byte object's lockdown/config/version fields are excluded.
    These dimensions are configuration, not a captured pen position.
    """
    if len(data) != 9:
        raise ValueError("expected the exact9-byte hardware config prefix")
    x, y, rx, tx, resolution = struct.unpack("<4HB", data)
    return {
        "status": "HARDWARE_CONFIG_PREFIX_NOT_PEN_INPUT",
        "display_x": x, "display_y": y,
        "rx": rx, "tx": tx, "super_resolution": resolution,
    }


def parse_runtime_hwinfo_prefix(data):
    """Read the pinned ALG41-byte initialized hardware-info config prefix.

    alg_read_config_param_core constructs it at ELF VA0xa9d10, then copies
    it to0xaa0d0 before alg_init_param. Only this prefix is needed; no raw
    matrices, identities, calibration buffers or solved coordinates are read.
    Byte8 is returned as an uninterpreted flag, never a readiness assertion.
    """
    if len(data) != 41:
        raise ValueError("expected the exact41-byte hardware-info config prefix")
    return {
        "status": "ALGORITHM_RUNTIME_CONFIG_NOT_PEN_INPUT",
        "algorithm_sha256": ALGORITHM_SHA256,
        "raw_hw_pointer": hex(struct.unpack_from("<Q", data)[0]),
        "flag_byte8": data[8],
        "hal_rows": data[9], "hal_cols": data[10],
        "nodes": _u16(data, 12), "snodes": _u16(data, 14),
        "nnodes": _u16(data, 16),
        "scaled_x": _u32(data, 20), "scaled_y": _u32(data, 24),
        "max_scaled": _u32(data, 28), "min_scaled": _u32(data, 32),
        "resolution": _u16(data, 36),
        "x_flip": data[38], "y_flip": data[39], "xy_flip": data[40],
    }


def parse_runtime_context_config(data):
    """Read only48 config bytes from context+0x10, not its0x84d10-byte buffer.

    The runtime context pointer must be independently checked against maps
    before collection. This offline parser neither follows pointers nor reads
    a process. The resolution comes from hwinfo+0x24, via alg_init_param.
    """
    if len(data) != 48:
        raise ValueError("expected exact48-byte context+0x10 config slice")
    return {
        "status": "ALGORITHM_CONTEXT_CONFIG_NOT_PEN_INPUT",
        "algorithm_sha256": ALGORITHM_SHA256,
        "hal_cols": _u32(data, 0), "hal_rows": _u32(data, 4),
        "column_extent": _u32(data, 8), "row_extent": _u32(data, 12),
        "scaled_x": _u32(data, 16), "scaled_y": _u32(data, 20),
        "resolution": _u32(data, 28),
        "x_flip": data[32], "y_flip": data[33], "xy_flip": data[34],
        "scale_coefficient0": struct.unpack_from("<f", data, 36)[0],
        "scale_coefficient1": struct.unpack_from("<f", data, 40)[0],
    }


def parse_stylus_point(data):
    """Decode a36-byte solved algorithm-object dump, never a SPI packet.

    The callback gate/state/pressure/distance writes and state-to-action table
    are statically traced for the pinned ROM. Units, button behavior and actual
    device events remain unverified; this function does not generate input.
    """
    if len(data) != 36:
        raise ValueError("expected the exact36-byte stylus_point object")
    x, y, tilt_x, tilt_y, state, previous, gate, pressure, distance = struct.unpack(
        "<9i", data)
    if not 0 <= state < len(STATE_ACTION):
        raise ValueError("unmapped algorithm stylus state")
    return {
        "status": "SOLVED_OBJECT_STATIC_ABI_ONLY_NOT_INPUT",
        "algorithm_sha256": ALGORITHM_SHA256,
        "coordinate_words": [x, y],
        "tilt_words": [tilt_x, tilt_y],
        "state": state,
        "previous_state": previous,
        "report_gate_word": gate,
        "callback_enabled": gate != 0,
        "pressure_word": pressure,
        "distance_word": distance,
        "mapped_action": STATE_ACTION[state],
        "mapped_action_name": ACTION_NAMES[STATE_ACTION[state]],
        "coordinate_units_verified": False,
        "pressure_normalization_verified": False,
        "button_semantics_verified": False,
        "device_tested": False,
    }


def parse_factory_hal_point(data):
    """Read one pinned ROM64-byte internal HAL point dump.

    ALG constructs this layout for both HAL report methods. The two Piano ini
    files select v1, which writes input_event records. The v2 mmap copy is an
    alternate path; actual kernel BTF confirms its receiver is56-byte, so this
    internal object must not be treated as the current kernel receiver layout.
    Nothing is converted to uinput/libinput.
    """
    if len(data) != 64:
        raise ValueError("expected one ROM64-byte HAL point, not MiCode56-byte")
    words = struct.unpack("<16i", data)
    if words[0] != 1:
        raise ValueError("factory HAL point is not stylus input style1")
    action = words[15]
    if not 0 <= action < len(ACTION_NAMES):
        raise ValueError("unmapped factory HAL action")
    return {
        "status": "FACTORY_INTERNAL_POINT_STATIC_ABI_ONLY_NOT_INPUT",
        "hal_sha256": HAL_SHA256,
        "bytes": 64,
        "input_style": words[0],
        "coordinate_words": list(words[2:4]),
        "tilt_words": list(words[6:8]),
        "distance_word": words[8],
        "pressure_word": words[9],
        "action": action,
        "action_name": ACTION_NAMES[action],
        "unassigned_words": {hex(i * 4): words[i] for i in (1, 4, 5, 10, 11, 12, 13, 14)},
        "public_micode_layout_matches": False,
        "current_kernel_receiver_bytes": 56,
        "current_kernel_receiver_layout_matches": False,
        "v1_static_key_values": {
            "BTN_TOUCH": int(action != 2 and words[9] != 0),
            "BTN_TOOL_PEN": int(action != 2 and (words[8] | words[9]) != 0),
        },
        "coordinate_units_verified": False,
        "button_semantics_verified": False,
        "device_tested": False,
    }


def parse_kernel_report_point(data):
    """Decode the actual BTF56-byte receiver layout, without generating events.

    prop[] indices are returned raw: BTF proves layout, not their runtime use.
    The factory v1 reporter bypasses this mmap point receiver.
    """
    if len(data) != 56:
        raise ValueError("expected the exact56-byte kernel point receiver object")
    words = struct.unpack("<14i", data)
    if words[0] != 1:
        raise ValueError("kernel point is not stylus input style1")
    action = words[13]
    if not 0 <= action < len(ACTION_NAMES):
        raise ValueError("unmapped kernel point action")
    return {
        "status": "KERNEL_RECEIVER_BTF_LAYOUT_ONLY_NOT_INPUT",
        "kernel_touch_btf_sha256": KERNEL_TOUCH_BTF_SHA256,
        "bytes": 56,
        "input_style": words[0],
        "coordinate_words": list(words[1:3]),
        "property_words": list(words[3:13]),
        "action": action,
        "action_name": ACTION_NAMES[action],
        "coordinate_units_verified": False,
        "device_tested": False,
    }


def parse_metadata(data):
    """Return source-defined raw fields and matrices, without solved X/Y/tilt.

    Only the outer additive checksum is checked. Its advertised extent is
    reported; the separate trailing pen checksum and hand packets are not
    validated or decoded by this function.
    """
    if not 64 <= len(data) <= MAX_PAYLOAD_SIZE:
        raise ValueError("P81 THP payload length outside current capture bounds")
    count = struct.unpack_from("<i", data, 8)[0]
    checksum_end = 20 + count * 4
    if count <= 0 or not 64 <= checksum_end <= len(data):
        raise ValueError("outer checksum extent outside payload")
    expected = _u16(data, 4)
    if (_u16(data, 12) != (~expected & 0xffff)
            or _u32(data, 16) != (~count & 0xffffffff)):
        raise ValueError("outer checksum/length complement differs")
    words = struct.unpack_from("<" + str(count * 2) + "H", data, 20)
    if (-sum(words) & 0xffff) != expected:
        raise ValueError("outer THP checksum differs")
    kind = data[56]
    if kind not in (6, 7, 9, 17, 29):
        raise ValueError("not a source-defined P81 pen frame")
    base = 64
    metadata_size = 36 if kind in (17, 29) else 26
    start = base + metadata_size
    if start > checksum_end:
        raise ValueError("pen metadata exceeds outer checksum extent")
    sizes = tuple(data[base + i] for i in (7, 8, 9, 10))
    n1, n2 = sizes[0] * sizes[1], sizes[2] * sizes[3]
    if not n1 or not n2:
        raise ValueError("zero Tip/Ring matrix geometry")
    if start + 4 * (n1 + n2) > checksum_end:
        raise ValueError("Tip/Ring arrays exceed outer checksum extent")
    arrays = {}
    cursor = start
    for name, length in zip(
        ("tip_group1", "tip_group2", "ring_group1", "ring_group2"),
        (n1, n2, n1, n2),
    ):
        arrays[name] = list(struct.unpack_from("<" + str(length) + "h", data, cursor))
        cursor += length * 2
    result = {
        "status": "RAW_METADATA_ONLY_NOT_PEN_INPUT",
        "source": SOURCE,
        "frame_type": kind,
        "drop_frame_no": _u16(data, base),
        "pen_frame_no": _u16(data, base + 2),
        "raw_pressure": _u16(data, base + 4),
        "raw_button1": data[base + 6],
        "raw_button2": data[base + 14],
        "raw_battery": data[base + 15],
        "raw_hover_status": data[base + 16],
        "geometry": {"group1": list(sizes[:2]), "group2": list(sizes[2:])},
        "raw_arrays": arrays,
        "hand_packet_no": data[base + 11],
        "hand_packet_bytes": _u16(data, base + 12),
        "outer_checksum_verified": True,
        "outer_checksum_range": [20, checksum_end],
        "pen_checksum_verified": False,
        "coordinates_available": False,
        "tilt_available": False,
        "button_semantics_verified": False,
        "device_tested": False,
    }
    if kind in (17, 29):
        result.update(
            raw_pen_scan_rate=_u16(data, base + 18),
            raw_pen_scan_frequency=_u16(data, base + 20),
            raw_pen_noise0=_u16(data, base + 22),
            raw_pen_noise1=_u16(data, base + 24),
        )
    return result


def read_config(path):
    """Read integer/list values from the relevant custom ini sections.

    Files are supplied by the caller. No proprietary configuration is embedded.
    This is the observed Piano ini text format, not an Android HAL execution.
    """
    section = None
    result = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        text = line.split("#", 1)[0].strip()
        if not text:
            continue
        if not line[0].isspace() and "=" not in text:
            section = text
            if section in CONFIG_SECTIONS:
                result.setdefault(section, {})
            continue
        if section not in CONFIG_SECTIONS or "=" not in text:
            continue
        key, value = map(str.strip, text.split("=", 1))
        if re.fullmatch(r"-?\d+", value):
            result[section][key] = int(value)
        elif re.fullmatch(r"\{\s*-?\d+(?:\s*,\s*-?\d+)*\s*\}", value):
            result[section][key] = [int(item.strip()) for item in value[1:-1].split(",")]
    return result


def profile(config, vendor_id):
    """Resolve the profile declared by an external ini, not the active pen ID."""
    values = config["stylus"]
    prefix = "stylus"
    for number in (2, 3):
        if values.get(f"stylus_{number}_vendor_id") == vendor_id:
            prefix = f"stylus_{number}"
    if prefix == "stylus" and values.get("stylus_vendor_id") != vendor_id:
        raise ValueError("unmapped pen vendor id")
    thresholds = values.get(prefix + "_coor_diff", values["stylus_coor_diff"])
    angles = values["stylus_angle"]
    if (len(thresholds) != 6 or len(angles) != 6 or thresholds[0] != 0
            or any(a >= b for a, b in zip(thresholds, thresholds[1:]))):
        raise ValueError("unsupported calibration table")
    return {
        "prefix": prefix,
        "vendor_id": vendor_id,
        "angles": angles,
        "thresholds": thresholds,
        "tilt_calibration_enabled": values["stylus_tilt_calibration_en"],
        "tilt_calibration_threshold": values.get(
            prefix + "_tilt_calibration_thd", values["stylus_tilt_calibration_thd"]),
        "tilt_calibration_rate": values.get(
            prefix + "_tilt_calibration_rate", values["stylus_tilt_calibration_rate"]),
    }


def truncdiv(a, b):
    """Signed integer division truncated toward zero, as ARM64 SDIV."""
    if not b:
        raise ValueError("zero calibration divisor")
    return (abs(a) // abs(b)) * (-1 if (a < 0) != (b < 0) else 1)


def _int32(value):
    if type(value) is not int or not -0x80000000 <= value <= 0x7fffffff:
        raise ValueError("working value exceeds reference int32 domain")
    return value


def factory_tilt_component(dx, dy, resolution, calibration):
    """Reconstruct the isolated calculate_tilt step at ELF offset 0x83278.

    Inputs must be already solved Tip/Ring deltas and their actual runtime
    resolution. Project/report ini factors are not substitutes. This does not
    apply tilt_calibration_enabled/threshold/rate: calibrate_coordinate_tilt is
    a separate, unimplemented step. Results are working components, not final
    tablet-tool events; vendor execution and hardware equivalence are untested.
    """
    if type(resolution) is not int or resolution <= 0:
        raise ValueError("explicit positive runtime resolution required")
    _int32(dx)
    _int32(dy)
    thresholds, angles = calibration["thresholds"], calibration["angles"]
    if (len(thresholds) != 6 or len(angles) != 6 or thresholds[0] != 0
            or any(a >= b for a, b in zip(thresholds, thresholds[1:]))):
        raise ValueError("unsupported calibration table")
    for value in (*thresholds, *angles):
        _int32(value)
    limit = _int32(resolution * thresholds[-1])
    saturated = _int32((thresholds[-1] - 1) * resolution)

    def cap(value, negative_bias=0):
        if value >= limit:
            return saturated
        if value <= -limit:
            return -saturated + negative_bias
        return value

    # CSINC in the reference adds one unit on negative Y saturation only.
    dx, dy = cap(dx), cap(dy, 1)
    squared = _int32(dx * dx + dy * dy)
    radius = int(math.sqrt(squared)) + 1
    ratio = math.sqrt(_int32(abs(dx) + abs(dy))) / math.sqrt(radius)

    def axis(delta):
        value = _int32(int(ratio * abs(delta)))
        if value >= limit:
            value = saturated
        segment = next((i for i in range(5) if value < thresholds[i + 1] * resolution), 4)
        slope = truncdiv(_int32(angles[segment + 1] - angles[segment]),
                         thresholds[segment + 1] - thresholds[segment])
        product = _int32((value - thresholds[segment] * resolution) * slope)
        scaled = _int32(angles[segment] + truncdiv(product, resolution))
        return truncdiv(scaled, 100 if delta > 0 else -100)

    return axis(dx), axis(dy)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    raw = commands.add_parser("metadata", help="parse one original SPI THP payload")
    raw.add_argument("payload", type=Path)
    config = commands.add_parser("config", help="read an external factory ini")
    config.add_argument("ini", type=Path)
    solved = commands.add_parser("stylus-point", help="read an exact36-byte solved object dump")
    solved.add_argument("point", type=Path)
    final = commands.add_parser("hal-point", help="read one ROM64-byte internal HAL point dump")
    final.add_argument("point", type=Path)
    kernel = commands.add_parser("kernel-point", help="read one actual BTF56-byte receiver dump")
    kernel.add_argument("point", type=Path)
    hwinfo = commands.add_parser("runtime-hwinfo", help="read the exact41-byte ALG config prefix")
    hwinfo.add_argument("config", type=Path)
    context = commands.add_parser("runtime-context", help="read exact48 config bytes from context+0x10")
    context.add_argument("config", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "stylus-point":
            result = parse_stylus_point(args.point.read_bytes())
        elif args.command == "hal-point":
            result = parse_factory_hal_point(args.point.read_bytes())
        elif args.command == "kernel-point":
            result = parse_kernel_report_point(args.point.read_bytes())
        elif args.command == "runtime-hwinfo":
            result = parse_runtime_hwinfo_prefix(args.config.read_bytes())
        elif args.command == "runtime-context":
            result = parse_runtime_context_config(args.config.read_bytes())
        elif args.command == "metadata":
            result = parse_metadata(args.payload.read_bytes())
        else:
            values = read_config(args.ini)
            stylus = values["stylus"]
            result = {
                "status": "FACTORY_CALIBRATION_READ_STATIC_ONLY",
                "configuration": values,
                "profiles": [profile(values, stylus[key]) for key in (
                    "stylus_vendor_id", "stylus_2_vendor_id", "stylus_3_vendor_id")],
                "input_devices_registered": False,
                "device_tested": False,
            }
    except (OSError, ValueError, KeyError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
