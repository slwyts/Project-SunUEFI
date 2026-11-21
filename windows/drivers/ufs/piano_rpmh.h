/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "../rpmh/interface.h"
#include "piano_power.h"
#include "platform.h"

/* The shared GCC provider manages HBA bus/ICE/UniPro clocks separately
 * from QMP's reference and interface clocks. Neither includes the three
 * PHY lane clocks owned by this adapter. */
struct piano_ufs_parent {
    void *context;
    int (*base_clocks)(void *, int enable);
    int (*phy_clocks)(void *, int enable);
    /* Storage PM has flushed the medium, completed its device power-down
     * command, stopped background operations and placed the link OFF.
     * Return the confirmed storage PM state, including after clock gating;
     * HCI stopped, PHY stopped or system D3 by themselves are insufficient. */
    int (*media_may_power_off)(void *);
};

typedef struct {
    PIANO_RPMH_INTERFACE api;
    struct rpmh_client *client;
    struct piano_ufs_parent parent;
    unsigned held, phy_held;
    BOOLEAN owns_media;
    ULONG ddr_kBps, cfg_kBps;
} PIANO_UFS_RPMH;

/* Pass an interface obtained from the PnP device that actually owns this
 * board's APPS RPMh channel. Take a separate reference for the adapter. */
_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN UfsPianoOpenRpmh(PIANO_UFS_RPMH *, const PIANO_RPMH_INTERFACE *,
                         const struct piano_ufs_parent *, struct piano_ufs_bus_inputs *);
_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN UfsPianoCloseRpmh(PIANO_UFS_RPMH *);
_IRQL_requires_(PASSIVE_LEVEL)
int UfsPianoSetPhySupply(PIANO_UFS_RPMH *, enum ufs_phy_supply, int enable);
/* Apply the negotiated UniPro gear/lane count, never an advertised maximum.
 * At link startup hold the vendor discovery bandwidth until this succeeds. */
enum piano_ufs_link_mode { PianoUfsPwm, PianoUfsHsA, PianoUfsHsB };
_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN UfsPianoSetLinkBandwidth(PIANO_UFS_RPMH *, enum piano_ufs_link_mode,
                                 unsigned tx_gear, unsigned rx_gear, unsigned tx_lanes, unsigned rx_lanes);
