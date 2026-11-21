/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum rpmh_accelerator { RPMH_ARC = 3, RPMH_VRM = 4, RPMH_BCM = 5 };

/* A read-only snapshot of the command database published by the AOP. */
struct rpmh_cmd_db { const uint8_t *bytes; size_t size; };
struct rpmh_db_entry { uint32_t address; const uint8_t *aux; size_t aux_size; };

int rpmh_db_open(struct rpmh_cmd_db *db, const uint8_t *bytes, size_t size);
int rpmh_db_find(const struct rpmh_cmd_db *db, const char *name, enum rpmh_accelerator type,
                 struct rpmh_db_entry *out);
int rpmh_arc_corner(const struct rpmh_db_entry *entry, uint32_t level, uint32_t *corner);
