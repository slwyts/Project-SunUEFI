// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included only inside the default-off, exclusive UFS diagnostic translation
// unit. This is not an arbitrary write protocol or a standalone transport.
STATIC UINTN mWindowDepth;
STATIC BOOLEAN mWindowRetainedTpl;
STATIC struct {VENDOR_DEVICE_PATH Vendor;EFI_DEVICE_PATH_PROTOCOL End;} mWindowPath;
STATIC VOID EFIAPI PianoUfsExitBoot(EFI_EVENT Event,VOID *Context);
STATIC PIANO_UFS_SHUTDOWN mShutdown;
STATIC BOOLEAN WindowContext(VOID *Context){
  return mWindowExecuting && mBlockBusy && !mExitRetained &&
    ((Context==&mWindow && !mWindow.State.Closed) || (Context==&mWindowRecovery && mWindow.State.Closed && mWindowDisconnected));
}
STATIC EFI_STATUS WindowQuiet(VOID *Context,BOOLEAN *Quiet){
  if(Quiet==NULL)return EFI_INVALID_PARAMETER;*Quiet=FALSE;if(!WindowContext(Context))return EFI_ACCESS_DENIED;
  EFI_STATUS S=Quiesce();if(!EFI_ERROR(S)){Write(0x24,0);if(Read(0x24)!=0 || !QueuesStopped())S=EFI_DEVICE_ERROR;}
  if(EFI_ERROR(S))return S;
  PIANO_DMA_BUFFER *Buffers[]={&mUcd,&mTrl,&mData};
  for(UINTN I=0;I<ARRAY_SIZE(Buffers);++I)if(Buffers[I]->Active){PianoDmaComplete(Buffers[I],mWindowTransportStatus,TRUE);if(Buffers[I]->Active || Buffers[I]->Quarantined)return EFI_DEVICE_ERROR;}
  *Quiet=TRUE;return EFI_SUCCESS;
}
STATIC EFI_STATUS WindowAcquire(VOID *Context){
  BOOLEAN Recovery=Context==&mWindowRecovery;
  if(Context!=&mWindow && !Recovery)return EFI_ACCESS_DENIED;
  if((Recovery && (!mWindow.State.Closed || !mWindowDisconnected)) || (!Recovery && (mWindow.State.Closed || mWindow.State.Quarantined)))return EFI_ACCESS_DENIED;
  // Root holds recovery across restore and final independent verification.
  // The verification core can nest under that same private recovery context.
  if(mWindowExecuting && mBlockBusy && mWindowRecovery && Recovery){++mWindowDepth;return EFI_SUCCESS;}
  if(mBlockBusy || mExitRetained || !mInstalled)return EFI_NOT_READY;
  if(!mWindowRetainedTpl)mWindowOldTpl=gBS->RaiseTPL(TPL_CALLBACK);
  mWindowExecuting=mBlockBusy=TRUE;mWindowRecovery=Recovery;mWindowDepth=1;
  BOOLEAN Quiet=FALSE;EFI_STATUS S=WindowQuiet(Context,&Quiet);
  if(S!=EFI_SUCCESS || !Quiet){mWindow.State.Quarantined=mWindow.State.NeedsRecovery=TRUE;mWindow.Media.ReadOnly=TRUE;mWindow.State.LastQuietStatus=S;mWindowRetainedTpl=TRUE;mWindowExecuting=mBlockBusy=FALSE;mWindowDepth=0;return S==EFI_SUCCESS?EFI_NOT_READY:S;}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS WindowRelease(VOID *Context,BOOLEAN Quarantined){
  if(!WindowContext(Context) || mWindowDepth==0)return EFI_ACCESS_DENIED;
  if(Quarantined){mWindow.State.Quarantined=mWindow.State.NeedsRecovery=TRUE;mWindow.State.RestoreVerified=FALSE;mWindow.Media.ReadOnly=TRUE;}
  if(--mWindowDepth)return EFI_SUCCESS;
  BOOLEAN Quiet=FALSE;EFI_STATUS S=WindowQuiet(Context,&Quiet);
  mWindowExecuting=mBlockBusy=FALSE;
  if(S!=EFI_SUCCESS || !Quiet){mWindow.State.Quarantined=mWindow.State.NeedsRecovery=TRUE;mWindow.Media.ReadOnly=TRUE;mWindow.State.LastQuietStatus=S;mWindow.State.RestoreVerified=FALSE;mWindowRetainedTpl=TRUE;return S==EFI_SUCCESS?EFI_NOT_READY:S;}
  // Dirty/NeedsRecovery still fence timer/exception reset outside requests.
  // Keep no Active/quarantined DMA on a confirmed quiet return. A private
  // recovery sequence can safely reuse it while public IO stays closed.
  mWindowRetainedTpl=FALSE;gBS->RestoreTPL(mWindowOldTpl);return EFI_SUCCESS;
}
STATIC EFI_STATUS WindowRead(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes,VOID *Buffer,UINTN *Got){
  if(Got==NULL)return EFI_INVALID_PARAMETER;*Got=0;
  if(!WindowContext(Context) || Lun!=4 || Buffer==NULL || Bytes==0 || Bytes%4096 ||
    Lba>378879 || Bytes/4096>378880-Lba)return EFI_ACCESS_DENIED;
  // Only metadata gate or exact gap reads; no private parent-LU API either.
  BOOLEAN Metadata=(Lba==1 && Bytes==4096)||(Lba==2 && Bytes==12288)||(Lba==378879 && Bytes==4096)||(Lba==378873 && Bytes==12288);
  if(!Metadata && (Lba<PIANO_UFS_WINDOW_FIRST || Lba>PIANO_UFS_WINDOW_LAST || Bytes/4096>PIANO_UFS_WINDOW_LAST-Lba+1))return EFI_ACCESS_DENIED;
  for(UINTN O=0;O<Bytes;O+=4096){
    EFI_STATUS S=ReadScsi("WINDOW_READ10",mServiceTag++,4,PianoUfsReadLba10,(UINT32)(Lba+O/4096),4096);
    UINT8 *R=(UINT8 *)mUcd.Cpu+PIANO_UFS_RESPONSE_OFFSET;
    if(EFI_ERROR(S))return S;if(mTransferred!=4096 || R[2]!=4 || (R[1]&0x60) || EngineBe32(R+12))return EFI_BAD_BUFFER_SIZE;
    CopyMem((UINT8 *)Buffer+O,mData.Cpu,4096);*Got+=4096;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS WindowMutationGate(VOID *Context){
  EFI_STATUS S=PianoUfsWriteTestCheckBaseline(&mWindow.Baseline);if(EFI_ERROR(S))return S;
  S=FreshWriteGuard(4,&mWindow.LastGuard);if(!EFI_ERROR(S))S=PianoUfsWriteTestCheckGuard(&mWindow.LastGuard);
  BOOLEAN Quiet=FALSE;EFI_STATUS Q=WindowQuiet(Context,&Quiet);if(EFI_ERROR(Q) || !Quiet)return EFI_ERROR(Q)?Q:EFI_NOT_READY;if(EFI_ERROR(S))return S;
  VOID *Buffers[]={mWindow.Primary,mWindow.Entries,mWindow.Backup,mWindow.BackupEntries};
  STATIC CONST EFI_LBA Lbas[]={1,2,378879,378873};STATIC CONST UINTN Sizes[]={4096,12288,4096,12288};
  for(UINTN I=0;I<4;++I){UINTN Got=0;S=WindowRead(Context,4,Lbas[I],Sizes[I],Buffers[I],&Got);Q=WindowQuiet(Context,&Quiet);if(EFI_ERROR(Q) || !Quiet)return EFI_ERROR(Q)?Q:EFI_NOT_READY;if(EFI_ERROR(S))return S;if(Got!=Sizes[I])return EFI_BAD_BUFFER_SIZE;}
  PIANO_UFS_WRITE_BLOB P={mWindow.Primary,4096},PE={mWindow.Entries,12288},B={mWindow.Backup,4096},BE={mWindow.BackupEntries,12288};
  return PianoUfsWriteTestCheckLiveGpt(&mWindow.Baseline,&P,&PE,&B,&BE);
}
STATIC EFI_STATUS WindowWriteFua(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes,CONST VOID *Buffer,UINTN *Done){
  if(Done==NULL)return EFI_INVALID_PARAMETER;*Done=0;
  if(!WindowContext(Context) || Lun!=4 || Lba<PIANO_UFS_WINDOW_FIRST || Lba>PIANO_UFS_WINDOW_LAST || Bytes!=4096 || Buffer==NULL)return EFI_ACCESS_DENIED;
  // Recovery also stays dirty until a complete independent verification; root
  // must keep its separate whole-gap attempt ledger before this callback.
  mWindow.State.Dirty=mWindow.State.NeedsRecovery=TRUE;mWindow.State.RestoreVerified=FALSE;
  EFI_STATUS Gate=WindowMutationGate(Context);if(EFI_ERROR(Gate))return Gate;
  CopyMem(mData.Cpu,Buffer,4096);UINT8 Tag=mServiceTag++;
  EFI_STATUS S=PianoUfsBoundedBuildWrite10(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,mData.DeviceAddress,Tag,Lun,Lba,Bytes);
  if(!EFI_ERROR(S))S=Submit("WINDOW_WRITE10_FUA",Tag,FALSE);if(!EFI_ERROR(S))*Done=mTransferred;return S;
}
STATIC EFI_STATUS WindowSync(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes){
  if(!WindowContext(Context) || Lun!=4 || Lba!=PIANO_UFS_WINDOW_FIRST || Bytes!=PIANO_UFS_WINDOW_BYTES)return EFI_ACCESS_DENIED;EFI_STATUS Gate=WindowMutationGate(Context);if(EFI_ERROR(Gate))return Gate;UINT8 Tag=mServiceTag++;
  EFI_STATUS S=PianoUfsBoundedBuildSync10(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,Tag,Lun,Lba,Bytes);
  return EFI_ERROR(S)?S:Submit("WINDOW_FULL_RANGE_SYNC10",Tag,FALSE);
}
STATIC EFI_STATUS WindowGuard(VOID *Context,UINT8 Lun,PIANO_UFS_WRITE_GUARD *Guard){return WindowContext(Context)?FreshWriteGuard(Lun,Guard):EFI_ACCESS_DENIED;}
STATIC PIANO_UFS_WINDOW_IO WindowIo(BOOLEAN Recovery){
  return (PIANO_UFS_WINDOW_IO){.Io={Recovery?(VOID *)&mWindowRecovery:(VOID *)&mWindow,WindowRead,WindowWriteFua,WindowSync,WindowGuard,WindowQuiet},.Acquire=WindowAcquire,.Release=WindowRelease};
}
CONST PIANO_UFS_WINDOW_STATE *PianoUfsBoundedTransportState(VOID){return &mWindow.State;}
EFI_HANDLE PianoUfsBoundedTransportHandle(VOID){return mBlockHandles[0];}
EFI_STATUS PianoUfsBoundedTransportCloseForRecovery(PIANO_UFS_WINDOW_IO *Recovery){
  if(Recovery==NULL || mWindow.State.Busy || mBlockBusy)return EFI_INVALID_PARAMETER;
  if(!mWindowDisconnected){
    EFI_STATUS S=mBlockHandles[0]==NULL?EFI_SUCCESS:gBS->DisconnectController(mBlockHandles[0],NULL,NULL);
    mWindow.State.DisconnectStatus=S;mWindow.State.ConsumersDetached=!EFI_ERROR(S);
    // Even failed FAT Stop cannot keep a writable protocol alive during raw
    // recovery. Close public IO and preserve the honest detach status/handles;
    // later normal Stop still requires real Disconnect before any free.
  }
  mWindowDisconnected=TRUE;EFI_STATUS S=PianoUfsBoundedBlockCloseForRecovery(&mWindow);
  if(!EFI_ERROR(S))*Recovery=WindowIo(TRUE);return S;
}
EFI_STATUS PianoUfsBoundedTransportVerifyRestored(VOID){PIANO_UFS_WINDOW_IO Recovery=WindowIo(TRUE);return PianoUfsBoundedBlockVerifyRestored(&mWindow,&Recovery);}
STATIC EFI_STATUS WindowRequireIsolation(VOID){
  EFI_GUID *Protocols[]={&gEfiBlockIoProtocolGuid,&gEfiSimpleFileSystemProtocolGuid};
  for(UINTN I=0;I<ARRAY_SIZE(Protocols);++I){
    EFI_HANDLE *Handles=NULL;UINTN Count=0;EFI_STATUS S=gBS->LocateHandleBuffer(ByProtocol,Protocols[I],NULL,&Count,&Handles);
    if(Handles!=NULL)FreePool(Handles);
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WINDOW_ISOLATION kind=%a count=%lu status=%r\n",I==0?"BlockIO":"SimpleFileSystem",(UINT64)Count,S));
    if(S!=EFI_NOT_FOUND && S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
    if(Count!=0)return EFI_ACCESS_DENIED;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS PublishWindow(VOID){
  EFI_STATUS Isolated=WindowRequireIsolation();if(EFI_ERROR(Isolated))return Isolated;
  PIANO_UFS_WRITE_BASELINE B={.PrimaryHeader={mPianoUfsWriteTestPrimaryHeader,sizeof(mPianoUfsWriteTestPrimaryHeader)},.PrimaryEntries={mPianoUfsWriteTestPrimaryEntries,sizeof(mPianoUfsWriteTestPrimaryEntries)},
    .BackupHeader={mPianoUfsWriteTestBackupHeader,sizeof(mPianoUfsWriteTestBackupHeader)},.BackupEntries={mPianoUfsWriteTestBackupEntries,sizeof(mPianoUfsWriteTestBackupEntries)},.OriginalBlock={mPianoUfsWriteTestOriginalBlock,sizeof(mPianoUfsWriteTestOriginalBlock)},
    .ExternalArchiveVerified=PIANO_UFS_WRITE_BASELINE_EXTERNAL_ARCHIVE_VERIFIED,.ExternalGapBytes=PIANO_UFS_WRITE_BASELINE_GAP_BYTES};
  CopyMem(B.ExternalGapSha256,mPianoUfsWriteTestGapSha256,32);PIANO_UFS_WINDOW_IO Io=WindowIo(FALSE);
  EFI_STATUS S=PianoUfsBoundedBlockOpen(&mWindow,TRUE,&B,&Io);if(EFI_ERROR(S))return S;
  mWindowPath.Vendor.Header=(EFI_DEVICE_PATH_PROTOCOL){HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{sizeof(VENDOR_DEVICE_PATH),0}};
  mWindowPath.Vendor.Guid=(EFI_GUID){0x9234B248,0x6383,0x4DD2,{0x88,0x20,0x04,0x37,0x50,0x40,0x14,0x01}};
  mWindowPath.End=(EFI_DEVICE_PATH_PROTOCOL){END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}};
  S=gBS->InstallMultipleProtocolInterfaces(&mBlockHandles[0],&gEfiBlockIoProtocolGuid,&mWindow.Block,&gEfiDevicePathProtocolGuid,&mWindowPath,NULL);if(EFI_ERROR(S))return S;
  EFI_GUID Guid=PIANO_UFS_SHUTDOWN_GUID;S=gBS->InstallMultipleProtocolInterfaces(&mShutdownHandle,&Guid,&mShutdown,NULL);if(EFI_ERROR(S))return S;
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,PianoUfsExitBoot,NULL,&gEfiEventExitBootServicesGuid,&mExitBootEvent);if(EFI_ERROR(S))return S;
  PianoUfsRetainClocks();
  S=gBS->ConnectController(mBlockHandles[0],NULL,NULL,TRUE);
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WINDOW_CONNECT %r block=4096 last=3583 only_window=1 original_lun_handles_published=0\n",S));
  // No FAT yet is an honest possible result, not a failed storage publication.
  return EFI_SUCCESS;
}
