/* SPDX-License-Identifier: BSD-2-Clause */
#include "piano_rpmh.h"
#include <assert.h>
#include <string.h>

struct board {
    struct rpmh_client handle;
    unsigned references, open, attempt, power_reference, refuse, vcc, vccq, releases;
};
static void ref(PVOID ctx) { ++((struct board *)ctx)->references; }
static void deref(PVOID ctx) { --((struct board *)ctx)->references; }
static NTSTATUS open_client(PVOID ctx,struct rpmh_client **client)
{
    struct board *b=ctx; assert(!b->open); b->open=1; *client=&b->handle; return 0;
}
static NTSTATUS close_client(PVOID ctx,struct rpmh_client *client)
{
    struct board *b=ctx; assert(b->open && client==&b->handle && !b->vcc && !b->vccq);
    b->open=b->power_reference=0; ++b->releases; return 0;
}
static NTSTATUS hold(PVOID ctx,struct rpmh_client *client)
{
    struct board *b=ctx; assert(b->open && client==&b->handle); ++b->attempt;
    if (b->refuse) return -1;
    b->power_reference=1; return 0;
}
static NTSTATUS level(PVOID ctx,struct rpmh_client *c,enum rpmh_rail id,ULONG value)
{ (void)ctx; (void)c; (void)id; (void)value; return 0; }
static NTSTATUS bandwidth(PVOID ctx,struct rpmh_client *c,enum rpmh_fabric_path path,ULONG avg,ULONG peak)
{ (void)ctx; (void)c; (void)path; (void)avg; (void)peak; return 0; }
static NTSTATUS supplies(PVOID ctx,struct rpmh_client *c,enum rpmh_supply_load vcc,enum rpmh_supply_load vccq)
{
    struct board *b=ctx; assert(c==&b->handle); b->vcc=vcc; b->vccq=vccq; return 0;
}
static NTSTATUS aux(PVOID ctx,struct rpmh_client *c,enum rpmh_ufs_supply s,const struct rpmh_supply_request *v)
{ (void)ctx; (void)c; (void)s; (void)v; return 0; }
static NTSTATUS resources(PVOID ctx,unsigned rails,unsigned paths,unsigned supplies_mask)
{ (void)ctx; (void)rails; (void)paths; (void)supplies_mask; return 0; }
int main(void)
{
    for (unsigned denied=0; denied<=1; ++denied) {
        struct board board={.refuse=denied};
        PIANO_RPMH_INTERFACE api={.Header={sizeof(api),PIANO_RPMH_INTERFACE_VERSION,&board,ref,deref},
          .Open=open_client,.Close=close_client,.HoldS0Power=hold,.SetLevel=level,.SetBandwidth=bandwidth,
          .SetUfsSupplies=supplies,.SetAuxSupply=aux,.HoldBootResources=resources,.FinishBootResources=resources};
        PIANO_UFS_RPMH r={0}; LONG off=0;
        assert(UfsPianoOpenRpmh(&r,&api,&off) && board.references==1 && board.attempt==1);
        assert(!UfsPianoHasS0Power(&r)); /* An object reference or a D0 reference alone is insufficient. */
        assert(UfsPianoSetRailsAndPaths(&r,1) && UfsPianoDeviceReference(&r,1));
        r.owns_media=TRUE;
        for (unsigned i=0;i<UFS_PHY_SUPPLY_COUNT;++i) {
            assert(!UfsPianoHasS0Power(&r));
            assert(UfsPianoSetPhySupply(&r,(enum ufs_phy_supply)i,1));
        }
        assert(UfsPianoHasS0Power(&r)==!denied && board.power_reference==!denied);
        assert(!UfsPianoCloseRpmh(&r) && board.open && board.vcc && board.vccq && !board.releases);
        for (unsigned i=UFS_PHY_SUPPLY_COUNT;i;--i) assert(UfsPianoSetPhySupply(&r,(enum ufs_phy_supply)(i-1),0));
        assert(UfsPianoDeviceReference(&r,0) && !UfsPianoSetRailsAndPaths(&r,0));
        assert(board.vcc && board.vccq);
        off=1;
        assert(UfsPianoCloseRpmh(&r) && !board.open && !board.references && !board.power_reference && board.releases==1);
    }
    return 0;
}
