/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdarg.h>
#include "../../windows/drivers/ufs/miniport.c"

struct host {
    UFS_ADAPTER adapter;
    ULONG regs[0x304/4];
    struct ufs_utrd list;
    struct ufs_management_ucd ucd;
    STOR_SCATTER_GATHER_LIST sg;
    SCSI_REQUEST_BLOCK *completed[16];
    unsigned completions, lock, timer, hold_list, hold_hce, reset_writes;
    unsigned begin, adjusted, worker_busy;
    uint8_t negotiated[UfsPaParameterCount], observed[UfsPaParameterCount], disk[4], data[4];
    uint8_t protected_lu, mode, s0;
    HW_WORKITEM *worker;
    PVOID context;
    ULONGLONG now;
};

ULONG StorPortReadRegisterUlong(PVOID ctx,PULONG reg) { (void)ctx; return *reg; }
VOID StorPortWriteRegisterUlong(PVOID ctx,PULONG reg,ULONG value)
{
    struct host *h=ctx;
    switch ((size_t)(reg-h->regs)*4) {
    case 0x20: *reg &= ~value; break;
    case 0x34:
        if (value) *reg=value;
        else {
            ++h->reset_writes;
            if (!h->hold_hce) {
                *reg=h->regs[0x60/4]=h->regs[0x58/4]=0; h->regs[0x30/4]=8;
            }
        }
        break;
    case 0x60: if (value || !h->hold_list) *reg=value; break;
    case 0x58: assert(!*reg && h->regs[0x60/4]); *reg=value; break;
    case 0x90:
        if (value==0x16) {
            assert(h->begin && !h->adjusted); h->regs[0x30/4]=0x10f;
        } else if (value==1) {
            static const unsigned ids[]={0x1571,0x1568,0x1583,0x1560,0x1580,0x156a};
            unsigned n=0; while (n<RTL_NUMBER_OF(ids) && ids[n]!=(h->regs[0x94/4]>>16)) ++n;
            assert(n<RTL_NUMBER_OF(ids)); h->regs[0x9c/4]=h->negotiated[n];
        } else if (value==0x17 || value==0x18) {
            h->regs[0x20/4]|=value==0x17?0x40:0x20;
            break;
        } else assert(0);
        h->regs[0x20/4]|=0x400;
        break;
    default: *reg=value;
    }
}
ULONG StorPortQueryPerformanceCounter(PVOID ctx,LARGE_INTEGER *hz,LARGE_INTEGER *value)
{
    if (hz) hz->QuadPart=1000000;
    value->QuadPart=(int64_t)((struct host *)ctx)->now;
    return STOR_STATUS_SUCCESS;
}
VOID StorPortNotification(int type,PVOID ctx,...)
{
    struct host *h=ctx;
    va_list ap; va_start(ap,ctx);
    if (type==RequestTimerCall) {
        HW_TIMER *callback=va_arg(ap,HW_TIMER *);
        assert(callback==poll_restart); h->timer=va_arg(ap,ULONG)!=0;
    } else {
        assert(type==RequestComplete && h->completions<RTL_NUMBER_OF(h->completed));
        h->completed[h->completions++]=va_arg(ap,PVOID);
    }
    va_end(ap);
}
PSTOR_SCATTER_GATHER_LIST StorPortGetScatterGatherList(PVOID ctx,PVOID srb)
{
    (void)srb; return &((struct host *)ctx)->sg;
}
BOOLEAN StorPortSynchronizeAccess(PVOID ctx,STOR_SYNCHRONIZED_ACCESS *f,PVOID data)
{
    struct host *h=ctx; assert(!h->lock); h->lock=1;
    BOOLEAN r=f(ctx,data); h->lock=0; return r;
}
ULONG StorPortQueueWorkItem(PVOID ctx,HW_WORKITEM *f,PVOID worker,PVOID data)
{
    struct host *h=ctx; assert(!h->lock && !h->worker && worker==h);
    if (h->worker_busy) { --h->worker_busy; return STOR_STATUS_BUSY; }
    h->worker=f; h->context=data; return STOR_STATUS_SUCCESS;
}

static void reply_management(struct host *h)
{
    uint8_t *p=h->ucd.request,*r=h->ucd.response;
    if (!p[0]) r[0]=0x20;
    else if (p[0]==1) {
        r[0]=0x21; r[2]=p[2];
        if (p[16]==0xa0) {
            assert(p[2]==0x81); r[1]=0x20; r[15]=248;
            h->ucd.lun_list[3]=8; h->ucd.lun_list[9]=7;
        } else if (p[16]==0x1b) {
            assert(p[2]==0xd0); h->mode=p[20] | (p[20]>>4);
        } else assert(p[16]==0x35 && p[2]==7);
    } else {
        assert(p[0]==0x16);
        r[0]=0x36; r[5]=p[5]; memcpy(r+12,p+12,4);
        if (p[12]==1) {
            uint8_t *d=r+32;
            d[0]=r[11]=r[19]=0x2d; d[1]=p[13];
            if (!p[13]) d[6]=1;
            else if (p[13]==2) { d[2]=7; d[3]=1; d[5]=h->protected_lu; }
            else assert(p[13]==7);
        } else if (p[12]==3) { assert(p[13]==2); r[23]=h->mode; }
        else if (p[12]==5 && (p[13]==3 || p[13]==4)) r[23]=p[13]==3?h->protected_lu:0;
        else assert(p[13]==1 && (p[12]==6 || p[12]==5));
    }
    h->list.data[8]=0; h->regs[0x58/4]=0; h->regs[0x20/4]|=1;
}
ULONG StorPortDelayExecution(PVOID ctx,ULONG us)
{
    struct host *h=ctx; assert(!h->lock); h->now+=us;
    if (h->adapter.io.managing && h->regs[0x58/4]) reply_management(h);
    return STOR_STATUS_SUCCESS;
}
static BOOLEAN bandwidth_begin(PVOID ctx)
{
    struct host *h=ctx; assert(!h->lock && !h->adapter.io.running);
    ++h->begin; h->adjusted=0; return TRUE;
}
static void bandwidth_ready(PVOID ctx,const uint8_t *parameters)
{
    struct host *h=ctx;
    assert(!h->lock && h->begin && !h->adapter.io.running && parameters);
    assert(adapter_control(h,ScsiRestartAdapter,NULL)==ScsiAdapterControlUnsuccessful && !h->adapter.io.running);
    memcpy(h->observed,parameters,sizeof(h->observed)); ++h->adjusted;
}
static BOOLEAN can_retain(PVOID ctx) { return ((struct host *)ctx)->s0; }
static void init(struct host *h)
{
    memset(h,0,sizeof(*h));
    UFS_ADAPTER *a=&h->adapter; UFS_IO_QUEUE *q=&a->io;
    h->now=10000; h->mode=0x11; h->regs[8/4]=0x400; h->regs[0x30/4]=0x10f; h->regs[0x34/4]=1;
    q->list=&h->list; q->list_dma.QuadPart=0x1000;
    q->slot[0].ucd=&h->ucd.scsi; q->slot[0].dma.QuadPart=0x2000;
    q->dma_limit=UINT32_MAX; q->slot_count=1; q->prds_per_command=2;
    q->registers=(PUCHAR)h->regs; q->extension=h; q->device_ready=TRUE; q->enabled_luns=0x80;
    a->platform.extension=h; a->platform.worker=h; a->platform.phy.phase=UFS_PLATFORM_READY;
    a->link_power_context=h; a->link_power_begin=bandwidth_begin; a->link_power_ready=bandwidth_ready;
    a->can_retain_s0=can_retain;
    a->state=UfsOnline;
    h->sg.NumberOfElements=1; h->sg.List[0].PhysicalAddress.QuadPart=0x4000; h->sg.List[0].Length=4;
    assert(UfsIoStart(q));
}
static void tick(struct host *h)
{
    assert(h->timer); h->timer=0; poll_restart(h);
}
static void transfer(struct host *h,SCSI_REQUEST_BLOCK *s,int write)
{
    *s=(SCSI_REQUEST_BLOCK){.Length=sizeof(*s),.Function=SRB_FUNCTION_EXECUTE_SCSI,.Lun=7,.CdbLength=10,
        .SrbFlags=write?SRB_FLAGS_DATA_OUT:SRB_FLAGS_DATA_IN,.DataTransferLength=4};
    s->Cdb[0]=write?0x2a:0x28;
    assert(start_io(h,s) && h->adapter.io.active==1 && s->SrbStatus==SRB_STATUS_PENDING);
}
static void finish_transfer(struct host *h)
{
    uint8_t *c=h->ucd.scsi.command,*r=h->ucd.scsi.response;
    assert(c[2]==7 && h->regs[0x58/4]==1);
    if (c[16]==0x2a) memcpy(h->disk,h->data,4);
    else { assert(c[16]==0x28); memcpy(h->data,h->disk,4); }
    r[0]=0x21; r[2]=7; h->list.data[8]=0; h->regs[0x58/4]=0; h->regs[0x20/4]|=1;
    assert(interrupt(h) && !h->adapter.io.active && !interrupt(h));
}
static void down(struct host *h,SCSI_REQUEST_BLOCK *p)
{
    *p=(SCSI_REQUEST_BLOCK){.Length=sizeof(*p),.Function=SRB_FUNCTION_POWER,.SrbPowerFlags=1,
        .DevicePowerState=StorPowerDeviceD3,.PowerAction=StorPowerActionSleep,.TimeOutValue=1};
    assert(start_io(h,p) && p->SrbStatus==SRB_STATUS_PENDING);
}
static void run_request(struct host *h,SCSI_REQUEST_BLOCK *s)
{
    for (unsigned n=0; n<100 && s->SrbStatus==SRB_STATUS_PENDING; ++n) {
        if (h->adapter.io.managing && h->regs[0x58/4]) reply_management(h);
        h->now+=1000;
        if (h->timer) tick(h);
        if (h->worker) {
            HW_WORKITEM *worker=h->worker; h->worker=NULL; worker(h,h->context,h);
        }
    }
    assert(s->SrbStatus!=SRB_STATUS_PENDING);
}
int main(void)
{
    struct host h; init(&h);
    SCSI_REQUEST_BLOCK w,r,p;
    memcpy(h.data,"UFS!",4);
    transfer(&h,&w,1); down(&h,&p); tick(&h);
    assert(!h.completions && !h.adapter.power_touched);
    h.now+=1000000; tick(&h);
    assert(h.adapter.state==UfsOnline && h.completed[0]==&p && p.SrbStatus==SRB_STATUS_ERROR);
    finish_transfer(&h);
    assert(h.completions==2 && h.completed[1]==&w && w.SrbStatus==SRB_STATUS_SUCCESS);
    memset(h.data,0,4); transfer(&h,&r,0); finish_transfer(&h);
    assert(h.completions==3 && h.completed[2]==&r && !memcmp(h.data,"UFS!",4));

    /* An unacknowledged list stop has already changed hardware. Reset with
     * DMA still pinned, then start a genuinely new link and apply its speed. */
    for (unsigned n=0; n<2; ++n) {
        h.hold_list=h.hold_hce=1; down(&h,&p); tick(&h);
        assert(p.SrbStatus==SRB_STATUS_ERROR && h.adapter.state==UfsRecovering && !h.adapter.media_off);
        tick(&h);
        assert(h.adapter.io.owns_list && h.adapter.io.resetting && !h.worker);
        h.regs[0x34/4]=h.regs[0x60/4]=0; h.regs[0x30/4]=8;
        h.hold_list=h.hold_hce=0; tick(&h); tick(&h);
        assert(h.worker && h.adapter.state==UfsStarting);
        h.negotiated[0]=0x11; h.negotiated[1]=(uint8_t)(4-n); h.negotiated[2]=(uint8_t)(5-n);
        h.negotiated[3]=h.negotiated[4]=(uint8_t)(2-n); h.negotiated[5]=2;
        HW_WORKITEM *worker=h.worker; h.worker=NULL; worker(&h,h.context,&h);
        assert(h.adapter.state==UfsOnline && h.adapter.io.running && h.begin==n+1 && h.adjusted==1);
        assert(!memcmp(h.observed,h.negotiated,sizeof(h.observed)));
        memset(h.data,0,4); transfer(&h,&r,0); finish_transfer(&h);
        assert(!memcmp(h.data,"UFS!",4));
    }
    init(&h);
    memcpy(h.data,"disk",4); transfer(&h,&w,1); finish_transfer(&h);
    h.protected_lu=h.s0=h.adapter.media.check_power_on_wp=1;
    p=(SCSI_REQUEST_BLOCK){.Length=sizeof(p),.Function=SRB_FUNCTION_POWER,.SrbPowerFlags=1,
                           .DevicePowerState=StorPowerDeviceD3,.PowerAction=StorPowerActionNone,.TimeOutValue=2};
    assert(start_io(&h,&p)); run_request(&h,&p);
    assert(p.SrbStatus==SRB_STATUS_SUCCESS && h.adapter.state==UfsAsleep && h.mode==0x22 &&
           !h.adapter.media_off && h.regs[0x34/4]==1 && h.protected_lu && !h.adapter.io.running);
    assert(adapter_control(&h,ScsiStopAdapter,NULL)==ScsiAdapterControlSuccess);
    p.DevicePowerState=StorPowerDeviceD0; h.worker_busy=2;
    assert(start_io(&h,&p)); run_request(&h,&p);
    assert(p.SrbStatus==SRB_STATUS_SUCCESS && h.adapter.state==UfsOnline && h.mode==0x11 && h.protected_lu);
    memset(h.data,0,4); transfer(&h,&r,0); finish_transfer(&h); assert(!memcmp(h.data,"disk",4));
    down(&h,&p); run_request(&h,&p); /* A subsequent Sx is not covered by the S0 reference. */
    assert(p.SrbStatus==SRB_STATUS_ERROR && h.adapter.state==UfsOnline && h.protected_lu && !h.adapter.media_off);

    p.DevicePowerState=StorPowerDeviceD3; p.PowerAction=StorPowerActionNone;
    assert(start_io(&h,&p)); run_request(&h,&p); assert(h.adapter.state==UfsAsleep);
    p.DevicePowerState=StorPowerDeviceD0; p.TimeOutValue=1; h.worker_busy=5;
    assert(start_io(&h,&p)); tick(&h); assert(p.SrbStatus==SRB_STATUS_PENDING);
    h.now+=1000000; tick(&h);
    assert(p.SrbStatus==SRB_STATUS_ERROR && h.adapter.state==UfsAsleep && !h.timer && !h.worker && !h.adapter.media_off);
    h.worker_busy=0; assert(start_io(&h,&p)); run_request(&h,&p);
    assert(p.SrbStatus==SRB_STATUS_SUCCESS && h.adapter.state==UfsOnline);
    return 0;
}
