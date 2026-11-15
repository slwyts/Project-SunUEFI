// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * FS/GPFS wire layouts are derived from the public MinkIPC listener headers.
 * Only the messages consumed by this backend are retained; this is a TA
 * protocol, not a new Linux kernel interface.
 */
#ifndef PIANO_GOODIX_STORAGE_PROTOCOL_H
#define PIANO_GOODIX_STORAGE_PROTOCOL_H

#include <stdint.h>

#define TZ_CM_MAX_NAME_LEN 256
#define MAX_READ_SIZE (64 * 1024)
#define GP_FS_VERSION 2
#define FILE_SERVICE_BUF_LEN (20 * 1024)
#define FILE_SERVICE_ID 0xa
#define GPFILE_SERVICE_BUF_LEN (504 * 1024)
#define GPFILE_SERVICE_ID 0x7000
#define TZ_GPFS_FILE_SIZE (500 * 1024)

typedef uint32_t tz_fs_msg_cmd_type;
typedef uint32_t tz_gpfs_msg_cmd_type;
enum {
	TZ_FS_MSG_CMD_FILE_OPEN = 0x202,
	TZ_FS_MSG_CMD_FILE_CLOSE = 0x209,
	TZ_FS_MSG_CMD_FILE_GET_ERRNO = 0x21c,
	TZ_GPFS_MSG_CMD_GPFS_VERSION = 12,
};

typedef struct __attribute__((packed)) {
	tz_fs_msg_cmd_type cmd_id;
	char pathname[TZ_CM_MAX_NAME_LEN];
	int32_t flags;
} tz_file_open_req_t;
typedef struct __attribute__((packed)) {
	tz_fs_msg_cmd_type cmd_id;
	int32_t ret;
} tz_file_open_rsp_t;
typedef struct __attribute__((packed)) {
	tz_fs_msg_cmd_type cmd_id;
	int32_t fd;
} tz_file_close_req_t;
typedef tz_file_open_rsp_t tz_file_close_rsp_t;
typedef tz_file_open_rsp_t tz_file_get_errno_rsp_t;
typedef struct __attribute__((packed)) {
	tz_gpfs_msg_cmd_type cmd_id;
	char pathname[TZ_CM_MAX_NAME_LEN];
	int32_t offset;
	uint32_t count;
} tz_gpfile_read_req_t;
typedef struct __attribute__((packed)) {
	tz_gpfs_msg_cmd_type cmd_id;
	int32_t err;
	uint32_t num_bytes_read;
	uint8_t buf[TZ_GPFS_FILE_SIZE];
} tz_gpfile_read_rsp_t;
typedef struct __attribute__((packed)) {
	tz_gpfs_msg_cmd_type cmd_id;
	char pathname[TZ_CM_MAX_NAME_LEN];
	int32_t offset;
	uint32_t count;
	uint32_t backup;
	uint8_t buf[TZ_GPFS_FILE_SIZE];
} tz_gpfile_write_req_t;
typedef struct __attribute__((packed)) {
	tz_gpfs_msg_cmd_type cmd_id;
	int32_t err;
	uint32_t num_bytes_written;
} tz_gpfile_write_rsp_t;
typedef struct __attribute__((packed)) {
	tz_gpfs_msg_cmd_type cmd_id;
	char pathname[TZ_CM_MAX_NAME_LEN];
} tz_gpfile_remove_req_t;
typedef struct __attribute__((packed)) {
	tz_gpfs_msg_cmd_type cmd_id;
	char from[TZ_CM_MAX_NAME_LEN];
	char to[TZ_CM_MAX_NAME_LEN];
} tz_gpfile_rename_req_t;
typedef struct __attribute__((packed)) {
	tz_gpfs_msg_cmd_type cmd_id;
	uint32_t version;
	int32_t err;
} tz_gpfile_version_rsp_t;

#endif
