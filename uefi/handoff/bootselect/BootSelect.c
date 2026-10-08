/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "BootSelect.h"
#include "BootRequest.h"
#include "EarlyTrace.h"

#define KERNEL_FIRST 0xA8000000ULL
#define KERNEL_END   0xB8000000ULL
#define MAX_IMAGE    0x08000000ULL

static void Trace(unsigned Stage,uint64_t Base,uint64_t Dtb) {
#if defined(__aarch64__)
  uint64_t El;__asm__ volatile("mrs %0, CurrentEL":"=r"(El));
  if(El!=4)return;
#endif
  PianoBootSelectTrace(Stage,Base,Dtb);
}

typedef struct {
  int Valid, Mode, Splash, StockRecovery;
  uint32_t Total;
  uint64_t Initrd, InitrdBytes;
} FDT_RESULT;

static uint32_t Be32(const uint8_t *P) {
  return ((uint32_t)P[0] << 24) | ((uint32_t)P[1] << 16) | ((uint32_t)P[2] << 8) | P[3];
}
static uint64_t Be64(const uint8_t *P) { return ((uint64_t)Be32(P) << 32) | Be32(P + 4); }
static int Equal(const uint8_t *P, size_t N, const char *Text) {
  size_t I = 0;
  while (Text[I]) { if (I >= N || P[I] != (uint8_t)Text[I]) return 0; ++I; }
  return I == N;
}
static int Range(uint64_t First, uint64_t Bytes, uint64_t End) {
  return Bytes && First <= End && Bytes <= End - First;
}
static int Overlap(uint64_t A, uint64_t N, uint64_t B, uint64_t M) {
  return N && M && A < B + M && B < A + N;
}
static int Space(uint8_t C) { return C == ' ' || C == '\t' || C == '\n' || C == '\r'; }

static int BootArgs(const uint8_t *P, uint32_t N, int *StockRecovery) {
  // The exact stock marker is present in both captured ABL MNTParam and the
  // real Mi Recovery kernel cmdline. It must take priority over persistent
  // NEXT requests. Other recovery-looking names are not inferred.
  static const char Key[] = "sunuefi.boot";
  static const char StockKey[] = "bootmonitor.bootmode";
  uint32_t I = 0, Seen = 0, StockSeen = 0;
  int ExplicitUefi = 0, StockValue = 0;
  *StockRecovery = 0;
  if (!N || N > 16384 || P[N - 1]) return PIANO_SELECT_NORMAL;
  for (I = 0; I + 1 < N; ++I) if (!P[I]) return PIANO_SELECT_NORMAL;
  I = 0;
  while (I + 1 < N) {
    uint32_t Start, Length, K = sizeof(Key) - 1;
    while (I + 1 < N && Space(P[I])) ++I;
    Start = I;
    while (I + 1 < N && !Space(P[I])) ++I;
    Length = I - Start;
    if (Length >= K && Equal(P + Start, K, Key) && (Length == K || P[Start + K] == '=')) {
      ++Seen;
      ExplicitUefi = Length == K + 5 && P[Start + K] == '=' && Equal(P + Start + K + 1, 4, "uefi");
    }
    K = sizeof(StockKey) - 1;
    if (Length >= K && Equal(P + Start, K, StockKey) && (Length == K || P[Start + K] == '=')) {
      ++StockSeen;
      StockValue = Length == K + 9 && P[Start + K] == '=' && Equal(P + Start + K + 1, 8, "recovery");
    }
  }
  *StockRecovery = StockSeen == 1 && StockValue;
  return !*StockRecovery && Seen == 1 && ExplicitUefi ? PIANO_SELECT_RECOVERY : PIANO_SELECT_NORMAL;
}

/* One bounded read-only walker serves mode selection, initrd and splash proof. */
static FDT_RESULT Walk(const void *Pointer, size_t Available) {
  FDT_RESULT R = {0, PIANO_SELECT_NORMAL, 0, 0, 0, 0, 0};
  const uint8_t *P = Pointer;
  uint32_t Total, Structure, Strings, Reserve, StringBytes, StructureBytes, Cursor, End;
  uint32_t Depth = 0, RootSeen = 0, Chosen = 0, Args = 0, StartSeen = 0, EndSeen = 0;
  uint32_t Reserved = 0, SplashNodes = 0, SplashRegs = 0;
  uint64_t InitrdStart = 0, InitrdEnd = 0;
  uint8_t Kind[64];
  int SplashReg = 0, Finished = 0, Mode = PIANO_SELECT_NORMAL, StockRecovery = 0;
  if (!P || Available < 40 || Be32(P) != 0xD00DFEEDU) return R;
  Total = Be32(P + 4); Structure = Be32(P + 8); Strings = Be32(P + 12); Reserve = Be32(P + 16);
  StringBytes = Be32(P + 32); StructureBytes = Be32(P + 36);
  if (Total < 40 || Total > Available || Total > PIANO_SELECT_MAX_FDT || Be32(P + 20) < 17 || Be32(P + 24) > 17 ||
      (Structure & 3) || (Reserve & 7) || Structure < 40 || Strings < 40 || Reserve < 40 ||
      !Range(Structure, StructureBytes, Total) || StringBytes > Total - Strings || Strings > Total ||
      Overlap(Structure, StructureBytes, Strings, StringBytes)) return R;
  Cursor = Reserve;
  for (;;) {
    if (!Range(Cursor, 16, Total)) return R;
    if (!Be64(P + Cursor) && !Be64(P + Cursor + 8)) { Cursor += 16; break; }
    Cursor += 16;
  }
  if (Overlap(Reserve, Cursor - Reserve, Structure, StructureBytes) || Overlap(Reserve, Cursor - Reserve, Strings, StringBytes)) return R;
  Cursor = Structure; End = Structure + StructureBytes;
  while (Range(Cursor, 4, End)) {
    uint32_t Token = Be32(P + Cursor); Cursor += 4;
    if (Token == 1) {
      uint32_t Start = Cursor, NameBytes;
      while (Cursor < End && P[Cursor]) ++Cursor;
      if (Cursor == End || Depth == sizeof(Kind)) return R;
      NameBytes = Cursor - Start;
      if (NameBytes > 255) return R;
      Kind[Depth] = 0;
      if (!Depth) { if (RootSeen || NameBytes) return R; RootSeen = 1; }
      else if (Depth == 1 && Equal(P + Start, NameBytes, "chosen")) { Kind[Depth] = 1; ++Chosen; }
      else if (Depth == 1 && Equal(P + Start, NameBytes, "reserved-memory")) { Kind[Depth] = 2; ++Reserved; }
      else if (Depth == 2 && Kind[1] == 2 && Equal(P + Start, NameBytes, "splash_region")) { Kind[Depth] = 3; ++SplashNodes; }
      ++Depth; Cursor = (Cursor + 4) & ~3U;
      if (Cursor > End) return R;
    } else if (Token == 2) {
      if (!Depth) return R;
      --Depth;
    } else if (Token == 3) {
      uint32_t Bytes, NameOffset, NameEnd;
      const uint8_t *Value, *Name;
      if (!Depth || !Range(Cursor, 8, End)) return R;
      Bytes = Be32(P + Cursor); NameOffset = Be32(P + Cursor + 4); Cursor += 8;
      if (NameOffset >= StringBytes || Bytes > End - Cursor) return R;
      NameEnd = NameOffset;
      while (NameEnd < StringBytes && P[Strings + NameEnd]) ++NameEnd;
      if (NameEnd == StringBytes) return R;
      Name = P + Strings + NameOffset; Value = P + Cursor;
      if (Depth == 2 && Kind[1] == 1) {
        if (Equal(Name, NameEnd - NameOffset, "bootargs")) { ++Args; Mode = BootArgs(Value, Bytes, &StockRecovery); }
        else if (Equal(Name, NameEnd - NameOffset, "linux,initrd-start")) {
          if ((Bytes != 4 && Bytes != 8) || StartSeen++) return R;
          InitrdStart = Bytes == 8 ? Be64(Value) : Be32(Value);
        } else if (Equal(Name, NameEnd - NameOffset, "linux,initrd-end")) {
          if ((Bytes != 4 && Bytes != 8) || EndSeen++) return R;
          InitrdEnd = Bytes == 8 ? Be64(Value) : Be32(Value);
        }
      } else if (Depth == 3 && Kind[2] == 3 && Equal(Name, NameEnd - NameOffset, "reg")) {
        ++SplashRegs;
        SplashReg = Bytes == 16 && Be64(Value) == 0xFC800000ULL && Be64(Value + 8) == 0x02B00000ULL;
      }
      Cursor += Bytes;
      if (Cursor > End - (3U & (0U - Cursor))) return R;
      Cursor = (Cursor + 3) & ~3U;
    } else if (Token == 4) {
      continue;
    } else if (Token == 9) {
      if (!RootSeen || Depth) return R;
      Finished = 1; break;
    } else return R;
  }
  if (!Finished || StartSeen != EndSeen || (StartSeen && (InitrdEnd <= InitrdStart))) return R;
  R.Valid = 1; R.Total = Total;
  R.Mode = Chosen == 1 && Args == 1 ? Mode : PIANO_SELECT_NORMAL;
  R.StockRecovery = Chosen == 1 && Args == 1 && StockRecovery;
  R.Splash = Reserved == 1 && SplashNodes == 1 && SplashRegs == 1 && SplashReg;
  R.Initrd = InitrdStart; R.InitrdBytes = StartSeen ? InitrdEnd - InitrdStart : 0;
  return R;
}

int PianoBootSelectParseFdt(const void *Fdt, size_t Available) { return Walk(Fdt, Available).Mode; }
int PianoBootSelectSplashFromFdt(const void *Fdt, size_t Available) { return Walk(Fdt, Available).Splash; }
int PianoEarlySplashAllowed(const void *Fdt) {
  uint64_t Address = (uintptr_t)Fdt;
  if ((Address & 7) || Address < KERNEL_FIRST || !Range(Address, 40, KERNEL_END)) return 0;
  return Walk(Fdt, (size_t)(KERNEL_END - Address < PIANO_SELECT_MAX_FDT ? KERNEL_END - Address : PIANO_SELECT_MAX_FDT)).Splash;
}

int PianoBootSelectMetadataValid(const PIANO_BOOT_SELECT_META *M, uint64_t SelectorBytes) {
  int64_t Target;
  if (!M || !Equal((const uint8_t *)M->Magic, 15, "SUNUEFI-SPLITv1") || M->Magic[15] || M->Version != 1 || M->Size != 128 ||
      M->OriginalImageSize < 64 || M->OriginalImageSize > MAX_IMAGE || M->SelectorOffset < M->OriginalImageSize ||
      (M->SelectorOffset & 4095) || !SelectorBytes || !Range(M->SelectorOffset, SelectorBytes, M->ContainerBytes) ||
      M->ContainerBytes > MAX_IMAGE || M->BootShimOffset < M->SelectorOffset + SelectorBytes ||
      M->BootShimBytes < 64 || M->FdBytes != 0x300000 ||
      !Range(M->BootShimOffset, M->BootShimBytes, M->ContainerBytes) ||
      !Range(M->BootShimOffset + M->BootShimBytes, M->FdBytes, M->ContainerBytes) ||
      M->AppOffset < M->BootShimOffset + M->BootShimBytes + M->FdBytes || M->AppBytes < 64 ||
      !Range(M->AppOffset, M->AppBytes, M->ContainerBytes) || (M->OriginalCode0 & 0xFFFF) != 0x5A4D ||
      (M->OriginalCode1 >> 26) != 5) return 0;
  Target = 4 + ((int64_t)(int32_t)(M->OriginalCode1 << 6) >> 4);
  return Target >= 64 && (uint64_t)Target < M->OriginalImageSize;
}

int PianoBootSelectRecoveryLayout(const PIANO_BOOT_SELECT_META *M, uint64_t Base,
  uint64_t SelectorBytes, uint64_t Dtb, uint64_t DtbBytes, uint64_t Initrd, uint64_t InitrdBytes) {
  if (!PianoBootSelectMetadataValid(M, SelectorBytes) || (Base & 0x1FFFFF) || Base < KERNEL_FIRST ||
      !Range(Base, M->ContainerBytes, KERNEL_END) || Dtb < KERNEL_FIRST || DtbBytes > PIANO_SELECT_MAX_FDT ||
      !Range(Dtb, DtbBytes, KERNEL_END) || Overlap(Base, M->ContainerBytes, Dtb, DtbBytes) ||
      Overlap(Base, M->ContainerBytes, 0xA7100000, 0x300000) ||
      Overlap(Base, M->ContainerBytes, 0xA7FFF000, 0x1000) ||
      Overlap(Dtb, DtbBytes, 0xA7100000, 0x300000) || Overlap(Dtb, DtbBytes, 0xA7FFF000, 0x1000)) return 0;
  if (InitrdBytes && (Initrd < KERNEL_FIRST || !Range(Initrd, InitrdBytes, KERNEL_END) ||
      Overlap(Base, M->ContainerBytes, Initrd, InitrdBytes) || Overlap(Dtb, DtbBytes, Initrd, InitrdBytes) ||
      Overlap(Initrd, InitrdBytes, 0xA7100000, 0x300000) || Overlap(Initrd, InitrdBytes, 0xA7FFF000, 0x1000))) return 0;
  return 1;
}

int PianoBootSelectEntry(const PIANO_BOOT_SELECT_META *M, const void *Fdt, uint64_t Base, uint64_t SelectorBytes) {
  FDT_RESULT R;
  unsigned Requested = 0;
  uint64_t Address = (uintptr_t)Fdt;
  if (!PianoBootSelectMetadataValid(M, SelectorBytes) || Base > UINT64_MAX - M->ContainerBytes) return PIANO_SELECT_INVALID_METADATA;
  Trace(2,Base,Address);
  if ((Address & 7) || Address < KERNEL_FIRST || !Range(Address, 40, KERNEL_END)) return PIANO_SELECT_NORMAL;
  Trace(3,Base,Address);
  R = Walk(Fdt, (size_t)(KERNEL_END - Address < PIANO_SELECT_MAX_FDT ? KERNEL_END - Address : PIANO_SELECT_MAX_FDT));
  if(!R.Valid || !PianoBootSelectRecoveryLayout(M, Base, SelectorBytes, Address, R.Total, R.Initrd, R.InitrdBytes))
    return PIANO_SELECT_NORMAL;
  // NORMAL returns to the untouched original GKI entry with ABL's recovery
  // DTB/initrd. Do not read, consume or clear NEXT pages on this route.
  if(R.StockRecovery) { Trace(4,Base,Address); return PIANO_SELECT_NORMAL; }
  uint64_t Pages = (M->OriginalImageSize + 4095U) & ~4095ULL;
  if(Pages <= M->SelectorOffset && PIANO_BOOT_REQUEST_PAGES_BYTES <= M->SelectorOffset-Pages)
    Requested = PianoBootRequestSelect((const void *)(uintptr_t)(Base+Pages), M->AppSha256);
  if(!Requested && R.Mode!=PIANO_SELECT_RECOVERY)Trace(4,Base,Address);
  // Preserve the exact request target for the product policy. Consumption on
  // persistent storage remains a separate operation.
  return Requested ? (int)Requested : R.Mode == PIANO_SELECT_RECOVERY ? PIANO_SELECT_RECOVERY : PIANO_SELECT_NORMAL;
}
