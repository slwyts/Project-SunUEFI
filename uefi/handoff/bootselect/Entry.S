/* SPDX-License-Identifier: BSD-2-Clause-Patent */
.section .text.entry,"ax",%progbits
.global _start
.type _start,%function
_start:
  mov x22, x0
  mov x23, x1
  mov x24, x2
  mov x25, x3
  mov x26, sp
  mrs x27, daif
  msr daifset, #15
  mrs x9, CurrentEL
  cmp x9, #4
  b.eq .Lel_ok
  cmp x9, #8
  b.ne .Lstop
.Lel_ok:
  adr x19, _start
  adr x20, bootselect_metadata
  ldr x9, [x20, #24]
  mov x10, #0x08000000
  cmp x9, x10
  b.hs .Lstop
  cmp x19, x9
  b.lo .Lstop
  sub x19, x19, x9
  adr x9, __stack_end
  mov sp, x9
  mrs x9, CurrentEL
  cmp x9, #4
  b.ne .Ltrace_entered_done
  mov w0, #1
  mov x1, x19
  mov x2, x22
  bl PianoBootSelectTrace
.Ltrace_entered_done:
  mov x0, x20
  mov x1, x22
  mov x2, x19
  adr x3, __image_end
  adr x9, _start
  sub x3, x3, x9
  bl PianoBootSelectEntry
  tbnz w0, #31, .Lstop
  mov w21, w0
  cbnz w21, .Lrecovery
.Lnormal:
  mrs x9, CurrentEL
  cmp x9, #4
  b.ne .Ltrace_stock_done
  mov w0, #6
  mov x1, x19
  mov x2, x22
  bl PianoBootSelectTrace
.Ltrace_stock_done:
  // Enter the stock primary_entry directly after the inherited-framebuffer
  // picker. Stock Recovery bypasses that picker. No executable kernel writes.
  ldr w8, [x20, #44]
  sbfx x8, x8, #0, #26
  add x4, x19, #4
  add x4, x4, x8, lsl #2
  mov x3, x25
  b .Lrestore
.Lrecovery:
  mov x28, x21
  mrs x9, CurrentEL
  cmp x9, #4
  b.ne .Ltrace_uefi_done
  mov w0, #5
  mov x1, x19
  mov x2, x22
  bl PianoBootSelectTrace
.Ltrace_uefi_done:
  mov x0, x22
  bl PianoEarlyDrawSplash
  movz x21, #0xf100
  movk x21, #0xa7ff, lsl #16
  adr x10, .Lrecord_magic
  ldp x8, x9, [x10]
  stp x8, x9, [x21]
  mov w8, #2
  mov w9, #64
  stp w8, w9, [x21, #16]
  ldr x8, [x20, #72]
  add x8, x19, x8
  ldr x9, [x20, #80]
  stp x8, x9, [x21, #24]
  ldr x9, [x20, #88]
  stp x19, x9, [x21, #40]
  str x28, [x21, #56]
  mov x0, x21
  mov x1, #64
  bl PianoBootSelectCleanPoC
  ldr x4, [x20, #48]
  add x4, x19, x4
  mov x3, x21
.Lrestore:
  mov sp, x26
  mov x0, x22
  mov x1, x23
  mov x2, x24
  msr daif, x27
  isb
  br x4
.Lstop:
  wfe
  b .Lstop
.size _start, .-_start

/* CTR-derived line sizes, PoC data clean, no cache/MMU control changes. */
.global PianoBootSelectCleanPoC
.type PianoBootSelectCleanPoC,%function
PianoBootSelectCleanPoC:
  mrs x9, ctr_el0
  ubfx x10, x9, #16, #4
  mov x11, #4
  lsl x11, x11, x10
  sub x10, x11, #1
  bic x10, x0, x10
  add x12, x0, x1
1:
  dc cvac, x10
  add x10, x10, x11
  cmp x10, x12
  b.lo 1b
  dsb sy
  ret
.size PianoBootSelectCleanPoC, .-PianoBootSelectCleanPoC
.weak PianoEarlyDrawSplash
.type PianoEarlyDrawSplash,%function
PianoEarlyDrawSplash:
  ret
.size PianoEarlyDrawSplash, .-PianoEarlyDrawSplash

.section .rodata,"a",%progbits
.balign 16
.Lrecord_magic:
  .ascii "SUNUEFI-EMBEDv1\0"
.global bootselect_metadata
.type bootselect_metadata,%object
.balign 16
bootselect_metadata:
  .zero 128
.size bootselect_metadata, 128

.section .note.GNU-stack,"",%progbits
