/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * config_store — persist the config as JSON in the flash current-settings region
 * (0xC000, 16 KB) using two 8 KB A/B slots. Each save writes the inactive slot
 * (higher seq) and leaves the previous one intact until the new one is durable,
 * so a power loss mid-write never loses the last good config. Load picks the
 * valid-CRC slot with the highest sequence; if neither is valid, defaults.
 *
 * Slot layout: magic "CAMD" | u16 ver | u16 pad | u32 seq | u32 len | u32 crc32 | json[len]
 * (fixed 20-byte header; integers in native byte order — the store is always
 * read and written by the same machine).
 */
#ifndef CAMD_CONFIG_STORE_H
#define CAMD_CONFIG_STORE_H

#include "config.h"

/* JSON (de)serialization (also used by the /api/config endpoint). */
char *config_to_json(const config *c, int include_secrets); /* malloc'd; caller free() */
int config_from_json(config *c, const char *json, int allow_secrets); /* merges; 0 ok */

/* Flash persistence. dev defaults to /dev/mtd0 when NULL. */
int config_store_load(config *c, const char *dev);     /* 0 loaded, 1 defaults used, -1 error */
int config_store_save(const config *c, const char *dev); /* 0 ok, -1 error */
int config_store_erase(const char *dev);                 /* wipe to defaults; 0 ok */

#endif
