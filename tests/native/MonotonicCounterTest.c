// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#define MDEPKG_NDEBUG
#include "PianoNvFile.c"
#include "MonotonicCounter.c"

EFI_GUID gMtcVendorGuid, gEfiMonotonicCounterArchProtocolGuid, gEfiFileInfoGuid;
EFI_GUID gEfiPartitionInfoProtocolGuid, gEfiSimpleFileSystemProtocolGuid, gEfiFileSystemInfoGuid;
EFI_GUID gEfiGlobalVariableGuid, gEfiEventExitBootServicesGuid;
STATIC EFI_TPL Tpl = TPL_APPLICATION;
STATIC BOOLEAN Runtime, Installed, Present, ReadOnly;
STATIC UINT32 Variable;
STATIC UINTN Flushes, FailFlush, Reentries;
STATIC EFI_BOOT_SERVICES Bs;
STATIC EFI_RUNTIME_SERVICES Rt;
EFI_BOOT_SERVICES *gBS = &Bs;
EFI_RUNTIME_SERVICES *gRT = &Rt;
STATIC EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Fs;
STATIC EFI_PARTITION_INFO_PROTOCOL Part;
STATIC EFI_FILE_PROTOCOL Root, Dir;
STATIC struct {
  EFI_FILE_PROTOCOL File;
  UINT8 Written[512], Durable[512];
  UINTN Size, DurableSize;
} Slots[2];

VOID *EFIAPI AllocateZeroPool(UINTN N) { return calloc(1, N); }
VOID *EFIAPI AllocatePool(UINTN N) { return malloc(N); }
VOID EFIAPI FreePool(VOID *P) { free(P); }
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { return memcpy(D, S, N); }
VOID *EFIAPI ZeroMem(VOID *D, UINTN N) { return memset(D, 0, N); }
INTN EFIAPI CompareMem(CONST VOID *A, CONST VOID *B, UINTN N) { return memcmp(A, B, N); }
BOOLEAN EFIAPI CompareGuid(CONST GUID *A, CONST GUID *B) { return A == B || !memcmp(A, B, sizeof(*A)); }
UINTN EFIAPI StrSize(CONST CHAR16 *N) { UINTN I=0; while (N[I]) ++I; return (I+1)*sizeof(*N); }
INTN EFIAPI StrnCmp(CONST CHAR16 *A, CONST CHAR16 *B, UINTN N) {
  while (N-- && (*A || *B)) { if (*A != *B) return *A - *B; ++A; ++B; } return 0;
}
INTN EFIAPI StrCmp(CONST CHAR16 *A, CONST CHAR16 *B) { return StrnCmp(A, B, MAX_UINTN); }
BOOLEAN EFIAPI EfiAtRuntime(VOID) { return Runtime; }
STATIC EFI_TPL EFIAPI Raise(EFI_TPL New) { assert(!Runtime && New >= Tpl); EFI_TPL Old=Tpl; Tpl=New; return Old; }
STATIC VOID EFIAPI Lower(EFI_TPL New) { assert(!Runtime && New <= Tpl); Tpl=New; }

STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *G, VOID *Registration, VOID **Out) {
  assert(!Runtime && !Registration && Tpl <= TPL_CALLBACK && CompareGuid(G, &mNvFileGuid));
  if (!Installed) return EFI_NOT_FOUND;
  *Out=&mProtocol; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Find(EFI_LOCATE_SEARCH_TYPE Type, EFI_GUID *G, VOID *Key, UINTN *N, EFI_HANDLE **H) {
  assert(Type == ByProtocol && G == &gEfiSimpleFileSystemProtocolGuid && !Key);
  *H=AllocatePool(sizeof(**H)); **H=&Fs; *N=1; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Handle(EFI_HANDLE H, EFI_GUID *G, VOID **Out) {
  assert(H==&Fs);
  *Out = G==&gEfiPartitionInfoProtocolGuid ? (VOID *)&Part : (VOID *)&Fs; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Event(UINT32 Type, EFI_TPL Level, EFI_EVENT_NOTIFY Notify, CONST VOID *Ctx,
                              CONST EFI_GUID *Group, EFI_EVENT *E) {
  assert(Type==EVT_NOTIFY_SIGNAL && Level==TPL_NOTIFY && Notify==Exit && !Ctx && Group==&gEfiEventExitBootServicesGuid);
  *E=(EFI_EVENT)&Fs; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Install(EFI_HANDLE *H, EFI_GUID *G, EFI_INTERFACE_TYPE Type, VOID *P) {
  assert(CompareGuid(G,&mNvFileGuid) && Type==EFI_NATIVE_INTERFACE && P==&mProtocol);
  *H=&Fs; Installed=TRUE; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI InstallCount(EFI_HANDLE *H, ...) { *H=&Bs; return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Get(CHAR16 *Name, EFI_GUID *G, UINT32 *Attr, UINTN *Bytes, VOID *Data) {
  assert(!Runtime && Tpl<=TPL_CALLBACK && !StrCmp(Name,MTC_VARIABLE_NAME) && CompareGuid(G,&gMtcVendorGuid));
  if (!Present) return EFI_NOT_FOUND;
  *Attr=MTC_ATTRIBUTES;
  if (*Bytes < sizeof(Variable)) { *Bytes=sizeof(Variable); return EFI_BUFFER_TOO_SMALL; }
  *Bytes=sizeof(Variable); memcpy(Data,&Variable,sizeof(Variable)); return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Put(CHAR16 *Name, EFI_GUID *G, UINT32 Attr, UINTN Bytes, VOID *Data) {
  assert(!Runtime && Tpl<=TPL_CALLBACK && !StrCmp(Name,MTC_VARIABLE_NAME) && CompareGuid(G,&gMtcVendorGuid));
  assert(Bytes==sizeof(Variable) && Attr==MTC_ATTRIBUTES);
  memcpy(&Variable,Data,Bytes); Present=TRUE; if (Installed) ++mProtocol.Changes; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Enumerate(UINTN *Bytes, CHAR16 *Name, EFI_GUID *G) {
  if (!Present || *Name) return EFI_NOT_FOUND;
  assert(*Bytes>=sizeof(MTC_VARIABLE_NAME)); *Bytes=sizeof(MTC_VARIABLE_NAME);
  memcpy(Name,MTC_VARIABLE_NAME,*Bytes); *G=gMtcVendorGuid; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI OpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *S, EFI_FILE_PROTOCOL **Out) {
  assert(S==&Fs && Tpl<=TPL_CALLBACK && !Runtime); *Out=&Root; return EFI_SUCCESS;
}
STATIC UINTN Index(EFI_FILE_PROTOCOL *F) { assert(F==&Slots[0].File || F==&Slots[1].File); return F!=&Slots[0].File; }
STATIC EFI_STATUS EFIAPI Open(EFI_FILE_PROTOCOL *F, EFI_FILE_PROTOCOL **Out, CHAR16 *Name, UINT64 Mode, UINT64 Attr) {
  assert(F==&Root && Tpl<=TPL_CALLBACK);
  if (Attr&EFI_FILE_DIRECTORY) { *Out=&Dir; return EFI_SUCCESS; }
  UINTN I=StrCmp(Name,mNames[0])!=0; assert(!StrCmp(Name,mNames[I]));
  if (!(Mode&EFI_FILE_MODE_CREATE) && !Slots[I].DurableSize) return EFI_NOT_FOUND;
  *Out=&Slots[I].File; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Close(EFI_FILE_PROTOCOL *F) { (VOID)F; return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Info(EFI_FILE_PROTOCOL *F, EFI_GUID *G, UINTN *Bytes, VOID *Buffer) {
  if (F==&Root) {
    assert(G==&gEfiFileSystemInfoGuid && *Bytes>=SIZE_OF_EFI_FILE_SYSTEM_INFO);
    ((EFI_FILE_SYSTEM_INFO *)Buffer)->ReadOnly=ReadOnly;
  } else {
    assert(G==&gEfiFileInfoGuid && *Bytes>=SIZE_OF_EFI_FILE_INFO);
    ((EFI_FILE_INFO *)Buffer)->Attribute=0; ((EFI_FILE_INFO *)Buffer)->FileSize=Slots[Index(F)].Size;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI SetInfo(EFI_FILE_PROTOCOL *F, EFI_GUID *G, UINTN Bytes, VOID *Buffer) {
  assert(G==&gEfiFileInfoGuid && Bytes>=SIZE_OF_EFI_FILE_INFO);
  Slots[Index(F)].Size=((EFI_FILE_INFO *)Buffer)->FileSize; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Read(EFI_FILE_PROTOCOL *F, UINTN *N, VOID *Buffer) {
  UINTN I=Index(F); assert(*N==Slots[I].DurableSize); memcpy(Buffer,Slots[I].Durable,*N); return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Write(EFI_FILE_PROTOCOL *F, UINTN *N, VOID *Buffer) {
  UINTN I=Index(F); assert(Tpl<=TPL_CALLBACK && !ReadOnly && *N<=sizeof(Slots[I].Written));
  Slots[I].Size=*N; memcpy(Slots[I].Written,Buffer,*N);
  if (mUpdating) {
    UINT64 Nested=123; assert(gBS->GetNextMonotonicCount(&Nested)==EFI_DEVICE_ERROR && Nested==123); ++Reentries;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Flush(EFI_FILE_PROTOCOL *F) {
  UINTN I=Index(F); assert(Tpl<=TPL_CALLBACK);
  if (++Flushes==FailFlush) return EFI_DEVICE_ERROR;
  Slots[I].DurableSize=Slots[I].Size; memcpy(Slots[I].Durable,Slots[I].Written,Slots[I].Size); return EFI_SUCCESS;
}

STATIC VOID ColdBoot(VOID) {
  for (UINTN I=0; I<2; ++I) {
    Slots[I].Size=Slots[I].DurableSize; memcpy(Slots[I].Written,Slots[I].Durable,Slots[I].Size);
  }
  mHigh=mLow=mPending=0; mReady=mCarry=mUpdating=FALSE;
  mProtocol.Frozen=Installed=Runtime=mStarted=mBusy=mExited=Present=FALSE;
  mFs=NULL; mSavedChanges=mSequence=mProtocol.Changes=0; mActive=0; mSaveError=EFI_SUCCESS;
  Flushes=FailFlush=0; Tpl=TPL_APPLICATION;
  assert(MonotonicCounterInitialize(NULL,NULL)==EFI_SUCCESS);
}
int main(void) {
  Bs=(EFI_BOOT_SERVICES){.RaiseTPL=Raise,.RestoreTPL=Lower,.LocateProtocol=Locate,.LocateHandleBuffer=Find,
    .HandleProtocol=Handle,.CreateEventEx=Event,.InstallProtocolInterface=Install,.InstallMultipleProtocolInterfaces=InstallCount};
  Rt=(EFI_RUNTIME_SERVICES){.GetVariable=Get,.SetVariable=Put,.GetNextVariableName=Enumerate};
  Fs.OpenVolume=OpenVolume; Part.Type=PARTITION_TYPE_GPT; memcpy(Part.Info.Gpt.PartitionName,L"persist",sizeof(L"persist"));
  Root=(EFI_FILE_PROTOCOL){.Open=Open,.Close=Close,.GetInfo=Info}; Dir.Close=Close;
  for (UINTN I=0;I<2;++I) Slots[I].File=(EFI_FILE_PROTOCOL){.GetInfo=Info,.SetInfo=SetInfo,.Read=Read,.Write=Write,
                                                             .Flush=Flush,.Close=Close};
  UINT64 Low=123; UINT32 High=456;
  ColdBoot();
  assert(Bs.GetNextMonotonicCount(&Low)==EFI_DEVICE_ERROR && Low==123);
  assert(!PianoNvFileStart() && !Rt.GetNextHighMonotonicCount(&High) && High==1 && Flushes==2);
  assert(!Bs.GetNextMonotonicCount(&Low) && Low==((UINT64)1<<32));
  Tpl=TPL_NOTIFY;
  assert(!Bs.GetNextMonotonicCount(&Low) && Low==(((UINT64)1<<32)|1));
  assert(Rt.GetNextHighMonotonicCount(&High)==EFI_DEVICE_ERROR && High==1 && Flushes==2);
  Tpl=TPL_APPLICATION;
  FailFlush=Flushes+2;
  assert(Rt.GetNextHighMonotonicCount(&High)==EFI_DEVICE_ERROR && High==1);
  assert(!Rt.GetNextHighMonotonicCount(&High) && High==2 && Flushes==6);
  mLow=MAX_UINT32;
  assert(!Bs.GetNextMonotonicCount(&Low) && Low==(((UINT64)2<<32)|MAX_UINT32));
  assert(!Bs.GetNextMonotonicCount(&Low) && Low==((UINT64)3<<32));
  assert(Reentries && !Bs.GetNextMonotonicCount(&Low) && Flushes==8);
  FailFlush=Flushes+2; assert(Rt.GetNextHighMonotonicCount(&High)==EFI_DEVICE_ERROR && High==2);
  Slots[mActive].Durable[0]^=1; // Lose the newest slot after an unreturned allocation.
  ColdBoot(); assert(!PianoNvFileStart() && !Rt.GetNextHighMonotonicCount(&High) && High==4);
  ColdBoot(); assert(!PianoNvFileStart() && !Rt.GetNextHighMonotonicCount(&High) && High==5);
  mLow=MAX_UINT32; mHigh=Variable=MAX_UINT32;
  assert(!Bs.GetNextMonotonicCount(&Low) && Low==MAX_UINT64);
  assert(Bs.GetNextMonotonicCount(&Low)==EFI_DEVICE_ERROR && Low==MAX_UINT64);
  assert(!PianoNvFileSave(TRUE));
  assert(Rt.GetNextHighMonotonicCount(&High)==EFI_DEVICE_ERROR && High==5);
  Exit(NULL,NULL); Runtime=TRUE; assert(Rt.GetNextHighMonotonicCount(&High)==EFI_UNSUPPORTED && High==5);
  ColdBoot(); ReadOnly=TRUE;
  assert(PianoNvFileStart()==EFI_WRITE_PROTECTED && PianoNvFileSave(TRUE)==EFI_NOT_STARTED);
  assert(Bs.GetNextMonotonicCount(&Low)==EFI_DEVICE_ERROR && !Flushes);
  ReadOnly=FALSE; Slots[0].Durable[0]^=1; Slots[1].Durable[0]^=1;
  ColdBoot(); assert(PianoNvFileStart()==EFI_VOLUME_CORRUPTED && PianoNvFileSave(TRUE)==EFI_NOT_STARTED);
  assert(Rt.GetNextHighMonotonicCount(&High)==EFI_DEVICE_ERROR && !Flushes);
}
