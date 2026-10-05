#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../bootprofiles/early-memory/PianoSmemDescriptor.h"
VOID *EFIAPI CopyMem(VOID*A,CONST VOID*B,UINTN N){return memcpy(A,B,N);}VOID *EFIAPI ZeroMem(VOID*P,UINTN N){return memset(P,0,N);}INTN EFIAPI CompareMem(CONST VOID*A,CONST VOID*B,UINTN N){return memcmp(A,B,N);}
static UINT8 Memory[PIANO_SMEM_BYTES];static UINTN calls,reads;static UINT32 changed;static UINT64 failaddress;
static PIANO_SMEM_DESCRIPTOR_WORK Work;static PIANO_SMEM_DESCRIPTOR_REPORT Report;
static VOID p16(UINT8*P,UINT16 V){memcpy(P,&V,2);}static VOID p32(UINT8*P,UINT32 V){memcpy(P,&V,4);}static VOID p64(UINT8*P,UINT64 V){memcpy(P,&V,8);}
static EFI_STATUS EFIAPI Read(VOID*C,UINT64 A,UINTN N,VOID*Out){(VOID)C;++calls;assert(A>=PIANO_SMEM_BASE&&A+N<=PIANO_SMEM_BASE+PIANO_SMEM_BYTES&&N<=256&&(N&3)==0);
 if(A==failaddress)return EFI_NOT_READY;
 if(A==0x81eff350&&++reads==2&&changed)Memory[0x1ff358]^=1;
 memcpy(Out,Memory+(A-PIANO_SMEM_BASE),N);return EFI_SUCCESS;}
static VOID setup(VOID){memset(Memory,0,sizeof(Memory));memset(&Work,0,sizeof(Work));reads=calls=changed=0;failaddress=0;
 UINT8*D=Memory+0x1ff350;p32(D,0x49494953);p32(D+4,PIANO_SMEM_BYTES);p64(D+8,PIANO_SMEM_BASE);p16(D+16,512);p16(D+18,1);p16(D+20,0x4853);p16(D+22,12);p64(D+24,0x1122334455667788);}
int main(VOID){PIANO_SMEM_READER R={NULL,Read,2048,262144};UINTN cases=0;
 setup();assert(PianoSmemDescriptorCollect(&R,0x81eff350,&Work,&Report)==EFI_SUCCESS&&Report.RepeatedEqual&&Report.RegionMatchesKnownWindow&&Report.SnapshotBytes==32&&Report.ItemCount==512&&Report.TlvCount==1&&Report.HostInfoBytes==12);++cases;
 setup();assert(PianoSmemDescriptorCollect(&R,0x90000000,&Work,&Report)==EFI_ACCESS_DENIED&&!calls);++cases;
 setup();assert(PianoSmemDescriptorCollect(&R,0x81eff351,&Work,&Report)==EFI_ACCESS_DENIED&&!calls);++cases;
 setup();p32(Memory+0x1ff350,0);assert(PianoSmemDescriptorCollect(&R,0x81eff350,&Work,&Report)==EFI_COMPROMISED_DATA&&!Work.Busy);++cases;
 setup();p16(Memory+0x1ff350+18,65);assert(PianoSmemDescriptorCollect(&R,0x81eff350,&Work,&Report)==EFI_BAD_BUFFER_SIZE);++cases;
 setup();p16(Memory+0x1ff350+22,2);assert(PianoSmemDescriptorCollect(&R,0x81eff350,&Work,&Report)==EFI_COMPROMISED_DATA);++cases;
 setup();p16(Memory+0x1ff350+22,2048);assert(PianoSmemDescriptorCollect(&R,0x81eff350,&Work,&Report)==EFI_COMPROMISED_DATA);++cases;
 setup();changed=1;assert(PianoSmemDescriptorCollect(&R,0x81eff350,&Work,&Report)==EFI_MEDIA_CHANGED&&!Report.RepeatedEqual);++cases;
 setup();failaddress=0x81eff350+20;assert(PianoSmemDescriptorCollect(&R,0x81eff350,&Work,&Report)==EFI_NOT_READY&&!Report.RepeatedEqual);++cases;
 setup();p16(Memory+0x1ff350+18,2);p16(Memory+0x1ff350+32,0x4853);p16(Memory+0x1ff350+34,4);
 assert(PianoSmemDescriptorCollect(&R,0x81eff350,&Work,&Report)==EFI_COMPROMISED_DATA&&Report.RepeatedEqual);++cases;
 setup();Work.Busy=TRUE;assert(PianoSmemDescriptorCollect(&R,0x81eff350,&Work,&Report)==EFI_ALREADY_STARTED&&!calls);++cases;
 setup();assert(PianoSmemDescriptorCollect(&R,0x81eff350,&Work,(VOID*)&Work)==EFI_INVALID_PARAMETER&&!calls);++cases;
 printf("Actual SIII fixed-window collector: %lu bounds/version/TLV/duplicate/coherence/alias/read-fault cases, no external pointer permission or DMA/memory ownership\n",(unsigned long)cases);return 0;}
