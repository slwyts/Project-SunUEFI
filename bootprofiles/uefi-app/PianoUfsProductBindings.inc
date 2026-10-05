// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included in the actual shared UFS translation unit; no additional DMA owner.
#pragma pack(1)
STATIC struct {VENDOR_DEVICE_PATH Vendor;UINT8 VolumeUuid[16];EFI_DEVICE_PATH_PROTOCOL End;} mProductPath;
#pragma pack()
STATIC BOOLEAN ProductSameContext(CONST PIANO_SMMU_DEVICE *A,CONST PIANO_SMMU_DEVICE *B){
  return A->Present==B->Present && A->Enabled==B->Enabled && A->Sid==B->Sid && A->Mask==B->Mask &&
    A->StreamIndex==B->StreamIndex && A->Type==B->Type && A->ContextBank==B->ContextBank && A->Smr==B->Smr && A->S2cr==B->S2cr &&
    A->Sctlr==B->Sctlr && A->Cbar==B->Cbar && A->Cba2r==B->Cba2r && A->Tcr==B->Tcr && A->Tcr2==B->Tcr2 &&
    A->Mair0==B->Mair0 && A->Mair1==B->Mair1 && A->Ttbr0==B->Ttbr0 && A->Ttbr1==B->Ttbr1 && A->Fsr==B->Fsr && A->Far==B->Far && A->Fsynr==B->Fsynr;
}
STATIC EFI_STATUS ProductRetain(EFI_STATUS S){
  mProductRetained=TRUE;mContext.TableMemory.Quarantined=TRUE;
  if(mProductVolume){mProductVolume->State.Quarantined=mProductVolume->State.NeedsRecovery=TRUE;mProductVolume->Media.ReadOnly=TRUE;mProductVolume->State.LastStatus=S==EFI_SUCCESS?EFI_DEVICE_ERROR:S;if(mProductVolume->State.FirstFailure==EFI_SUCCESS)mProductVolume->State.FirstFailure=S==EFI_SUCCESS?EFI_DEVICE_ERROR:S;}
  return S==EFI_SUCCESS?EFI_DEVICE_ERROR:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
}
STATIC EFI_STATUS ProductSmmuGuard(BOOLEAN Establish){
  if(!mProductExecuting || !mBlockBusy || !mInstalled || mExitRetained || mProductRetained || !mContext.Attached || !mContext.Verified ||
     !mContext.Domain || !mContext.TableMemory.Signature || mContext.TableMemory.Quarantined)return EFI_ACCESS_DENIED;
  if(Read(0x50)!=(UINT32)mTrl.DeviceAddress || Read(0x54)!=(UINT32)(mTrl.DeviceAddress>>32) || Read(0x58) || Read(0x78))return ProductRetain(EFI_COMPROMISED_DATA);
  PIANO_SMMU_SNAPSHOT Current;EFI_STATUS S=PianoSmmuCapture(mContext.Fdt,"product-volume-lease",&Current);
  if(S!=EFI_SUCCESS)return ProductRetain(S);
  if(!Current.Valid || Current.Base!=0x15000000 || Current.Window!=0x100000 || !Current.Groups || Current.Groups>256 ||
     Current.GlobalFault || !Current.Device[0].Present || !Current.Device[0].Enabled || Current.Device[0].Fsr)return ProductRetain(EFI_COMPROMISED_DATA);
  PIANO_SMMU_DEVICE Expected=mContext.AttachedSnapshot.Device[0];Expected.Fsr=0;
  if(!ProductSameContext(&Expected,&Current.Device[0]) || (Current.Device[0].Ttbr0&0x0000FFFFFFFFF000ULL)!=mContext.TableMemory.Physical)return ProductRetain(EFI_COMPROMISED_DATA);
  PIANO_DMA_BUFFER *Buffers[]={&mUcd,&mTrl,&mData};CONST UINTN Sizes[]={PIANO_UFS_UCD_BYTES,1024,4096};
  for(UINTN I=0;I<3;++I){PIANO_DMA_BUFFER *B=Buffers[I];UINT64 First=0,Last=0;
    if(!B->Signature || !B->Mapped || B->Active || B->Quarantined || B->ExitRetained || B->Device!=&mDevice || !B->Cpu || B->Bytes!=Sizes[I] || B->ReservedBytes!=4096 || B->MemoryType!=EfiReservedMemoryType || !(B->MemoryAttributes&EFI_MEMORY_WB) ||
       B->Direction!=PianoDmaBidirectional || B->Alignment<4096 || ((B->Physical|B->DeviceAddress)&4095) || B->DeviceAddress>MAX_UINT32-B->Bytes+1 ||
       PianoIoPageTableTranslate(&mContext.PageTable,B->DeviceAddress,TRUE,&First)!=EFI_SUCCESS ||
       PianoIoPageTableTranslate(&mContext.PageTable,B->DeviceAddress+B->Bytes-1,TRUE,&Last)!=EFI_SUCCESS || First!=B->Physical || Last!=B->Physical+B->Bytes-1)return ProductRetain(EFI_COMPROMISED_DATA);
  }
  if(Establish){mProductLeaseBasis=Current;return EFI_SUCCESS;}
  CONST PIANO_SMMU_SNAPSHOT *B=&mProductLeaseBasis;
  if(Current.Base!=B->Base || Current.Window!=B->Window || Current.ContextBase!=B->ContextBase || Current.PageShift!=B->PageShift ||
     Current.Groups!=B->Groups || Current.Banks!=B->Banks || Current.ExtendedIds!=B->ExtendedIds || Current.Id0!=B->Id0 ||
     Current.Id1!=B->Id1 || Current.Id2!=B->Id2 || Current.GlobalControl!=B->GlobalControl || Current.GlobalFault!=B->GlobalFault)return ProductRetain(EFI_COMPROMISED_DATA);
  for(UINTN I=0;I<Current.Groups;++I)if(Current.RawSmr[I]!=B->RawSmr[I] || Current.RawS2cr[I]!=B->RawS2cr[I])return ProductRetain(EFI_COMPROMISED_DATA);
  for(UINTN I=0;I<PIANO_SMMU_DEVICE_COUNT;++I)if(!ProductSameContext(&Current.Device[I],&B->Device[I]))return ProductRetain(EFI_COMPROMISED_DATA);
  return EFI_SUCCESS;
}
STATIC BOOLEAN ProductContext(VOID *Context){return mProductExecuting && mBlockBusy && Context==mProductVolume && mProductVolume && !mExitRetained && !mProductRetained;}
STATIC EFI_STATUS ProductQuiet(VOID *Context,BOOLEAN *Quiet){
  if(!Quiet)return EFI_INVALID_PARAMETER;*Quiet=FALSE;if(!ProductContext(Context))return EFI_ACCESS_DENIED;
  EFI_STATUS S=Quiesce();if(S!=EFI_SUCCESS)return ProductRetain(S);Write(0x24,0);if(Read(0x24)!=0 || !QueuesStopped())return ProductRetain(EFI_DEVICE_ERROR);
  PIANO_DMA_BUFFER *Buffers[]={&mUcd,&mTrl,&mData};
  for(UINTN I=0;I<3;++I){PIANO_DMA_BUFFER *B=Buffers[I];if(B->Active){S=PianoDmaComplete(B,mProductTransportStatus,TRUE);if(S!=EFI_SUCCESS)return ProductRetain(S);}if(B->Active || B->Quarantined || B->ExitRetained)return ProductRetain(EFI_ACCESS_DENIED);}
  S=ProductSmmuGuard(FALSE);if(S!=EFI_SUCCESS)return S;*Quiet=TRUE;return EFI_SUCCESS;
}
STATIC EFI_STATUS ProductAcquire(VOID *Context){
  if(Context!=mProductVolume || !mProductVolume || mProductExecuting || mBlockBusy || mExitRetained || mProductRetained ||
     !mInstalled || !mBlockLive || mResetReport.Prepared || mResetReport.Failed)return EFI_NOT_READY;
  EFI_TPL Tpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Tpl);
  PIANO_UFS_PRODUCT_IO_SCOPE Scope=mProductVolume->State.Scope;
  if((Scope==PianoProductIoFat && Tpl>TPL_CALLBACK) || (Scope==PianoProductIoNv && Tpl>TPL_NOTIFY) ||
     (Scope!=PianoProductIoFat && Scope!=PianoProductIoNv && Tpl>TPL_CALLBACK))return EFI_UNSUPPORTED;
  mProductOldTpl=Tpl;mProductRaisedTpl=Tpl<TPL_CALLBACK;
  if(mProductRaisedTpl)gBS->RaiseTPL(TPL_CALLBACK);
  mProductExecuting=mBlockBusy=TRUE;
  EFI_STATUS S=ProductSmmuGuard(TRUE);if(S==EFI_SUCCESS){BOOLEAN Quiet=FALSE;S=ProductQuiet(Context,&Quiet);if(S==EFI_SUCCESS && !Quiet)S=EFI_NOT_READY;}
  if(S!=EFI_SUCCESS)return ProductRetain(S);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS ProductRelease(VOID *Context,BOOLEAN Quarantined){
  if(Context!=mProductVolume || !mProductExecuting || !mBlockBusy)return EFI_ACCESS_DENIED;
  if(Quarantined)return ProductRetain(EFI_ACCESS_DENIED);
  BOOLEAN Quiet=FALSE;EFI_STATUS S=ProductQuiet(Context,&Quiet);if(S!=EFI_SUCCESS || !Quiet)return ProductRetain(S);
  DEBUG((DEBUG_WARN,"PIANO_PRODUCT_UFS_IO_COMPLETE scope=%u write_doorbells=%lu sync_doorbells=%lu verified=%lu old_tpl=%lu raised=%u\n",mProductVolume->State.Scope,(UINT64)mProductWriteDoorbells,(UINT64)mProductSyncDoorbells,mProductVolume->State.VerifiedWrites,(UINT64)mProductOldTpl,mProductRaisedTpl));
  mProductExecuting=mBlockBusy=FALSE;if(mProductRaisedTpl)gBS->RestoreTPL(mProductOldTpl);mProductRaisedTpl=FALSE;return EFI_SUCCESS;
}
STATIC EFI_STATUS ProductRead(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes,VOID *Buffer,UINTN *Got){
  if(!Got)return EFI_INVALID_PARAMETER;*Got=0;if(!ProductContext(Context) || Lun!=4 || !Buffer || !Bytes || Bytes%4096)return EFI_ACCESS_DENIED;
  BOOLEAN Metadata=(Lba==1 && Bytes==4096)||(Lba==2 && Bytes==12288)||(Lba==378879 && Bytes==4096)||(Lba==378873 && Bytes==12288);
  if(!Metadata && (Lba<375040 || Lba>378623 || Bytes/4096>378624-Lba))return EFI_ACCESS_DENIED;
  for(UINTN O=0;O<Bytes;O+=4096){EFI_STATUS S=ReadScsi("PRODUCT_VOLUME_READ10",mServiceTag++,4,PianoUfsReadLba10,(UINT32)(Lba+O/4096),4096);
    UINT8 *R=(UINT8 *)mUcd.Cpu+PIANO_UFS_RESPONSE_OFFSET;if(S!=EFI_SUCCESS)return ProductRetain(S);
    if(mTransferred!=4096 || R[2]!=4 || (R[1]&0x60) || EngineBe32(R+12))return ProductRetain(EFI_BAD_BUFFER_SIZE);
    CopyMem((UINT8 *)Buffer+O,mData.Cpu,4096);*Got+=4096;}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS ProductWriteFua(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes,CONST VOID *Buffer,UINTN *Done){
  if(!Done)return EFI_INVALID_PARAMETER;*Done=0;
  if(!ProductContext(Context) || !mProductVolume->State.Provisioned || Lun!=4 || !Buffer || Bytes!=4096 || Lba<375042 || Lba>378623)return EFI_ACCESS_DENIED;
  EFI_STATUS S=PianoUfsProductVolumeRefreshReservation(mProductVolume);if(S!=EFI_SUCCESS)return S;
  CopyMem(mData.Cpu,Buffer,4096);UINT8 Tag=mServiceTag++;
  S=PianoUfsBoundedBuildWrite10(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,mData.DeviceAddress,Tag,4,Lba,4096);
  if(S==EFI_SUCCESS)S=Submit("PRODUCT_VOLUME_WRITE10_FUA",Tag,FALSE);
  if(S!=EFI_SUCCESS)return ProductRetain(S);*Done=mTransferred;return EFI_SUCCESS;
}
STATIC EFI_STATUS ProductSync(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes){
  if(!ProductContext(Context) || !mProductVolume->State.Provisioned || Lun!=4 || Lba!=375040 || Bytes!=14680064)return EFI_ACCESS_DENIED;
  EFI_STATUS S=PianoUfsProductVolumeRefreshReservation(mProductVolume);if(S!=EFI_SUCCESS)return S;UINT8 Tag=mServiceTag++;
  S=PianoUfsBoundedBuildSync10(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,Tag,4,Lba,Bytes);
  if(S==EFI_SUCCESS)S=Submit("PRODUCT_VOLUME_FULL_RANGE_SYNC10",Tag,FALSE);return S==EFI_SUCCESS?S:ProductRetain(S);
}
STATIC EFI_STATUS ProductGuard(VOID *Context,UINT8 Lun,PIANO_UFS_WRITE_GUARD *G){return ProductContext(Context)?FreshWriteGuard(Lun,G):EFI_ACCESS_DENIED;}
EFI_STATUS PianoUfsProductTransportIo(PIANO_UFS_PRODUCT_VOLUME *D,PIANO_UFS_WINDOW_IO *Io){
  if(!gBS || !gBS->RaiseTPL || !gBS->RestoreTPL)return EFI_UNSUPPORTED;
  EFI_TPL Tpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Tpl);if(Tpl!=TPL_APPLICATION)return EFI_UNSUPPORTED;
  if(!D || !Io || D->Signature || mProductVolume || mProductExecuting || mProductRetained || !mBlockLive || !mInstalled || mExitRetained || mData.Direction!=PianoDmaBidirectional)return EFI_NOT_READY;
  mProductVolume=D;*Io=(PIANO_UFS_WINDOW_IO){.Io={D,ProductRead,ProductWriteFua,ProductSync,ProductGuard,ProductQuiet},.Acquire=ProductAcquire,.Release=ProductRelease};return EFI_SUCCESS;
}
EFI_STATUS PianoUfsProductTransportPublish(PIANO_UFS_PRODUCT_VOLUME *D){
  EFI_TPL Tpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Tpl);if(Tpl!=TPL_APPLICATION)return EFI_UNSUPPORTED;
  if(D!=mProductVolume || !D || !D->State.Provisioned || !D->Media.MediaPresent || D->Media.ReadOnly || D->State.Quarantined || D->State.NeedsRecovery ||
     D->State.Busy || mProductRetained || mProductPublished || mBlockHandles[7]!=NULL || mLunCount>7)return EFI_NOT_READY;
  D->State.Scope=PianoProductIoProbe;EFI_STATUS S=ProductAcquire(D);if(S!=EFI_SUCCESS)return S;D->State.Busy=TRUE;S=PianoUfsProductVolumeRefreshReservation(D);D->State.Busy=FALSE;
  EFI_STATUS End=ProductRelease(D,S!=EFI_SUCCESS);if(S!=EFI_SUCCESS || End!=EFI_SUCCESS)return S==EFI_SUCCESS?End:S;
  mProductPath.Vendor.Header=(EFI_DEVICE_PATH_PROTOCOL){HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{sizeof(VENDOR_DEVICE_PATH)+16,0}};
  mProductPath.Vendor.Guid=(EFI_GUID)PIANO_PRODUCT_STORAGE_TYPE_GUID;CopyMem(mProductPath.VolumeUuid,D->NvIo.VolumeUuid,16);
  mProductPath.End=(EFI_DEVICE_PATH_PROTOCOL){END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}};
  S=gBS->InstallMultipleProtocolInterfaces(&mBlockHandles[7],&gEfiBlockIoProtocolGuid,&D->Block,&gEfiDevicePathProtocolGuid,&mProductPath,NULL);
  if(S!=EFI_SUCCESS || mBlockHandles[7]==NULL)return ProductRetain(S);mProductPublished=TRUE;
  S=gBS->ConnectController(mBlockHandles[7],NULL,NULL,TRUE);
  if(S!=EFI_SUCCESS && S!=EFI_NOT_FOUND)return ProductRetain(S);return EFI_SUCCESS;
}
