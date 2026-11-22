/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "../rpmh/interface.h"
#include "piano_power.h"
#include "platform.h"

typedef struct {
    PIANO_RPMH_INTERFACE api;
    struct rpmh_client *client;
    volatile LONG *media_off;
    unsigned held, phy_held;
    BOOLEAN owns_media, device_clock;
    BOOLEAN s0_reference;
    ULONG ddr_kBps, cfg_kBps;
} PIANO_UFS_RPMH;

/* Pass an interface obtained from the PnP device that actually owns this
 * board's APPS RPMh channel. Take a separate reference for the adapter. */
_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN UfsPianoOpenRpmh(PIANO_UFS_RPMH *, const PIANO_RPMH_INTERFACE *,
                         volatile LONG *media_off);
_IRQL_requires_(PASSIVE_LEVEL)
int UfsPianoSetRailsAndPaths(PIANO_UFS_RPMH *, int enable);
_IRQL_requires_(PASSIVE_LEVEL)
int UfsPianoDeviceReference(PIANO_UFS_RPMH *, int enable);
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
BOOLEAN UfsPianoDiscoveryBandwidth(PIANO_UFS_RPMH *);
BOOLEAN UfsPianoHasS0Power(const PIANO_UFS_RPMH *);
