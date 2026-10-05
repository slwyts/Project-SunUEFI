// Actual reader/blob with real incremental SHA; SFS service boundary fixture.
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#undef NULL
#include "../bootprofiles/os-boot/PianoBootFileSource.h"
#include <Guid/FileInfo.h>
#include <Guid/EventGroup.h>
EFI_GUID gEfiSimpleFileSystemProtocolGuid={0},gEfiFileInfoGuid={0},gEfiEventExitBootServicesGuid=EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
UINTN EFIAPI Sha256GetContextSize(void){return sizeof(SHA256_CTX);}BOOLEAN EFIAPI Sha256Init(VOID *C){return SHA256_Init(C)==1;}BOOLEAN EFIAPI Sha256Update(VOID *C,CONST VOID *P,UINTN N){return SHA256_Update(C,P,N)==1;}BOOLEAN EFIAPI Sha256Final(VOID *C,UINT8 *D){return SHA256_Final(D,C)==1;}
typedef struct {EFI_EVENT_NOTIFY Notify;VOID *Context;} EVENT;
static EFI_BOOT_SERVICES Bs;static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Sfs,Other;static EFI_FILE_PROTOCOL Root,File;
static UINT8 SmallContent[131073],*Content=SmallContent,Expected[32];static UINTN ContentBytes=sizeof(SmallContent);static int SourceFd=-1;static CONST char *LargePath;static UINT64 Position;static UINTN Case,Reads,Seeks,Opens,Roots,FileCloses,RootCloses,Allocations,Frees,Closes,Calls,Infos;static EFI_TPL Tpl;static BOOLEAN Services=TRUE;static EVENT *Fence;static CONST CHAR16 *Leaf;
static VOID *Pools[4];static UINTN PoolSizes[4];
static UINTN Slices,MemoryChecks,MemoryValidations,BufferValidations;
static EFI_STATUS CpuSlice(VOID *C,UINTN Budget){(void)C;assert(Budget==1000&&Services);++Slices;return Case==35?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
static EFI_STATUS CpuMemory(VOID *C,PIANO_LINUX_MEMORY_PROOF *P){(void)C;++MemoryChecks;
 *P=(PIANO_LINUX_MEMORY_PROOF){.Revision=1,.Status=EFI_SUCCESS,.BootEpoch=1,.DramBytes=16ULL*1024*1024*1024,
 .NormalBytes=4ULL*1024*1024*1024,.FullDdr=Case!=32,.FixedReservations=TRUE,.DynamicReservations=TRUE,
 .RuntimeRegions=TRUE,.CacheVerified=TRUE,.OwnershipVerified=TRUE};return EFI_SUCCESS;}
static EFI_STATUS CpuValidate(VOID *C,CONST PIANO_LINUX_MEMORY_PROOF *P){(void)C;++MemoryValidations;assert(P->BootEpoch==1);return Case==33?EFI_NOT_READY:EFI_SUCCESS;}
static EFI_STATUS CpuBuffer(VOID *C,CONST PIANO_LINUX_MEMORY_PROOF *P,CONST VOID *Producer,VOID *Owner,CONST VOID *Base,UINT64 Bytes){
 (void)C;assert(P->BootEpoch==1&&Producer&&Base&&Bytes);++BufferValidations;
 CONST PIANO_BOOT_FILE_SOURCE *S=Producer;assert(S->Data==Base&&S->Bytes==Bytes&&Owner==S->Owner);
 return Case==34?EFI_ACCESS_DENIED:EFI_SUCCESS;}
static VOID Check(void){assert(Services);Calls++;}
static BOOLEAN Live(VOID *C){(void)C;return Services;}
static EFI_TPL EFIAPI Raise(EFI_TPL New){Check();EFI_TPL Old=Tpl;Tpl=New;return Old;}static VOID EFIAPI Restore(EFI_TPL Old){Check();Tpl=Old;}
static EFI_STATUS EFIAPI Create(UINT32 Type,EFI_TPL T,EFI_EVENT_NOTIFY N,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *Out){Check();assert(Type==EVT_NOTIFY_SIGNAL&&T==TPL_NOTIFY&&G==&gEfiEventExitBootServicesGuid);Fence=malloc(sizeof(*Fence));*Fence=(EVENT){N,(VOID *)C};*Out=Fence;return EFI_SUCCESS;}
static VOID Exit(void){Fence->Notify(Fence,Fence->Context);Services=FALSE;}
static EFI_STATUS EFIAPI CloseEvent(EFI_EVENT E){Check();Closes++;if(Case==16)return EFI_WARN_STALE_DATA;free(E);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE H,EFI_GUID *G,VOID **Out){Check();assert(H==(VOID *)7&&G==&gEfiSimpleFileSystemProtocolGuid);*Out=Case==18&&Reads?&Other:&Sfs;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Allocate(EFI_MEMORY_TYPE T,UINTN N,VOID **Out){Check();assert(T==EfiLoaderData&&N==ContentBytes);assert(Allocations<4);*Out=malloc(N);Pools[Allocations]=*Out;PoolSizes[Allocations++]=N;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Free(VOID *P){Check();Frees++;if(Case==15)return EFI_WARN_STALE_DATA;BOOLEAN Found=FALSE;for(UINTN I=0;I<Allocations;I++)if(Pools[I]==P){Found=TRUE;for(UINTN Q=0;Q<PoolSizes[I];Q++)assert(((UINT8 *)P)[Q]==0);}assert(Found);free(P);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI CloseFile(EFI_FILE_PROTOCOL *P){Check();assert(P==&File);FileCloses++;return Case==13?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
static EFI_STATUS EFIAPI CloseRoot(EFI_FILE_PROTOCOL *P){Check();assert(P==&Root);RootCloses++;return Case==14?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI OpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *P,EFI_FILE_PROTOCOL **Out){Check();assert(P==&Sfs);Roots++;*Out=&Root;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Open(EFI_FILE_PROTOCOL *P,EFI_FILE_PROTOCOL **Out,CHAR16 *Path,UINT64 Mode,UINT64 Attributes){Check();assert(P==&Root&&Mode==EFI_FILE_MODE_READ&&!Attributes&&Path[0]=='\\');Opens++;Leaf=Path;for(UINTN I=0;Path[I];I++)if(Path[I]=='\\')Leaf=Path+I+1;Position=0;*Out=&File;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Seek(EFI_FILE_PROTOCOL *P,UINT64 Offset){Check();assert(P==&File);Seeks++;Position=Offset;return Case==5?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
static EFI_STATUS EFIAPI GetPosition(EFI_FILE_PROTOCOL *P,UINT64 *Out){Check();assert(P==&File);*Out=Case==6?Position+1:Position;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Read(EFI_FILE_PROTOCOL *P,UINTN *N,VOID *Buffer){
  Check();assert(P==&File);Reads++;
  if(Case==17){Exit();return EFI_SUCCESS;}
  UINTN Bytes=MIN(*N,ContentBytes-(UINTN)Position);if(Case==3&&Bytes)Bytes--;
  if(Case==12&&Position==ContentBytes){Bytes=1;((UINT8 *)Buffer)[0]=0;*N=Bytes;return EFI_SUCCESS;}
  if(SourceFd>=0){assert(pread(SourceFd,Buffer,Bytes,(off_t)Position)==(ssize_t)Bytes);}else memcpy(Buffer,Content+(UINTN)Position,Bytes);Position+=Bytes;*N=Bytes;return Case==4?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Info(EFI_FILE_PROTOCOL *P,EFI_GUID *G,UINTN *N,VOID *Buffer){
  Check();assert(P==&File&&G==&gEfiFileInfoGuid);UINTN Chars=0;while(Leaf[Chars])Chars++;UINTN Needed=SIZE_OF_EFI_FILE_INFO+(Chars+1)*2;
  if(!Buffer){*N=Case==10?8192:Needed;return Case==11?EFI_WARN_STALE_DATA:EFI_BUFFER_TOO_SMALL;}
  assert(*N>=Needed);memset(Buffer,0,*N);EFI_FILE_INFO *F=Buffer;F->Size=Needed;F->FileSize=Case==1?0:Case==7&&Reads?ContentBytes+1:ContentBytes;F->PhysicalSize=(ContentBytes+4095)&~4095ULL;F->Attribute=Case==2?EFI_FILE_DIRECTORY:0;
  memcpy(F->FileName,Leaf,(Chars+1)*2);if(Case==8)F->FileName[0]='X';F->CreateTime.Year=2026;F->ModificationTime.Year=Case==9&&Reads?2027:2026;F->LastAccessTime.Year=2026+Reads;
  *N=Needed;Infos++;return EFI_SUCCESS;
}
static VOID Setup(void){
  if(SourceFd>=0){SHA256_CTX ctx;assert(SHA256_Init(&ctx)==1);UINT8 block[65536];for(UINTN at=0;at<ContentBytes;){UINTN n=MIN(sizeof(block),ContentBytes-at);assert(pread(SourceFd,block,n,(off_t)at)==(ssize_t)n);assert(SHA256_Update(&ctx,block,n)==1);at+=n;}assert(SHA256_Final(Expected,&ctx)==1);}
  else {for(UINTN I=0;I<ContentBytes;I++)Content[I]=(UINT8)(I*11);SHA256(Content,ContentBytes,Expected);}Tpl=TPL_APPLICATION;
  Bs.Hdr.Signature=EFI_BOOT_SERVICES_SIGNATURE;Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;Bs.HandleProtocol=Handle;Bs.AllocatePool=Allocate;Bs.FreePool=Free;Bs.CreateEventEx=Create;Bs.CloseEvent=CloseEvent;
  Sfs.Revision=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;Sfs.OpenVolume=OpenVolume;Root.Revision=File.Revision=EFI_FILE_PROTOCOL_REVISION;Root.Open=Open;Root.Close=CloseRoot;
  File.Close=CloseFile;File.SetPosition=Seek;File.GetPosition=GetPosition;File.GetInfo=Info;File.Read=Read;
}
static VOID Run(UINTN C){Case=C;
  if(C==29){ContentBytes=PIANO_CPU_INPUT_LOW_BYTES+65537;Content=malloc(ContentBytes);assert(Content);}
  if(C==30){struct stat st;SourceFd=open(LargePath,O_RDONLY);assert(SourceFd>=0&&fstat(SourceFd,&st)==0&&st.st_size>0&&(UINT64)st.st_size>PIANO_CPU_INPUT_LOW_BYTES&&(UINT64)st.st_size<=PIANO_CPU_INPUT_MAX_BYTES);ContentBytes=(UINTN)st.st_size;}
  Setup();PIANO_BOOT_FILE_ENV Env={.Services=&Bs,.BootServicesAlive=Live};PIANO_BOOT_FILE_SPEC Spec={(VOID *)7,L"\\EFI\\Piano\\kernel.efi",200000,Expected};PIANO_BOOT_FILE_SOURCE State={0};PIANO_BOOT_SOURCE Reader;PIANO_LAUNCH_BLOB Blob;
  if(C>=29){
    PIANO_CPU_INPUT_ENV CpuEnv={.BootServicesAlive=Live,.ServiceSlice=CpuSlice,.CheckMemory=CpuMemory,.ValidateMemory=CpuValidate,.ValidateBuffer=CpuBuffer};
    Env.Cpu=C==31?NULL:&CpuEnv;Spec.MaxBytes=PIANO_CPU_INPUT_MAX_BYTES;
    EFI_STATUS Status=PianoBootFileLoad(&State,&Env,&Spec,&Reader,&Blob);
    if(C==31||C==32||C==33){assert(Status==EFI_NOT_READY&&!Reads&&!Opens&&!Allocations);return;}
    if(C==34){assert(Status==EFI_ACCESS_DENIED&&State.Retained&&!Reads&&Allocations==1&&!Frees);return;}
    if(C==35){assert(Status==EFI_DEVICE_ERROR&&State.Retained&&!Reads&&Allocations==1&&!Frees);return;}
    assert(Status==EFI_SUCCESS&&State.Bytes==ContentBytes&&State.Chunks==(ContentBytes+65535)/65536&&Slices>=State.Chunks&&BufferValidations>=2);
    assert(!memcmp(State.Sha256,Expected,32)&&MemoryChecks==1);
    VOID *Owner=NULL,*Loan=NULL;CONST VOID *View=NULL;assert(Blob.Take(Blob.Context,&Owner)==EFI_SUCCESS);
    assert(Blob.BorrowView(Blob.Context,Owner,(PIANO_BOOT_RANGE){0,ContentBytes},&View,&Loan)==EFI_SUCCESS&&View==State.Data);
    UINT8 Tail[17];UINTN OldReads=Reads;assert(Blob.Read(Blob.Context,Owner,ContentBytes-17,17,Tail)==EFI_SUCCESS&&Reads==OldReads);
    if(SourceFd>=0){UINT8 CheckTail[17];assert(pread(SourceFd,CheckTail,17,(off_t)(ContentBytes-17))==17&&!memcmp(Tail,CheckTail,17));}
    else assert(!memcmp(Tail,Content+ContentBytes-17,17));
    assert(Blob.Unborrow(Blob.Context,Owner,Loan)==EFI_SUCCESS);
    assert(Blob.ZeroRelease(Blob.Context,Owner)==EFI_SUCCESS&&Frees==1&&Slices>2*State.Chunks&&BufferValidations>=4);
    if(SourceFd>=0)close(SourceFd);else free(Content);
    printf("Actual large BootFileSource: %llu bytes, %llu 64KiB chunks, %llu service slices, full SHA/owner/readback/zero-release passed; host SFS, no device\n",(unsigned long long)ContentBytes,(unsigned long long)State.Chunks,(unsigned long long)Slices);return;
  }
  if(C>=22&&C<=24){
    PIANO_BOOT_FILE_SOURCE States[3]={{0}};PIANO_BOOT_SOURCE Readers[3];PIANO_LAUNCH_BLOB Blobs[3];UINT8 Bad[32];memcpy(Bad,Expected,32);Bad[0]^=1;
    PIANO_BOOT_FILE_SPEC Specs[3]={Spec,{(VOID *)7,L"\\EFI\\Piano\\board.dtb",200000,Expected},{(VOID *)7,L"\\EFI\\Piano\\initrd.img",200000,Expected}};
    if(C==23)Specs[1].ExpectedSha256=Bad;
    EFI_STATUS S=PianoBootFileLoadBundle(States,3,&Env,Specs,C==24?500000:600000,Readers,Blobs);
    if(C==22){assert(S==EFI_SUCCESS&&Allocations==3&&FileCloses==3&&RootCloses==3);UINTN Before=Reads;
      for(UINTN I=0;I<3;I++){VOID *Owner=NULL,*Loan=NULL;CONST VOID *View=NULL;assert(Blobs[I].Take(Blobs[I].Context,&Owner)==EFI_SUCCESS);assert(Blobs[I].BorrowView(Blobs[I].Context,Owner,(PIANO_BOOT_RANGE){0,ContentBytes},&View,&Loan)==EFI_SUCCESS&&View==States[I].Data);assert(Blobs[I].Unborrow(Blobs[I].Context,Owner,Loan)==EFI_SUCCESS&&Blobs[I].ZeroRelease(Blobs[I].Context,Owner)==EFI_SUCCESS);}assert(Reads==Before&&Frees==3);}
    else {assert(S!=EFI_SUCCESS&&!Readers[0].Read&&!Blobs[0].Take);if(C==24)assert(!Opens&&!Allocations);else assert(States[0].Consumed&&States[1].Consumed&&!States[2].Signature);}
    return;
  }
  if(C==19)Spec.AbsolutePath=L"\\EFI\\Piano\\..\\kernel.efi";
  if(C==20)Spec.MaxBytes=131072;
  if(C==21){Expected[0]^=1;}
  EFI_STATUS S=PianoBootFileLoad(&State,&Env,&Spec,&Reader,&Blob);
  if(C>=25){
    assert(S==EFI_SUCCESS);PIANO_BOOT_FILE_SOURCE Saved=State;UINT8 Hash[32];SHA256(State.Data,State.Bytes,Hash);UINTN Before=Reads;
    if(C==25){
      assert(Reader.Read(Reader.Context,0,16,State.Data+1)==EFI_INVALID_PARAMETER);
      assert(Reader.Read(Reader.Context,0,16,(UINT8 *)&State+1)==EFI_INVALID_PARAMETER);
      assert(Reader.Read(Reader.Context,0,16,(VOID *)(MAX_UINTN-4))==EFI_INVALID_PARAMETER);
    }else if(C==26){
      assert(PianoBootFileExport(&State,(PIANO_BOOT_SOURCE *)&State,&Blob)==EFI_INVALID_PARAMETER);
      assert(PianoBootFileExport(&State,&Reader,(PIANO_LAUNCH_BLOB *)State.Data)==EFI_INVALID_PARAMETER);
      assert(PianoBootFileExport(&State,(PIANO_BOOT_SOURCE *)&Blob,&Blob)==EFI_INVALID_PARAMETER);
    }else if(C==27){
      assert(Blob.Take(Blob.Context,&State.Owner)==EFI_INVALID_PARAMETER);
      assert(Blob.Take(Blob.Context,(VOID **)State.Data)==EFI_INVALID_PARAMETER);
      VOID *Owner=NULL,*Loan=NULL;CONST VOID *View=NULL;assert(Blob.Take(Blob.Context,&Owner)==EFI_SUCCESS);
      assert(Blob.BorrowView(Blob.Context,Owner,(PIANO_BOOT_RANGE){0,16},(CONST VOID **)&State.Loan,&Loan)==EFI_INVALID_PARAMETER);
      assert(Blob.BorrowView(Blob.Context,Owner,(PIANO_BOOT_RANGE){0,16},&View,(VOID **)State.Data)==EFI_INVALID_PARAMETER);
      assert(Blob.BorrowView(Blob.Context,Owner,(PIANO_BOOT_RANGE){0,16},&View,(VOID **)&View)==EFI_INVALID_PARAMETER);
      assert(Blob.Read(Blob.Context,Owner,0,16,State.Data)==EFI_INVALID_PARAMETER);
      assert(Blob.Read(Blob.Context,Owner,0,16,&State)==EFI_INVALID_PARAMETER);
      assert(Blob.Restore(Blob.Context,Owner)==EFI_SUCCESS);Saved=State;
    }else if(C==28){
      PIANO_BOOT_FILE_SOURCE Empty={0};assert(PianoBootFileLoad(&Empty,&Env,&Spec,(PIANO_BOOT_SOURCE *)&Empty,&Blob)==EFI_INVALID_PARAMETER);
      assert(!Empty.Signature&&!Empty.Data);PIANO_BOOT_FILE_SOURCE States[3]={{0}};PIANO_BOOT_FILE_SPEC Specs[3]={Spec,Spec,Spec};PIANO_LAUNCH_BLOB Blobs[3];
      assert(PianoBootFileLoadBundle(States,3,&Env,Specs,600000,(PIANO_BOOT_SOURCE *)States,Blobs)==EFI_INVALID_PARAMETER);
      assert(!States[0].Signature&&!States[1].Signature&&!States[2].Signature);
    }
    UINT8 After[32];SHA256(State.Data,State.Bytes,After);assert(!memcmp(Hash,After,32)&&!memcmp(&Saved,&State,sizeof(State))&&Reads==Before);
    assert(PianoBootFileDispose(&State)==EFI_SUCCESS);return;
  }
  if(C==15||C==16){assert(S==EFI_SUCCESS);assert(PianoBootFileDispose(&State)!=EFI_SUCCESS&&State.Retained);UINTN Before=Calls;assert(PianoBootFileDispose(&State)!=EFI_SUCCESS&&Calls==Before);return;}
  if(C==0){
    assert(S==EFI_SUCCESS&&State.Ready&&!State.File&&!State.Root&&FileCloses==1&&RootCloses==1&&Allocations==1&&State.Chunks==3);UINTN Before=Reads;
    UINT8 Bytes[17];assert(Reader.Read(Reader.Context,65530,sizeof(Bytes),Bytes)==EFI_SUCCESS&&!memcmp(Bytes,Content+65530,sizeof(Bytes))&&Reads==Before);
    VOID *Owner=NULL,*Loan=NULL;CONST VOID *View=NULL;assert(Blob.Take(Blob.Context,&Owner)==EFI_SUCCESS);assert(Blob.BorrowView(Blob.Context,Owner,(PIANO_BOOT_RANGE){0,State.Bytes},&View,&Loan)==EFI_SUCCESS&&View==State.Data);
    assert(Blob.ZeroRelease(Blob.Context,Owner)==EFI_ACCESS_DENIED&&Blob.Restore(Blob.Context,Owner)==EFI_ACCESS_DENIED);
    assert(Blob.Unborrow(Blob.Context,Owner,(VOID *)999)==EFI_INVALID_PARAMETER);assert(Blob.Unborrow(Blob.Context,Owner,Loan)==EFI_SUCCESS);assert(Blob.Restore(Blob.Context,Owner)==EFI_SUCCESS);
    VOID *Second=NULL;assert(Blob.Take(Blob.Context,&Second)==EFI_SUCCESS&&Second!=Owner);assert(Blob.Read(Blob.Context,Owner,0,1,Bytes)==EFI_ACCESS_DENIED);
    assert(Blob.ZeroRelease(Blob.Context,Second)==EFI_SUCCESS&&Frees==1&&Closes==1&&Reads==Before);return;
  }
  assert(S!=EFI_SUCCESS&&!Reader.Read&&!Blob.Take);
  if(C==13||C==14||C==17||C==18){assert(State.Retained);UINTN Before=Calls;assert(PianoBootFileDispose(&State)!=EFI_SUCCESS&&Calls==Before);}
  else if(C==19)assert(!Roots&&!Opens&&!Allocations);
  else assert(FileCloses==1&&RootCloses==1&&State.Consumed);
}
int main(int argc,char **argv){if(argc==2){LargePath=argv[1];Run(30);return 0;}assert(argc==1);
 for(UINTN C=0;C<36;C++){if(C==30)continue;pid_t P=fork();assert(P>=0);if(!P){Run(C);_exit(0);}int S;waitpid(P,&S,0);if(!WIFEXITED(S)||WEXITSTATUS(S)){fprintf(stderr,"bootfile case %llu failed\n",(unsigned long long)C);return 1;}}
 puts("Actual readonly BootFileSource:35 SFS/large-memory/owner/slice/exactIO/SHA/metadata/close/warning/EBS/bundle/alias/zero-release cases passed; no boot/device");return 0;}
