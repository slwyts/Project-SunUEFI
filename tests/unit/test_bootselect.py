import ctypes
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'uefi/handoff/bootselect'
TOOLS = ROOT / 'build/host-tools/usr/bin'


def tool(name):
    return shutil.which(name) or (str(TOOLS / name) if (TOOLS / name).is_file() else None)


def dtb(args=b'sunuefi.boot=uefi\0', duplicate=False, initrd=None, splash=None):
    strings = b'bootargs\0linux,initrd-start\0linux,initrd-end\0reg\0'
    offsets = {key: strings.index(key + b'\0') for key in (b'bootargs', b'linux,initrd-start', b'linux,initrd-end', b'reg')}
    pad = lambda data: data + b'\0' * (-len(data) % 4)
    node = lambda name: struct.pack('>I', 1) + pad(name + b'\0')
    prop = lambda name, data: struct.pack('>III', 3, len(data), offsets[name]) + pad(data)
    body = node(b'') + node(b'chosen') + prop(b'bootargs', args)
    if duplicate:
        body += prop(b'bootargs', args)
    if initrd is not None:
        body += prop(b'linux,initrd-start', struct.pack('>Q', initrd[0]))
        body += prop(b'linux,initrd-end', struct.pack('>Q', initrd[1]))
    body += struct.pack('>I', 2)
    if splash is not None:
        body += node(b'reserved-memory') + node(b'splash_region') + prop(b'reg', struct.pack('>QQ', *splash)) + struct.pack('>II', 2, 2)
    body += struct.pack('>II', 2, 9)
    return struct.pack('>10I', 0xd00dfeed, 56 + len(body) + len(strings), 56, 56 + len(body), 40, 17, 16, 0, len(strings), len(body)) + b'\0' * 16 + body + strings


class Meta(ctypes.Structure):
    _fields_ = [('magic', ctypes.c_char * 16), ('version', ctypes.c_uint32), ('size', ctypes.c_uint32),
                ('selector', ctypes.c_uint64), ('original_size', ctypes.c_uint64),
                ('code0', ctypes.c_uint32), ('code1', ctypes.c_uint32),
                ('shim', ctypes.c_uint64), ('shim_bytes', ctypes.c_uint64), ('fd_bytes', ctypes.c_uint64),
                ('app', ctypes.c_uint64), ('app_bytes', ctypes.c_uint64), ('container', ctypes.c_uint64),
                ('sha', ctypes.c_uint8 * 32)]


def meta():
    return Meta(b'SUNUEFI-SPLITv1\0', 1, 128, 0x23c0000, 0x23c0000,
                0xfa405a4d, 0x1473f019, 0x23c4000, 0x200, 0x300000, 0x26c4200, 0x1000, 0x26c5200)


class BootSelectTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='bootselect-tests-')
        cls.base = Path(cls.temp.name)
        stub=cls.base/'trace-stub.c'
        stub.write_text('#include <stdint.h>\nvoid PianoBootSelectTrace(unsigned S,uint64_t B,uint64_t D){(void)S;(void)B;(void)D;}\n')
        subprocess.run([tool('cc'), '-shared', '-fPIC', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        SOURCE / 'BootSelect.c', SOURCE / 'BootRequest.c', stub, '-o', cls.base / 'parser.so'], check=True)
        cls.lib = ctypes.CDLL(str(cls.base / 'parser.so'))
        cls.lib.PianoBootSelectParseFdt.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.PianoEarlySplashAllowed.argtypes = [ctypes.c_void_p]
        cls.lib.PianoBootSelectSplashFromFdt.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.PianoBootSelectMetadataValid.argtypes = [ctypes.POINTER(Meta), ctypes.c_uint64]
        cls.lib.PianoBootSelectRecoveryLayout.argtypes = [ctypes.POINTER(Meta)] + [ctypes.c_uint64] * 6
        cls.lib.PianoBootSelectEntry.argtypes = [ctypes.POINTER(Meta), ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def parse(self, data, available=None):
        buffer = ctypes.create_string_buffer(data)
        return self.lib.PianoBootSelectParseFdt(buffer, len(data) if available is None else available)

    def test_only_exact_unique_request_selects_uefi(self):
        self.assertEqual(self.parse(dtb()), 1)
        for args in (b'bootmonitor.bootmode=normal\0', b'bootmonitor.bootmode=recovery\0',
                     b'androidboot.bootmode=recovery\0', b'sunuefi.boot=uefi-extra\0', b'sunuefi.boot\0',
                     b'sunuefi.boot=uefi sunuefi.boot=uefi\0',
                     b'sunuefi.boot=android sunuefi.boot=uefi\0', b'sunuefi.boot=uefi\0hidden\0'):
            with self.subTest(args=args):
                self.assertEqual(self.parse(dtb(args)), 0)
        self.assertEqual(self.parse(dtb(duplicate=True)), 0)

    def test_entry_reads_request_pages_and_preserves_both_stock_modes(self):
        libc = ctypes.CDLL(None)
        libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_long]
        libc.mmap.restype = ctypes.c_void_p
        base, tree = 0xA8000000, 0xB5000000
        first = libc.mmap(base, 0x10000, 3, 0x100022, -1, 0)
        second = libc.mmap(tree, 0x1000, 3, 0x100022, -1, 0)
        self.assertEqual((first, second), (base, tree))
        try:
            m = meta();m.original_size = 0x2000;m.selector = 0x4000;m.shim = 0x8000
            m.app = m.shim + m.shim_bytes + m.fd_bytes;m.container = m.app + m.app_bytes
            m.code1 = 0x1400003f;m.sha[:] = bytes(range(1, 33))
            def page(sequence, target):
                record = bytearray(struct.pack('<16sIIQII16s8s', b'SUNUEFI-NEXTv1', 1, 64,
                                              sequence, target, 0, bytes(m.sha)[:16], bytes(8)))
                struct.pack_into('<I', record, 36, zlib.crc32(record))
                return bytes(record)
            ctypes.memmove(base+0x2000, page(0, 0), 64);ctypes.memmove(base+0x3000, page(0, 0), 64)
            for stock_mode in ('normal', 'recovery'):
                data = dtb(('bootmonitor.bootmode='+stock_mode+'\0').encode())
                ctypes.memmove(tree, data, len(data))
                self.assertEqual(self.lib.PianoBootSelectEntry(ctypes.byref(m), tree, base, 0x4000), 0)
            # Normal mode still follows the highest valid persistent request.
            data = dtb(b'bootmonitor.bootmode=normal\0')
            ctypes.memmove(tree, data, len(data))
            ctypes.memmove(base+0x3000, page(1, 3), 64)
            self.assertEqual(self.lib.PianoBootSelectEntry(ctypes.byref(m), tree, base, 0x4000), 3)
            ctypes.memmove(base+0x3000, page(1, 2), 64)
            self.assertEqual(self.lib.PianoBootSelectEntry(ctypes.byref(m), tree, base, 0x4000), 2)
            # Stock Recovery wins over that same valid NEXT2, without changing it.
            saved = ctypes.string_at(base+0x2000, 0x2000)
            data = dtb(b'bootmonitor.bootmode=recovery sunuefi.boot=uefi\0')
            ctypes.memmove(tree, data, len(data))
            self.assertEqual(self.lib.PianoBootSelectEntry(ctypes.byref(m), tree, base, 0x4000), 0)
            self.assertEqual(ctypes.string_at(base+0x2000, 0x2000), saved)
            data = dtb(b'bootmonitor.bootmode=normal\0')
            ctypes.memmove(tree, data, len(data))
            ctypes.memmove(base+0x2000, page(2, 0), 64)
            self.assertEqual(self.lib.PianoBootSelectEntry(ctypes.byref(m), tree, base, 0x4000), 0)
        finally:
            libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
            libc.munmap(first, 0x10000);libc.munmap(second, 0x1000)

    def test_invalid_fdt_bounds_and_structure_fall_back_without_mutation(self):
        good = dtb()
        variants = [good[:39], good[:-1]]
        for offset, value in ((0, 0), (4, 0x200001), (8, 0xfffffff0), (12, 0xfffffff0), (16, 41), (36, 0xffffffff)):
            bad = bytearray(good)
            struct.pack_into('>I', bad, offset, value)
            variants.append(bytes(bad))
        for data in variants:
            buffer = ctypes.create_string_buffer(data)
            before = bytes(buffer)
            self.assertEqual(self.lib.PianoBootSelectParseFdt(buffer, len(data)), 0)
            self.assertEqual(bytes(buffer), before)
        self.assertEqual(self.parse(good, len(good) - 1), 0)
        self.assertEqual(self.parse(dtb(initrd=(0xB1000000, 0xB0000000))), 0)

    def test_splash_requires_exact_valid_reserved_region(self):
        for value, expected in (((0xFC800000, 0x2B00000), 1), ((0xFC800000, 0x2A00000), 0), ((0xFC900000, 0x2B00000), 0)):
            data = dtb(splash=value)
            buffer = ctypes.create_string_buffer(data)
            self.assertEqual(self.lib.PianoBootSelectSplashFromFdt(buffer, len(data)), expected)
            self.assertEqual(self.lib.PianoEarlySplashAllowed(buffer), 0)
        self.assertEqual(self.lib.PianoEarlySplashAllowed(ctypes.create_string_buffer(dtb())), 0)

    def test_metadata_and_nonoverlap_admission(self):
        m = meta()
        self.assertEqual(ctypes.sizeof(m), 128)
        self.assertEqual(self.lib.PianoBootSelectMetadataValid(ctypes.byref(m), 0x4000), 1)
        layout = self.lib.PianoBootSelectRecoveryLayout
        self.assertEqual(layout(ctypes.byref(m), 0xA8000000, 0x4000, 0xB5000000, 0x1000, 0xB0200000, 0x1000000), 1)
        for base, tree, rd in ((0xA7100000, 0xB5000000, 0xB0200000), (0xA8000000, 0xA8001000, 0xB0200000),
                               (0xA8000000, 0xB5000000, 0xA9000000), (0xB6000000, 0xB5000000, 0xB0200000)):
            self.assertEqual(layout(ctypes.byref(m), base, 0x4000, tree, 0x1000, rd, 0x1000000), 0)
        m.shim = m.selector + 0x100
        self.assertEqual(self.lib.PianoBootSelectMetadataValid(ctypes.byref(m), 0x4000), 0)

    def cross_build(self):
        linker, objcopy = tool('aarch64-linux-gnu-ld'), tool('aarch64-linux-gnu-objcopy')
        if not tool('clang') or not linker or not objcopy:
            self.skipTest('AArch64 compiler/binutils unavailable')
        out = self.base / 'cross'
        subprocess.run(['make', '-C', SOURCE, 'OUT=' + str(out), 'BOOTSELECT_LD=' + linker,
                        'BOOTSELECT_OBJCOPY=' + objcopy], check=True, capture_output=True)
        nm = tool('aarch64-linux-gnu-nm')
        symbols = subprocess.check_output([nm, '-n', out / 'bootselect.elf'], text=True)
        values = {parts[2]: int(parts[0], 16) for line in symbols.splitlines() if len(parts := line.split()) == 3}
        return out, values

    def test_flat_link_labels_stack_and_no_unresolved_relocations(self):
        out, values = self.cross_build()
        self.assertEqual(values['_start'], 0)
        self.assertEqual(values['__stack_end'] - values['__stack_start'], 8192)
        self.assertGreaterEqual(values['__image_end'], values['__stack_end'])
        self.assertGreater(values['__image_end'], (out / 'bootselect.bin').stat().st_size)
        self.assertEqual((out / 'bootselect.bin').read_bytes()[values['bootselect_metadata']:values['bootselect_metadata'] + 128], b'\0' * 128)
        reloc = subprocess.check_output([tool('aarch64-linux-gnu-readelf'), '-r', out / 'bootselect.elf'], text=True)
        self.assertIn('no relocations', reloc)
        disasm = subprocess.check_output([tool('aarch64-linux-gnu-objdump'), '-d', out / 'bootselect.elf'], text=True)
        self.assertIn('dc\tcvac', disasm)
        self.assertNotIn('ic\tivau', disasm)  # NORMAL never modifies executable code.
        self.assertNotIn('\tsmc\t', disasm)

    def test_qemu_actual_normal_and_recovery_branch(self):
        out, values = self.cross_build()
        self.assert_qemu_branches((out / 'bootselect.bin').read_bytes(), values, 'makefile')

    def test_production_selector_qemu_alignment_and_both_branches(self):
        toolroot = Path(os.environ.get('SUNUEFI_TOOLCHAIN_ROOT', TOOLS.parent))
        missing = [name for name in ('clang', 'ld.lld', 'llvm-objcopy', 'llvm-nm')
                   if not (toolroot / 'bin' / name).is_file() or not os.access(toolroot / 'bin' / name, os.X_OK)]
        if missing:
            self.skipTest('Production selector toolchain unavailable: ' + ', '.join(missing))
        if not (ROOT / 'upstream/Mu-Silicium/Mu_Basecore/MdePkg/Include').is_dir():
            self.skipTest('Production selector needs the Mu MdePkg headers')
        self.qemu_tools()
        sys.path.insert(0, str(ROOT / 'tools'))
        from package_product_trampoline import build_selector
        out = self.base / 'production'
        out.mkdir()
        frontend, values, _ = build_selector(out)
        self.assert_qemu_branches(frontend, values, 'production')

    def qemu_tools(self):
        qemu = tool('qemu-system-aarch64')
        if not qemu:
            self.skipTest('QEMU system emulator unavailable')
        if not tool('clang') or not tool('aarch64-linux-gnu-ld'):
            self.skipTest('QEMU harness needs an AArch64 compiler/linker')
        return qemu

    def assert_qemu_branches(self, binary, values, build_name):
        qemu = self.qemu_tools()
        out = self.base / ('qemu-' + build_name)
        out.mkdir()
        frontend = bytearray(binary)
        span = (values['__image_end'] + 4095) & ~4095
        shim = 0x2000 + span
        app = shim + 0x100 + 0x300000
        total = app + 64
        m = Meta(b'SUNUEFI-SPLITv1\0', 1, 128, 0x2000, 0x2000, 0xfa405a4d, 0x1400003f,
                 shim, 0x100, 0x300000, app, 64, total)
        frontend[values['bootselect_metadata']:values['bootselect_metadata'] + 128] = bytes(m)
        frontend.extend(b'\0' * (span - len(frontend)))
        blob = out / 'frontend.bin'; blob.write_bytes(frontend)
        for mode in ('normal', 'recovery'):
            # FDT guarantees 4-byte property alignment, not 8-byte alignment.
            # The extra spaces place these 64-bit initrd cells at offset % 8=4.
            # Run with SCTLR.A=1 so the compiler cannot silently widen readers.
            args = b'sunuefi.boot=uefi    \0' if mode == 'recovery' else b'bootmonitor.bootmode=normal    \0'
            data = dtb(args, initrd=(0xB6000000, 0xB6010000))
            self.assertEqual(data.index(struct.pack('>Q', 0xB6000000)) % 8, 4)
            tree = out / 'tree.bin'; tree.write_bytes(data)
            source = out / 'harness.S'
            source.write_text(HARNESS.replace('@FRONT@', str(blob)).replace('@DTB@', str(tree)))
            script = out / 'harness.ld'
            script.write_text('ENTRY(harness_start)\nSECTIONS { . = 0x40080000; .harness : { *(.harness) } . = 0xA8000000; .kernel : { *(.kernel) } . = 0xB5000000; .dtb : { *(.dtb) } /DISCARD/ : { *(.note*) *(.comment) } }\n')
            obj, elf = out / 'harness.o', out / 'harness.elf'
            subprocess.run([tool('clang'), '--target=aarch64-linux-gnu', '-c', source, '-o', obj], check=True)
            subprocess.run([tool('aarch64-linux-gnu-ld'), '-T', script, obj, '-o', elf], check=True)
            result = subprocess.run([qemu, '-machine', 'virt', '-cpu', 'cortex-a57', '-m', '2G', '-nographic',
                                     '-nodefaults', '-serial', 'none', '-monitor', 'none',
                                     '-semihosting-config', 'enable=on,target=native', '-kernel', elf], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS ' + mode, result.stdout + result.stderr)


HARNESS = r'''
.section .harness,"ax"
.global harness_start
harness_start:
  ldr x8, =0x40100000
  mov sp, x8
  mrs x8, sctlr_el1
  orr x8, x8, #2
  msr sctlr_el1, x8
  isb
  mov x8, #0x240
  msr daif, x8
  ldr x0, =0xB5000000
  mov x1, #0x1111
  mov x2, #0x2222
  mov x3, #0x3333
  ldr x9, =0xA7FFF100
  ldr x10, =0x1122334455667788
  mov x11, #4
1: stp x10,x10,[x9],#16
  subs x11,x11,#1
  b.ne 1b
  ldr x4, =kernel_base
  br x4
verify_common:
  ldr x8, =0xB5000000
  cmp x0,x8
  b.ne fail
  mov x8,#0x1111
  cmp x1,x8
  b.ne fail
  mov x8,#0x2222
  cmp x2,x8
  b.ne fail
  mov x8,sp
  ldr x9,=0x40100000
  cmp x8,x9
  b.ne fail
  mrs x8,daif
  cmp x8,#0x240
  b.ne fail
  ret
normal_marker:
  bl verify_common
  mov x8,#0x3333
  cmp x3,x8
  b.ne fail
  ldr x9,=kernel_base
  ldr x8,[x9,#16]
  ldr x10,=container_end-kernel_base
  cmp x8,x10
  b.ne fail
  ldr w8,[x9]
  ldr w10,=0xfa405a4d
  cmp w8,w10
  b.ne fail
  ldr w8,[x9,#4]
  ldr w10,=0x140007ff
  cmp w8,w10
  b.ne fail
  ldr x9,=0xA7FFF100
  ldr x10,=0x1122334455667788
  mov x11,#8
2: ldr x8,[x9],#8
  cmp x8,x10
  b.ne fail
  subs x11,x11,#1
  b.ne 2b
  adr x1,normal_message
  b passed
rec_marker:
  bl verify_common
  ldr x9,=0xA7FFF100
  cmp x3,x9
  b.ne fail
  adr x10,record_magic
  ldp x8,x11,[x9]
  ldp x12,x13,[x10]
  cmp x8,x12
  b.ne fail
  cmp x11,x13
  b.ne fail
  ldp w8,w10,[x9,#16]
  cmp w8,#2
  b.ne fail
  cmp w10,#64
  b.ne fail
  ldr x8,[x9,#24]
  ldr x10,=app_payload
  cmp x8,x10
  b.ne fail
  ldr x8,[x9,#32]
  cmp x8,#64
  b.ne fail
  ldr x8,[x9,#40]
  ldr x10,=kernel_base
  cmp x8,x10
  b.ne fail
  ldr x8,[x9,#48]
  ldr x10,=container_end-kernel_base
  cmp x8,x10
  b.ne fail
  ldr x8,[x9,#56]
  cmp x8,#1
  b.ne fail
  adr x1,rec_message
passed:
  mov x0,#4
  hlt #0xf000
  adr x1,exit_ok
  mov x0,#0x20
  hlt #0xf000
fail:
  adr x1,fail_message
  mov x0,#4
  hlt #0xf000
  adr x1,exit_fail
  mov x0,#0x20
  hlt #0xf000
normal_message: .asciz "PASS normal\n"
rec_message: .asciz "PASS recovery\n"
fail_message: .asciz "FAIL bootselect\n"
.balign 8
exit_ok: .quad 0x20026,0
exit_fail: .quad 0x20026,1
record_magic: .ascii "SUNUEFI-EMBEDv1\0"
.ltorg
.section .kernel,"ax"
kernel_base:
  .word 0xfa405a4d
  b frontend
  .quad 0
  .quad container_end-kernel_base
  .quad 0xa,0,0,0
  .ascii "ARM\x64"
  .word 0x40
  .org 0x100
  ldr x4,=normal_marker
  br x4
  .ltorg
  .org 0x2000
frontend: .incbin "@FRONT@"
bootshim:
  ldr x4,=rec_marker
  br x4
  .ltorg
  .org bootshim-kernel_base+0x100
  .space 0x300000
app_payload: .space 64
container_end:
.section .dtb,"a"
  .incbin "@DTB@"
'''


if __name__ == '__main__':
    unittest.main()
