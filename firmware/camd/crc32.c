/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * crc32 — table-driven IEEE 802.3 CRC-32.
 */

#include "crc32.h"

uint32_t crc32(const void *data, size_t len)
{
	static uint32_t table[256];
	static int built = 0;
	const unsigned char *p = data;
	uint32_t c = 0xFFFFFFFFu;
	size_t i;

	if (!built) {
		uint32_t n, k, v;
		for (n = 0; n < 256; n++) {
			v = n;
			for (k = 0; k < 8; k++)
				v = (v & 1) ? (0xEDB88320u ^ (v >> 1)) : (v >> 1);
			table[n] = v;
		}
		built = 1;
	}
	for (i = 0; i < len; i++)
		c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}
