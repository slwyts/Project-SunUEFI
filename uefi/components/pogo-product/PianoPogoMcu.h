// SPDX-License-Identifier: BSD-2-Clause-Patent
// Byte layouts of the WN8030 bootrom download and runtime commands; no
// hardware or Boot Services access
#pragma once
#include <Uefi.h>

#define PIANO_POGO_MCU_ADDRESS 0x4CU
#define PIANO_POGO_BOOT_REGISTER 0x5CU
#define PIANO_POGO_RUNTIME_REGISTER 0x4CU
#define PIANO_POGO_BOOT_ID 0xC8U
#define PIANO_POGO_IMAGE_HEADER_BYTES 64U
#define PIANO_POGO_HEADER_WIRE_BYTES 66U   // register + 65-byte header command
#define PIANO_POGO_DATA_CHUNK 503U
#define PIANO_POGO_DATA_WIRE_BYTES 513U    // register + 512-byte data command
#define PIANO_POGO_COMMAND_WIRE_BYTES 67U  // register + 66-byte runtime command
#define PIANO_POGO_REPORT_BYTES 68U
#define PIANO_POGO_MAX_CODE_BYTES 0x10000U

// Runtime commands to the MCU (Linux hid-nanosic-wn8030.c)
#define PIANO_POGO_CMD_TOUCHPAD 0x21U
#define PIANO_POGO_CMD_BACKLIGHT 0x23U
#define PIANO_POGO_CMD_KEYBOARD_POWER 0x25U
#define PIANO_POGO_CMD_INDICATOR 0x2EU
#define PIANO_POGO_LED_CAPS_ON 0xFDU
#define PIANO_POGO_LED_CAPS_OFF 0xFCU

// Validates the image header (at file offset 0x7FC0) against the RAM code
// (file offset 0x8000) and builds "0x5C, 0x06, header..." with the 32-bit code
// sum and 8-bit header sum the bootrom checks
EFI_STATUS PianoPogoMcuHeaderCommand(IN CONST UINT8 *ImageHeader,IN CONST UINT8 *Code,IN UINT32 CodeBytes,
                                     OUT UINT8 Wire[PIANO_POGO_HEADER_WIRE_BYTES]);
// "0x5C, 0x07, offset (BE32), length (BE32), up to 503 bytes, zero padding".
// Returns the number of code bytes in this packet, 0 at the end
UINT32 PianoPogoMcuDataCommand(IN CONST UINT8 *Code,IN UINT32 CodeBytes,IN UINT32 Offset,
                               OUT UINT8 Wire[PIANO_POGO_DATA_WIRE_BYTES]);
// "0x5C, Command": bootrom requests 0x04 (ID), 0x05 (state), 0x08 (verify), 0x09 (start)
VOID PianoPogoMcuBootCommand(IN UINT8 Command,OUT UINT8 Wire[2]);
// "0x4C, 32 00 4E 31 80 38 cmd 01 value sum ...", 66 bytes after the register
VOID PianoPogoMcuRuntimeCommand(IN UINT8 Command,IN UINT8 Value,OUT UINT8 Wire[PIANO_POGO_COMMAND_WIRE_BYTES]);
// A runtime read frame carries reports only when it starts with 0x57 and its
// third byte is non-zero; Linux drops every other frame
BOOLEAN PianoPogoMcuFrameHasReports(IN CONST UINT8 Frame[PIANO_POGO_REPORT_BYTES]);
