/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "transfer.h"

enum { UFS_DESCRIPTOR_MAX = 255, UFS_MANAGEMENT_RESPONSE = 288, UFS_MAX_UNIT_NUMBERS = 32 };
/* 128-byte-aligned UCD. A descriptor Query response carries its payload at
 * byte 32 of the returned UPIU, not in the SCSI scatter/gather list.
 */
struct ufs_management_ucd {
    union {
        struct { uint8_t request[UFS_UPIU_SPACE], response[UFS_MANAGEMENT_RESPONSE]; };
        struct ufs_upius scsi;
    };
    struct ufs_prd report_prd;
    uint8_t lun_list[8 + 8 * UFS_MAX_UNIT_NUMBERS];
};
struct ufs_link_io {
    void *context;
    uint32_t (*read)(void *, unsigned);
    void (*write)(void *, unsigned, uint32_t);
    void (*publish)(void *);
    void (*consume)(void *);
    struct ufs_utrd *list;
    struct ufs_management_ucd *ucd;
    uint64_t list_dma, ucd_dma;
};
enum ufs_link_phase {
    UFS_LINK_IDLE, UFS_LINK_WAIT_HCE, UFS_LINK_WAIT_UIC, UFS_LINK_WAIT_STARTUP,
    UFS_LINK_WAIT_PRESENT, UFS_LINK_SEND, UFS_LINK_WAIT_REPLY, UFS_LINK_INIT_POLL,
    UFS_LINK_READY, UFS_LINK_FAILED, UFS_LINK_STOPPED
};
enum ufs_link_error {
    UFS_LINK_OK, UFS_LINK_RESOURCES, UFS_LINK_OWNED, UFS_LINK_CONTROLLER,
    UFS_LINK_TIMEOUT, UFS_LINK_UIC, UFS_LINK_OCS, UFS_LINK_REJECTED, UFS_LINK_PROTOCOL
};
enum ufs_management_command {
    UFS_MGMT_NOP, UFS_MGMT_SET_INIT, UFS_MGMT_READ_INIT, UFS_MGMT_DEVICE,
    UFS_MGMT_GEOMETRY, UFS_MGMT_UNIT, UFS_MGMT_REPORT_LUNS
};
struct ufs_link {
    struct ufs_link_io io;
    enum ufs_link_phase phase;
    enum ufs_link_error error;
    enum ufs_management_command command;
    uint64_t since, init_since;
    uint32_t enabled_luns, reported_luns, previous_ie, fault_interrupts;
    uint16_t specification, manufacturer;
    uint8_t lun_count, lun_capacity, unit, query_status, ocs, uic_result, nop_attempts;
    unsigned cold, owns_controller, owns_list, uic_inflight, transfer_inflight, stopping;
};

/* Caller serializes management with the ISR, D-state and regular SRBs.
 * A poll takes one bounded step and does not stall. now_us is monotonic.
 * A terminal result may still own DMA; always stop before releasing it.
 */
enum ufs_link_error ufs_link_begin(struct ufs_link *link, const struct ufs_link_io *io, uint64_t now_us);
enum ufs_link_phase ufs_link_poll(struct ufs_link *link, uint64_t now_us);
int ufs_link_stop(struct ufs_link *link);
