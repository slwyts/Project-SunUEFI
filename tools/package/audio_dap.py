"""Load the original Piano speaker tuning into the AudioReach topology."""

from hashlib import sha256
import re
import struct


def _words(data, offset, count):
    return struct.unpack_from(f"<{count}I", data, offset)


def _chunks(data):
    result = {}
    offset = 12
    while offset < len(data):
        name, length = struct.unpack_from("<4sI", data, offset)
        offset += 8
        end = offset + length
        if name in result or end > len(data):
            raise ValueError("Invalid ACDB chunk")
        result[name] = data[offset:end]
        offset = end
    return result


def _speaker_params(chunks):
    speaker = 0xb000019f
    tagged = chunks[b"TMLU"]
    count, = _words(tagged, 0, 1)
    source_instances = []
    for index in range(count):
        subgraph, tag, definition = _words(tagged, 4 + 12 * index, 3)
        if subgraph == speaker and tag == 0x0c000033:
            modules, module, instance = _words(chunks[b"TMDE"], definition, 3)
            if modules != 1 or module != 0x100108db:
                raise ValueError("Unexpected Piano speaker DAP module")
            source_instances.append(instance)
    source_instance, = source_instances

    subgraphs = chunks[b"CSLU"]
    count, = _words(subgraphs, 0, 1)
    offset = 4
    defaults = []
    for _ in range(count):
        subgraph, tables = _words(subgraphs, offset, 2)
        offset += 8
        if subgraph == speaker:
            for index in range(tables):
                keys, table = _words(subgraphs, offset + 8 * index, 2)
                if _words(chunks[b"CAKT"], keys, 1) == (0,):
                    defaults.append(table)
        offset += 8 * tables
    default, = defaults
    keys, variants, definition, lookup = _words(chunks[b"CDLU"], default, 4)
    if keys != 0 or variants != 1:
        raise ValueError("Unexpected default speaker calibration table")

    pool = chunks[b"POOL"]
    count, = _words(chunks[b"CDDE"], definition, 1)
    if _words(chunks[b"CDDO"], lookup, 1) != (count,):
        raise ValueError("Inconsistent ACDB calibration table")
    params = {}
    for index in range(count):
        instance, param = _words(chunks[b"CDDE"], definition + 4 + 8 * index, 2)
        if instance != source_instance:
            continue
        pointer, = _words(chunks[b"CDDO"], lookup + 4 + 4 * index, 1)
        length, = _words(pool, pointer, 1)
        payload = pool[pointer + 4:pointer + 4 + length]
        if not payload or len(payload) != length or param in params:
            raise ValueError("Invalid speaker DAP parameter")
        params[param] = payload
    if len(params) != 95 or _words(params[0x000108de], 0, 2) != (1, 4):
        raise ValueError("The speaker DAP calibration must describe four outputs")
    if next(reversed(params)) != 0x08001026 or params[0x08001026] != struct.pack("<I", 1):
        raise ValueError("The speaker DAP must be enabled after calibration")
    return params


def write_calibration(source, destination, policy):
    data = source.read_bytes()
    if sha256(data).hexdigest() not in policy["acdb_sha256"]:
        raise ValueError("Expected the original Piano Mi_acdb_cal.acdb from a supported HyperOS release")
    result = bytearray()
    for param, payload in _speaker_params(_chunks(data)).items():
        result.extend(struct.pack("<4I", policy["instance"], param, len(payload), 0))
        result.extend(payload)
        result.extend(bytes(-len(payload) % 8))
    # The ALSA AudioReach driver expects the payload byte count, followed by
    # the static-configuration type and two reserved words.
    destination.write_bytes(struct.pack("<4I", len(result), 0x01001006, 0, 0) + result)


def add_speaker_route(source, include):
    routes = (
        (b"``SECONDARY_TDM_RX_0'', ``MultiMedia1''",
         b"``SECONDARY_TDM_RX_0'', ``MultiMedia1'', ``MultiMedia4''"),
        (b"``MultiMedia1, stream0.logger1''",
         b"``MultiMedia1, stream0.logger1'', ``MultiMedia4, stream3.logger1''"),
    )
    for original, updated in routes:
        if source.count(original) != 1:
            raise ValueError(f"AudioReach speaker mixer changed: {original.decode()}")
        source = source.replace(original, updated)
    return source + f"\ninclude(`{include}')\n".encode()


def route_speaker(topology, instance):
    dap = topology.index(b'SectionVendorTuples."stream3.dap1_tuples"')
    widget = topology.index(b'SectionWidget."stream3.dap1"')
    end = topology.index(b'\n}\n', widget) + 3
    logger = topology.index(b'SectionVendorTuples."stream3.logger1_tuples"')
    topology = topology[:logger] + topology[dap:end] + b'\n' + topology[logger:dap] + topology[end:]
    mfc = re.compile(rb'(SectionVendorTuples\."stream3\.mfc1_tuples" \{.*?)'
                     rb'(AR_TKN_U32_MODULE_DST_INSTANCE_ID )"24837"', re.S)
    topology, matches = mfc.subn(lambda match: match[1] + match[2] + f'"{instance}"'.encode(), topology)
    if matches != 1:
        raise ValueError("AudioReach speaker MFC destination changed")

    original = b'"stream3.logger1, , stream3.mfc1"'
    if topology.count(original) != 1:
        raise ValueError("AudioReach speaker PCM route changed")
    topology = topology.replace(original, b'"stream3.dap1, , stream3.mfc1"\n'
                                b'                "stream3.logger1, , stream3.dap1"')
    return topology.replace(b'stream3.vol_ctrl3', b'stream3.vol3')
