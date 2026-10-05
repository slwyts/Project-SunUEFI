// SPDX-License-Identifier: BSD-2-Clause-Patent
// SEC-only, no native firmware calls or page table changes. A precise fault
// fixes up only the one load PC and exact FAR. Every other exception resets.
.text
.p2align 2
.globl PianoSecRead32
PianoSecRead32:
  mrs x9, CurrentEL
  cmp x9, #4
  b.ne .Lunsupported
  mrs x9, SPSel
  cmp x9, #1
  b.ne .Lunsupported
  mrs x9, sctlr_el1
  tst x9, #1
  b.ne .Lunsupported
  tst x9, #4
  b.ne .Lunsupported
  stp x19, x20, [sp, #-16]!
  mrs x19, daif
  msr daifset, #15
  mrs x20, vbar_el1
  str x20, [x2, #72]
  str x19, [x2, #80]
  str x0, [x2, #24]
  str xzr, [x2, #8]
  str xzr, [x2, #16]
  adr x9, .Lresume
  str x9, [x2, #64]
  mov x9, #1
  str x9, [x2]
  adr x9, PianoSecReadActiveState
  str x2, [x9]
  adr x9, PianoSecReadVectors
  msr vbar_el1, x9
  dsb sy
  isb
  // IRQ/FIQ/debug remain masked. A pending SError must reach the fatal vector,
  // rather than being silently deferred beyond restoration of the SEC VBAR.
  msr daifclr, #4
  mov w9, #0
.Lload:
  ldr w9, [x0]
.Lresume:
  dsb sy
  isb
  msr daifset, #15
  str xzr, [x2]
  ldr x10, [x2, #8]
  cbnz x10, .Lrestore
  str w9, [x1]
.Lrestore:
  msr vbar_el1, x20
  isb
  adr x9, PianoSecReadActiveState
  str xzr, [x9]
  msr daif, x19
  mov x0, x10
  ldp x19, x20, [sp], #16
  ret
.Lunsupported:
  mov x0, #2
  ret

.macro SLOT target
  b \target
  .space 124
.endm
.p2align 11
.globl PianoSecReadVectors
PianoSecReadVectors:
  SLOT .Lfatal
  SLOT .Lfatal
  SLOT .Lfatal
  SLOT .Lfatal
  SLOT .Lsync
  SLOT .Lfatal
  SLOT .Lfatal
  SLOT .Lfatal
  SLOT .Lfatal
  SLOT .Lfatal
  SLOT .Lfatal
  SLOT .Lfatal
  SLOT .Lfatal
  SLOT .Lfatal
  SLOT .Lfatal
  SLOT .Lfatal
.Lsync:
  stp x9, x10, [sp, #-64]!
  stp x11, x12, [sp, #16]
  stp x13, x14, [sp, #32]
  stp x15, x16, [sp, #48]
  mrs x9, elr_el1
  mrs x10, esr_el1
  mrs x11, far_el1
  mrs x12, spsr_el1
  adr x13, PianoSecReadActiveState
  ldr x13, [x13]
  cmp x13, x2
  b.ne .Lsavedfatal
  cbz x13, .Lsavedfatal
  stp x9, x10, [x2, #32]
  stp x11, x12, [x2, #48]
  ldr x13, [x2]
  cmp x13, #1
  b.ne .Lsavedfatal
  adr x13, .Lload
  cmp x9, x13
  b.ne .Lsavedfatal
  ldr x13, [x2, #24]
  cmp x11, x13
  b.ne .Lsavedfatal
  lsr x13, x10, #26
  cmp x13, #0x25
  b.ne .Lsavedfatal
  tbz x10, #25, .Lsavedfatal
  tbnz x10, #6, .Lsavedfatal
  tbnz x10, #10, .Lsavedfatal
  and x13, x12, #15
  cmp x13, #5
  b.ne .Lsavedfatal
.Lrecover:
  mov x13, #1
  str x13, [x2, #8]
  ldr x13, [x2, #64]
  msr elr_el1, x13
  ldp x15, x16, [sp, #48]
  ldp x13, x14, [sp, #32]
  ldp x11, x12, [sp, #16]
  ldp x9, x10, [sp], #64
  eret
.Lsavedfatal:
  add sp, sp, #64
  b .Lfatal
.Lfatal:
  // A fault in the fatal logger must not trust the logger's clobbered x2 or
  // overwrite the first syndrome. The producer-owned pointer remains valid
  // until VBAR restoration. Nested or ownerless faults go straight to reset.
  adr x13, PianoSecReadActiveState
  ldr x2, [x13]
  cbz x2, .Lreset
  ldr x14, [x2, #16]
  cbnz x14, .Lreset
  mov x14, #1
  str x14, [x2, #16]
  mrs x9, elr_el1
  mrs x10, esr_el1
  mrs x11, far_el1
  mrs x12, spsr_el1
  stp x9, x10, [x2, #32]
  stp x11, x12, [x2, #48]
.Lcallfatal:
  msr daifset, #15
  mov x0, x2
  bl PianoSecReadFatal
.Lreset:
  msr daifset, #15
  movz x0, #9
  movk x0, #0x8400, lsl #16
  smc #0
  b .
.data
.p2align 3
PianoSecReadActiveState:
  .quad 0
