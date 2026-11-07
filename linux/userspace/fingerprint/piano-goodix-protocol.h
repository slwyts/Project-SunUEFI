/* SPDX-License-Identifier: BSD-3-Clause */
/* Request layouts of the stock `gfpia` TA and its QTEE file service, kept
 * free of QTEE calls so they can be tested on the host
 */
#ifndef PIANO_GOODIX_PROTOCOL_H
#define PIANO_GOODIX_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* From cmd_id_table (gf_strcmd, libgf_hal.so) */
enum goodix_command {
	GOODIX_DETECT_SENSOR = 1000,
	GOODIX_INIT = 1001,
	GOODIX_EXIT = 1002,
	GOODIX_INIT_FINISHED = 1005,
	GOODIX_PRE_ENROLL = 1006,
	GOODIX_ENROLL = 1007,
	GOODIX_CANCEL = 1009,
	GOODIX_AUTHENTICATE = 1010,
	GOODIX_SET_ACTIVE_GROUP = 1014,
	GOODIX_ENUMERATE = 1015,
	GOODIX_IRQ = 1016,
	GOODIX_AUTHENTICATE_FINISH = 1086,
	GOODIX_UPDATE_CONFIG = 1109,
	GOODIX_INIT_MODEL = 1111,
	GOODIX_TRANSPORT_DATA = 1113,
};

/* Every request starts with the 0x6c-byte gf_cmd_header_t */
#define GOODIX_HEADER_BYTES 0x6c
#define GOODIX_CANCEL_BYTES 0x70
#define GOODIX_AUTHENTICATE_BYTES 0x80
#define GOODIX_SET_ACTIVE_GROUP_BYTES 0x270
#define GOODIX_GROUP_PATH_BYTES 0x200
#define GOODIX_ENUMERATE_BYTES 0xc0
#define GOODIX_ENUMERATE_SLOTS 10
#define GOODIX_IRQ_BYTES 0xd134
#define GOODIX_AUTHENTICATE_FINISH_BYTES 0x148
#define GOODIX_TOKEN_OFFSET 0x2ac
#define GOODIX_TOKEN_BYTES 69

/* GF_IRQ_* bits, named by gf_strirq */
#define GOODIX_IRQ_FINGER_DOWN 0x2U
#define GOODIX_IRQ_FINGER_UP 0x4U
#define GOODIX_IRQ_IMAGE 0x80U
#define GOODIX_IRQ_ESD 0x800U
#define GOODIX_IRQ_FARGO_ERROR 0x10000U
#define GOODIX_IRQ_FARGO_UNRESOLVED 0x20000U
#define GOODIX_IRQ_RESET_FIRST 0x40000U
#define GOODIX_IRQ_RESET_FAILED 0x80000U
#define GOODIX_IRQ_NEEDS_RESET (GOODIX_IRQ_ESD | GOODIX_IRQ_FARGO_ERROR | \
	GOODIX_IRQ_FARGO_UNRESOLVED | GOODIX_IRQ_RESET_FIRST | GOODIX_IRQ_RESET_FAILED)

/* Processing step returned at IRQ offset 0x98 */
#define GOODIX_STEP_CAPTURE 2
#define GOODIX_STEP_REPEAT 4
#define GOODIX_STEP_CONTINUE 5

/* OPERATION_* values, named by gf_stroperation */
#define GOODIX_OPERATION_AUTHENTICATE_IMAGE 1
#define GOODIX_OPERATION_AUTHENTICATE_FF 2
#define GOODIX_OPERATION_RAW_DATA 0x2f

/* GF_ERROR_* codes, named by gf_strerror */
#define GOODIX_NOT_MATCH 1006
#define GOODIX_ACQUIRED_PARTIAL 1011
#define GOODIX_ACQUIRED_IMAGER_DIRTY 1012
#define GOODIX_TA_DEAD 1049
#define GOODIX_INVALID_HAT_VERSION 1055
#define GOODIX_INVALID_CHALLENGE 1056
#define GOODIX_UNTRUSTED_ENROLL 1057
#define GOODIX_SENSOR_BROKEN_CHECK_ALGO_ERROR 1060
#define GOODIX_MATCH_FAIL_AND_RETRY 1064
#define GOODIX_TOO_FAST 1094
#define GOODIX_ALGO_DIRTY_FINGER 1101
#define GOODIX_ALGO_COVER_BROKEN 1104
#define GOODIX_ALGO_PALM_DETECT 1110
#define GOODIX_TOO_SLOW 1117
#define GOODIX_ALGO_FAKE 1119

enum goodix_result {
	GOODIX_RESULT_NONE,
	GOODIX_RESULT_MATCH,
	GOODIX_RESULT_NO_MATCH,
	GOODIX_RESULT_RETRY,
	GOODIX_RESULT_PARTIAL,
	GOODIX_RESULT_DIRTY,
	GOODIX_RESULT_TOO_FAST,
	GOODIX_RESULT_TOO_SLOW,
	GOODIX_RESULT_PALM,
	GOODIX_RESULT_ERROR,
};

uint32_t goodix_get32(const uint8_t *buffer, size_t offset);
void goodix_put32(uint8_t *buffer, size_t offset, uint32_t value);

void goodix_authenticate_request(uint8_t *request, uint32_t group);
void goodix_irq_request(uint8_t *request);
void goodix_irq_capture_again(uint8_t *irq);
void goodix_authenticate_finish_request(uint8_t *request, const uint8_t *irq);
int goodix_set_active_group_request(uint8_t *request, uint32_t group,
				    const char *path);
enum goodix_result goodix_authenticate_result(uint32_t operation, int status);
const char *goodix_error_name(int status);

/* Legacy GPFS listener used by the TA's secure object storage */
#define GOODIX_FILE_SERVICE_ID 0x7000U
#define GOODIX_FILE_SERVICE_BYTES (504U * 1024U)
#define GOODIX_TA_OBJECT_PREFIX "/data/vendor_de/0/fpdata/"

/* The last refusal is kept so unexpected TA paths can be reported */
struct goodix_file_counts {
	unsigned int reads, writes, removes, renames, refused;
	uint32_t refused_command;
	char refused_path[257];
};

int goodix_file_object_name(const char *path, size_t capacity, const char **name);
int goodix_file_service(int directory, uint8_t *buffer, size_t size,
			struct goodix_file_counts *counts);

#endif
