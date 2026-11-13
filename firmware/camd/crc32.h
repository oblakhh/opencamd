/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * crc32 — IEEE 802.3 CRC-32, used to check config store slots.
 */

#ifndef CAMD_CRC32_H
#define CAMD_CRC32_H
#include <stddef.h>
#include <stdint.h>
uint32_t crc32(const void *data, size_t len); /* standard IEEE 802.3 CRC-32 */
#endif
