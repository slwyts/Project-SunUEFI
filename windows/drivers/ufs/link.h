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
    UFS_LINK_READY, UFS_LINK_FAILED, UFS_LINK_STOPPED,
    UFS_LINK_GET_PARAMETERS, UFS_LINK_WAIT_PARAMETER,
    UFS_LINK_ENTER_HIBERN8, UFS_LINK_WAIT_HIBERN8, UFS_LINK_STOP_HOST, UFS_LINK_WAIT_OFF, UFS_LINK_OFF,
    UFS_LINK_KEEP_POWER, UFS_LINK_ASLEEP, UFS_LINK_EXIT_HIBERN8, UFS_LINK_RESETTING
};
enum ufs_pa_parameter { UfsPaPowerMode, UfsPaTxGear, UfsPaRxGear, UfsPaTxLanes, UfsPaRxLanes,
                        UfsPaHsSeries, UfsPaParameterCount };
enum ufs_link_error {
    UFS_LINK_OK, UFS_LINK_RESOURCES, UFS_LINK_OWNED, UFS_LINK_CONTROLLER,
    UFS_LINK_TIMEOUT, UFS_LINK_UIC, UFS_LINK_OCS, UFS_LINK_REJECTED, UFS_LINK_PROTOCOL
};
enum ufs_management_command {
    UFS_MGMT_NOP, UFS_MGMT_SET_INIT, UFS_MGMT_READ_INIT, UFS_MGMT_DEVICE,
    UFS_MGMT_GEOMETRY, UFS_MGMT_UNIT, UFS_MGMT_REPORT_LUNS,
    UFS_MGMT_FLUSH, UFS_MGMT_READ_BKOPS, UFS_MGMT_CLEAR_BKOPS, UFS_MGMT_CHECK_BKOPS,
    UFS_MGMT_POWER_DOWN, UFS_MGMT_CHECK_DOWN, UFS_MGMT_READ_MODE, UFS_MGMT_ACTIVE,
    UFS_MGMT_CHECK_ACTIVE, UFS_MGMT_RESTORE_BKOPS, UFS_MGMT_CHECK_RESTORED, UFS_MGMT_CHECK_POWER_ON_WP,
    UFS_MGMT_VERIFY_POWER_ON_WP
};
/* Preserve the device's background-work policy and the host's automatic
 * Hibern8 timer across D3, including an interrupted transition. */
struct ufs_media_settings {
    uint32_t ahit;
    uint8_t restore_ahit, restore_bkops, check_power_on_wp, verify_power_on_wp;
};
enum ufs_link_operation { UFS_DISCOVER, UFS_POWER_OFF, UFS_SLEEP, UFS_WAKE };
struct ufs_link {
    struct ufs_link_io io;
    enum ufs_link_phase phase;
    enum ufs_link_error error;
    enum ufs_management_command command;
    uint64_t since, init_since;
    uint32_t enabled_luns, reported_luns, previous_ie, fault_interrupts;
    uint16_t specification, manufacturer;
    uint8_t lun_count, lun_capacity, unit, query_status, ocs, uic_result, nop_attempts;
    unsigned cold, owns_controller, owns_list, uic_inflight, transfer_inflight, stopping, list_stop_issued;
    uint8_t read_parameters, parameter_count, parameter[UfsPaParameterCount];
    struct ufs_media_settings *media;
    enum ufs_link_operation operation;
    uint32_t flush_luns;
    uint64_t command_since;
};

/* Caller serializes management with the ISR, D-state and regular SRBs.
 * A poll takes one bounded step and does not stall. now_us is monotonic.
 * A terminal result may still own DMA; always stop before releasing it.
 */
enum ufs_link_error ufs_link_begin(struct ufs_link *link, const struct ufs_link_io *io, uint64_t now_us);
/* Transfer management uses only the adapter's common buffers after normal
 * I/O has drained. ASLEEP keeps HCE, PHY and device rails; wake it with
 * UFS_WAKE. Only OFF allows the platform to remove power. Restore OFF with
 * normal link startup and the saved media policy. */
enum ufs_link_error ufs_link_media(struct ufs_link *link, const struct ufs_link_io *io,
                                  struct ufs_media_settings *media, uint32_t enabled_luns,
                                  enum ufs_link_operation operation, uint64_t now_us);
enum ufs_link_phase ufs_link_poll(struct ufs_link *link, uint64_t now_us);
int ufs_link_stop(struct ufs_link *link);
/* A lost UIC event cannot prevent an HCI reset. Keep all DMA ownership
 * until the disabled HCE and both stopped lists/doorbells have been read. */
int ufs_link_reset(struct ufs_link *link);

/* Resolve the Windows/SCSI LUN into the address byte used by UFS UPIUs.
 * REPORT LUNS is sent to the device's standard report-LUNs well-known LU,
 * also when target discovery began at a non-existent ordinary LUN zero.
 */
int ufs_scsi_lun(uint32_t enabled, uint8_t requested, const uint8_t *cdb, size_t bytes, uint8_t *wire_lun);
