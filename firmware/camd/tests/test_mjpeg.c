/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * Host unit tests for mjpeg_ensure_dht (DHT insertion).
 */
#include "mjpeg.h"
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
	/* SOI, APP0, DQT, SOF0, SOS, scan, EOI — no DHT. */
	unsigned char in[] = { 0xFF,0xD8, 0xFF,0xE0,0,4,1,2, 0xFF,0xDB,0,4,3,4,
	                       0xFF,0xC0,0,4,5,6, 0xFF,0xDA,0,4,7,8, 0xAA,0xBB, 0xFF,0xD9 };
	unsigned char out[2048];
	size_t n, i;
	int dht_before = 0;

	n = mjpeg_ensure_dht(in, sizeof(in), out, sizeof(out));
	CHECK(n == sizeof(in) + 420);
	for (i = 0; i + 1 < n; i++) {
		if (out[i] == 0xFF && out[i + 1] == 0xDA) {        /* SOS */
			CHECK(i >= 420 && out[i - 420] == 0xFF && out[i - 419] == 0xC4);
			dht_before = 1;
			break;
		}
	}
	CHECK(dht_before);

	{ /* already has a DHT -> pass through (0) */
		unsigned char in2[] = { 0xFF,0xD8, 0xFF,0xC4,0,4,1,2, 0xFF,0xDA,0,4,7,8 };
		CHECK(mjpeg_ensure_dht(in2, sizeof(in2), out, sizeof(out)) == 0);
	}
	{ /* not a JPEG -> 0 */
		unsigned char in3[] = { 0,1,2,3 };
		CHECK(mjpeg_ensure_dht(in3, sizeof(in3), out, sizeof(out)) == 0);
	}
	CHECK(mjpeg_ensure_dht(in, sizeof(in), out, 100) == 0);  /* won't fit -> 0 */

	if (!fails) printf("test_mjpeg: all OK\n");
	return fails ? 1 : 0;
}
