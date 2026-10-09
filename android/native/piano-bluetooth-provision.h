// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PIANO_BLUETOOTH_PROVISION_H
#define PIANO_BLUETOOTH_PROVISION_H
void piano_check_bluetooth_address(const char *factory);
void piano_provision_bluetooth(const char *boot, const char *factory);
#endif
