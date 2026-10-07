/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "EarlyTrace.h"

// Ordinary passthrough must only access its loaded image and the supplied FDT.
// The retained console region is not yet proven accessible at this early entry.
// Firmware runtime logging remains enabled after EDK2 establishes its memory map.
#if defined(HOSTTEST) || defined(PIANO_BOOTSELECT_EARLY_TRACE)

typedef struct {
  volatile uint32_t Signature, Start, Size;
  uint8_t Data[];
} TRACE_RING;

_Static_assert(sizeof(PIANO_BOOTSELECT_TRACE_ID) > 1 && sizeof(PIANO_BOOTSELECT_TRACE_ID) <= 17,
               "trace ID must be a nonempty string of at most 16 bytes");

static unsigned Text(uint8_t *Out, unsigned At, const char *Value) {
  while (*Value) Out[At++] = (uint8_t)*Value++;
  return At;
}
static unsigned Hex(uint8_t *Out, unsigned At, uint64_t Value) {
  static const char Digits[] = "0123456789abcdef";
  unsigned I;
  for (I = 0; I < 16; ++I) Out[At++] = (uint8_t)Digits[(Value >> (60 - 4 * I)) & 15];
  return At;
}

void PianoBootSelectTrace(unsigned Stage, uint64_t ImageBase, uint64_t Dtb) {
  TRACE_RING *Ring;
  uint8_t Marker[96];
  uint32_t Start, Size, Next;
  unsigned Length = 0, First, I;
  if (Stage < PIANO_EARLY_TRACE_ENTERED || Stage > PIANO_EARLY_TRACE_BEFORE_STOCK_JUMP) return;
#ifdef HOSTTEST
  Ring = PianoBootSelectTraceHostRam;
#else
  Ring = (TRACE_RING *)(uintptr_t)0xA3500000ULL;
#endif
  if (!Ring || Ring->Signature != 0x43474244U) return;
  Start = Ring->Start; Size = Ring->Size;
  if (Start >= PIANO_EARLY_TRACE_CAPACITY || Size > PIANO_EARLY_TRACE_CAPACITY ||
      (Size < PIANO_EARLY_TRACE_CAPACITY && Start > Size)) return;
  Length = Text(Marker, Length, "[SUNUEFI-EARLY] id=");
  Length = Text(Marker, Length, PIANO_BOOTSELECT_TRACE_ID);
  Length = Text(Marker, Length, " stage=");
  Marker[Length++] = (uint8_t)('0' + Stage);
  Length = Text(Marker, Length, " image=0x");
  Length = Hex(Marker, Length, ImageBase);
  Length = Text(Marker, Length, " dtb=0x");
  Length = Hex(Marker, Length, Dtb);
  Marker[Length++] = '\n';
  First = PIANO_EARLY_TRACE_CAPACITY - Start;
  if (First > Length) First = Length;
  for (I = 0; I < First; ++I) Ring->Data[Start + I] = Marker[I];
  PianoBootSelectCleanPoC(Ring->Data + Start, First);
  if (Length > First) {
    for (I = First; I < Length; ++I) Ring->Data[I - First] = Marker[I];
    PianoBootSelectCleanPoC(Ring->Data, Length - First);
  }
  Next = Start + Length;
  if (Next >= PIANO_EARLY_TRACE_CAPACITY) Next -= PIANO_EARLY_TRACE_CAPACITY;
  Ring->Start = Next;
  Ring->Size = Size > PIANO_EARLY_TRACE_CAPACITY - Length ? PIANO_EARLY_TRACE_CAPACITY : Size + Length;
  PianoBootSelectCleanPoC(Ring, PIANO_EARLY_TRACE_HEADER_BYTES);
}

#else
void PianoBootSelectTrace(unsigned Stage, uint64_t ImageBase, uint64_t Dtb) {
  (void)Stage;
  (void)ImageBase;
  (void)Dtb;
}
#endif
