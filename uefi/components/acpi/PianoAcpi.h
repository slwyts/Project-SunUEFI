// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PIANO_ACPI_H_
#define PIANO_ACPI_H_

#include <IndustryStandard/Acpi63.h>

#define PIANO_ACPI_HEADER(Signature, TableType, Revision)           \
  { Signature, sizeof (TableType), Revision, 0,                      \
    { 'S', 'U', 'N', 'U', 'E', 'F' },                               \
    SIGNATURE_64 ('P', 'I', 'A', 'N', 'O', ' ', ' ', ' '), 1,        \
    SIGNATURE_32 ('S', 'U', 'E', 'F'), 1 }

#endif
