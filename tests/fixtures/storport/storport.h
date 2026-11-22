/* SPDX-License-Identifier: BSD-2-Clause */
/* Native test port: the genuine WDK is checked separately by the ARM64 build. */
#pragma once
#include "ntddk.h"
enum { RequestTimerCall, RequestComplete, PCIBus, PCIConfiguration, StorSynchronizeHalfDuplex,
       PCI_INVALID_VENDORID=0xffff, PCI_DEVICE_TYPE=0, PCI_CLASS_MASS_STORAGE_CTLR=1,
       PCI_COMMON_HDR_LENGTH=64, PCI_ADDRESS_MEMORY_TYPE_MASK=6, PCI_TYPE_64BIT=4,
       PCI_ADDRESS_IO_SPACE=1, PCI_ENABLE_MEMORY_SPACE=2, PCI_ENABLE_BUS_MASTER=4 };
#define PCI_ADDRESS_MEMORY_ADDRESS_MASK UINT32_C(0xfffffff0)
enum { SRB_FUNCTION_EXECUTE_SCSI=0, SRB_FUNCTION_SHUTDOWN=7, SRB_FUNCTION_FLUSH=8,
       SRB_FUNCTION_POWER=0x24, SRB_FUNCTION_PNP=0x25, SRB_FUNCTION_STORAGE_REQUEST_BLOCK=0x28,
       SRB_STATUS_PENDING=0, SRB_STATUS_SUCCESS=1, SRB_STATUS_ABORTED=2, SRB_STATUS_ERROR=4,
       SRB_STATUS_BUSY=5, SRB_STATUS_INVALID_REQUEST=6, SRB_STATUS_NO_DEVICE=8, SRB_STATUS_BUS_RESET=14,
       SRB_STATUS_NO_HBA=17, SRB_STATUS_DATA_OVERRUN=18, SRB_STATUS_AUTOSENSE_VALID=0x80,
       SRB_FLAGS_DATA_IN=0x40, SRB_FLAGS_DATA_OUT=0x80, SRB_FLAGS_QUEUE_ACTION_ENABLE=2,
       SRB_FLAGS_DISABLE_AUTOSENSE=0x20, SRB_HEAD_OF_QUEUE_TAG_REQUEST=0x21, SRB_ORDERED_QUEUE_TAG_REQUEST=0x22,
       SRB_POWER_FLAGS_ADAPTER_REQUEST=1, SRB_PNP_FLAGS_ADAPTER_REQUEST=1, SRBEX_DATA_POWER_LENGTH=12,
       SRBEX_DATA_PNP_LENGTH=16, SCSISTAT_BUSY=8, SCSISTAT_CHECK_CONDITION=2, STOR_STATUS_SUCCESS=0, STOR_STATUS_BUSY=1,
       SP_RETURN_FOUND=1, SP_RETURN_NOT_FOUND=0, SP_RETURN_BAD_CONFIG=2, SP_RETURN_ERROR=3,
       SCSI_DMA64_SYSTEM_SUPPORTED=1, SCSI_DMA64_MINIPORT_FULL64BIT_NO_BOUNDARY_REQ_SUPPORTED=2,
       STOR_ADAPTER_DMA_ADDRESS_WIDTH_SPECIFIED=4, SRB_TYPE_FLAG_SCSI_REQUEST_BLOCK=1,
       SRB_TYPE_FLAG_STORAGE_REQUEST_BLOCK=2, ADDRESS_TYPE_FLAG_BTL8=1 };
typedef enum { StorPowerDeviceUnspecified,StorPowerDeviceD0,StorPowerDeviceD1,StorPowerDeviceD2,StorPowerDeviceD3 }
    STOR_DEVICE_POWER_STATE;
typedef enum { StorPowerActionNone,StorPowerActionReserved,StorPowerActionSleep,StorPowerActionHibernate,
               StorPowerActionShutdown } STOR_POWER_ACTION;
typedef enum { StorStartDevice, StorRemoveDevice=2, StorStopDevice=4 } STOR_PNP_ACTION;
typedef enum { ScsiQuerySupportedControlTypes, ScsiStopAdapter, ScsiRestartAdapter, ScsiSetRunningConfig,
               ScsiAdapterSurpriseRemoval } SCSI_ADAPTER_CONTROL_TYPE;
typedef enum { ScsiAdapterControlSuccess, ScsiAdapterControlUnsuccessful } SCSI_ADAPTER_CONTROL_STATUS;
typedef struct { ULONG MaxControlType; BOOLEAN SupportedTypeList[10]; } *PSCSI_SUPPORTED_CONTROL_TYPE_LIST;
typedef struct { ULONG Type,Length; UCHAR SrbPowerFlags; STOR_DEVICE_POWER_STATE DevicePowerState;
                 STOR_POWER_ACTION PowerAction; } *PSRBEX_DATA_POWER;
typedef struct { ULONG Type,Length; STOR_PNP_ACTION PnPAction; ULONG SrbPnPFlags; } *PSRBEX_DATA_PNP;
typedef struct { ULONG Type,Length; } *PSRBEX_DATA;
typedef struct { USHORT Length; UCHAR Function,SrbStatus,SrbPowerFlags,PathId,TargetId,Lun;
                 STOR_DEVICE_POWER_STATE DevicePowerState; ULONG SrbFlags,DataTransferLength,TimeOutValue;
                 STOR_POWER_ACTION PowerAction; STOR_PNP_ACTION PnPAction; ULONG SrbPnPFlags;
                 ULONG SrbLength,SrbFunction; UCHAR Cdb[16],CdbLength,ScsiStatus,RequestAttribute,SenseLength;
                 void *Sense,*MiniportContext; PSRBEX_DATA ExData; }
    SCSI_REQUEST_BLOCK,SCSI_POWER_REQUEST_BLOCK,SCSI_PNP_REQUEST_BLOCK,*PSCSI_REQUEST_BLOCK,
    *PSCSI_POWER_REQUEST_BLOCK,*PSCSI_PNP_REQUEST_BLOCK,*PSTORAGE_REQUEST_BLOCK;
typedef struct { STOR_PHYSICAL_ADDRESS RangeStart; ULONG RangeLength; BOOLEAN RangeInMemory; } ACCESS_RANGE;
typedef unsigned INTERFACE_TYPE;
typedef struct { ULONG Length,NumberOfBuses,MaximumNumberOfTargets,MaximumNumberOfLogicalUnits; UCHAR InitiatorBusId[1];
                 BOOLEAN TaggedQueuing,MultipleRequestPerLu,AutoRequestSense,CachesData;
                 ULONG AdapterInterfaceType,SystemIoBusNumber,SlotNumber,NumberOfAccessRanges; ACCESS_RANGE (*AccessRanges)[1];
                 UCHAR Dma64BitAddresses,Dma32BitAddresses,DmaAddressWidth; ULONG FeatureSupport;
                 BOOLEAN ScatterGather,Master; ULONG AlignmentMask,NumberOfPhysicalBreaks,MaximumTransferLength,
                 SrbExtensionSize,SynchronizationModel,MaxNumberOfIO,MaxIOsPerLun; } PORT_CONFIGURATION_INFORMATION,
    *PPORT_CONFIGURATION_INFORMATION;
typedef struct { ULONG VendorID,HeaderType,BaseClass,SubClass,ProgIf,Command;
                 struct { struct { ULONG BaseAddresses[6]; } type0; } u; } PCI_COMMON_CONFIG;
typedef struct { ULONG NumberOfElements; struct { STOR_PHYSICAL_ADDRESS PhysicalAddress; ULONG Length; } List[1]; }
    STOR_SCATTER_GATHER_LIST,*PSTOR_SCATTER_GATHER_LIST;
typedef NTSTATUS DRIVER_INITIALIZE(PDRIVER_OBJECT,PUNICODE_STRING);
typedef ULONG HW_FIND_ADAPTER(PVOID,PVOID,PVOID,PCHAR,PPORT_CONFIGURATION_INFORMATION,PBOOLEAN);
typedef HW_FIND_ADAPTER *PHW_FIND_ADAPTER;
typedef BOOLEAN HW_INITIALIZE(PVOID), HW_PASSIVE_INITIALIZE_ROUTINE(PVOID), HW_STARTIO(PVOID,PSCSI_REQUEST_BLOCK),
    HW_INTERRUPT(PVOID), HW_RESET_BUS(PVOID,ULONG), STOR_SYNCHRONIZED_ACCESS(PVOID,PVOID);
typedef SCSI_ADAPTER_CONTROL_STATUS HW_ADAPTER_CONTROL(PVOID,SCSI_ADAPTER_CONTROL_TYPE,PVOID);
typedef VOID HW_TIMER(PVOID), HW_WORKITEM(PVOID,PVOID,PVOID);
typedef struct { ULONG HwInitializationDataSize,AdapterInterfaceType,DeviceExtensionSize,NumberOfAccessRanges;
                 PHW_FIND_ADAPTER HwFindAdapter; HW_INITIALIZE *HwInitialize; HW_STARTIO *HwBuildIo,*HwStartIo;
                 HW_INTERRUPT *HwInterrupt; HW_RESET_BUS *HwResetBus; HW_ADAPTER_CONTROL *HwAdapterControl;
                 ULONG NeedPhysicalAddresses,TaggedQueuing,MultipleRequestPerLu,AutoRequestSense,SrbTypeFlags,
                   AddressTypeFlags; } HW_INITIALIZATION_DATA;
ULONG StorPortQueryPerformanceCounter(PVOID,LARGE_INTEGER *,LARGE_INTEGER *);
ULONG StorPortReadRegisterUlong(PVOID,PULONG);
VOID StorPortWriteRegisterUlong(PVOID,PULONG,ULONG);
VOID StorPortNotification(int,PVOID,...);
BOOLEAN StorPortSynchronizeAccess(PVOID,STOR_SYNCHRONIZED_ACCESS *,PVOID);
ULONG StorPortDelayExecution(PVOID,ULONG);
ULONG StorPortQueueWorkItem(PVOID,HW_WORKITEM *,PVOID,PVOID);
PSTOR_SCATTER_GATHER_LIST StorPortGetScatterGatherList(PVOID,PVOID);
ULONG StorPortGetBusData(PVOID,int,ULONG,ULONG,void *,ULONG);
BOOLEAN StorPortEnablePassiveInitialization(PVOID,HW_PASSIVE_INITIALIZE_ROUTINE *);
ULONG StorPortInitialize(PDRIVER_OBJECT,PUNICODE_STRING,HW_INITIALIZATION_DATA *,PVOID);
PVOID StorPortGetDeviceBase(PVOID,ULONG,ULONG,STOR_PHYSICAL_ADDRESS,ULONG,BOOLEAN);
PVOID StorPortGetUncachedExtension(PVOID,PPORT_CONFIGURATION_INFORMATION,ULONG);
STOR_PHYSICAL_ADDRESS StorPortGetPhysicalAddress(PVOID,PVOID,PVOID,ULONG *);
VOID StorPortFreeDeviceBase(PVOID,PVOID);
ULONG StorPortGetSystemAddress(PVOID,PVOID,PVOID *);
#define StorPortMoveMemory memcpy
