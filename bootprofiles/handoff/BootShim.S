/* Based on Project-Silicium BootShim. SPDX-License-Identifier: BSD-2-Clause */
_Head:
  adr x1, _Payload
  b _Start
  .quad FD_BASE
  .quad FD_SIZE
  .quad 0
  .quad 0
  .quad 0
  .quad 0
  .ascii "ARM\x64"
  .long 0
_Start:
  /* Preserve only the Linux ABI DTB argument in a dedicated RAM page. */
  ldr x7, =0xA7FFF000
  ldr x8, =0x534E554546494448
  stp x8, x0, [x7]
  mrs x9, CurrentEL
  str x9, [x7, #16]
  mov x4, x1
  ldr x5, =FD_BASE
  cmp x4, x5
  beq _Entry
  ldr x6, =FD_SIZE
_CopyLoop:
  ldp x2, x3, [x4], #16
  stp x2, x3, [x5], #16
  subs x6, x6, #16
  b.ne _CopyLoop
  ldr x5, =FD_BASE
_Entry:
  br x5
.text
/* Emit all literal data before the label that identifies the appended FD. */
.ltorg
.align 4
_Payload:
