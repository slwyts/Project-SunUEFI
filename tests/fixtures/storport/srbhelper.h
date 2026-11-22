/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "storport.h"
#define SRB_FIELD(p,m) (((PSCSI_REQUEST_BLOCK)(p))->m)
#define SrbGetSrbFunction(p) (SRB_FIELD(p,Function)==SRB_FUNCTION_STORAGE_REQUEST_BLOCK ? \
                              SRB_FIELD(p,SrbFunction) : SRB_FIELD(p,Function))
#define SrbGetPrimarySrbExData(p) SRB_FIELD(p,ExData)
#define SrbGetPathId(p) SRB_FIELD(p,PathId)
#define SrbGetTargetId(p) SRB_FIELD(p,TargetId)
#define SrbGetLun(p) SRB_FIELD(p,Lun)
#define SrbGetTimeOutValue(p) SRB_FIELD(p,TimeOutValue)
#define SrbGetCdb(p) SRB_FIELD(p,Cdb)
#define SrbGetCdbLength(p) SRB_FIELD(p,CdbLength)
#define SrbGetSrbFlags(p) SRB_FIELD(p,SrbFlags)
#define SrbGetDataTransferLength(p) SRB_FIELD(p,DataTransferLength)
#define SrbGetRequestAttribute(p) SRB_FIELD(p,RequestAttribute)
#define SrbGetMiniportContext(p) SRB_FIELD(p,MiniportContext)
#define SrbGetSenseInfoBuffer(p) SRB_FIELD(p,Sense)
#define SrbGetSenseInfoBufferLength(p) SRB_FIELD(p,SenseLength)
#define SrbSetScsiStatus(p,v) (SRB_FIELD(p,ScsiStatus)=(v))
#define SrbSetSrbStatus(p,v) (SRB_FIELD(p,SrbStatus)=(v))
#define SrbSetSenseInfoBufferLength(p,v) (SRB_FIELD(p,SenseLength)=(v))
#define SrbSetDataTransferLength(p,v) (SRB_FIELD(p,DataTransferLength)=(v))
