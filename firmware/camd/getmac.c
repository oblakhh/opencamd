/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * getmac — print this unit's real Ethernet MAC, read from the flash HW-settings
 * block, so nothing is hardcoded (the firmware is portable across units).
 *
 * The MAC lives in the "COMPHS" block at flash 0x6000: a 12-byte
 * COMPRESS_MIB_HEADER_T { char sig[6]; u16 compRate; u32 compLen; } followed by
 * LZSS-compressed data (Okumura N=4096 F=18 THRESHOLD=2) that expands to the raw
 * HW_SETTING record stream. Within it, record id 0xC9 (u16-BE length 0x0006) is
 * the eth0 address. This mirrors tools/mib.py. Read-only; prints XX:XX:… or
 * nothing (exit non-zero) so rcS can fall back to the driver default.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define N 4096
#define F 18
#define THRESHOLD 2

static unsigned lzss_decode(const unsigned char *in, unsigned inlen,
                            unsigned char *out, unsigned outcap)
{
	unsigned char buf[N];
	unsigned o = 0, pos = 0, flags = 0;
	int r = N - F;
	memset(buf, ' ', N - F);
	memset(buf + N - F, 0, F);
	while (pos < inlen) {
		flags >>= 1;
		if (!(flags & 0x100)) {
			if (pos >= inlen) break;
			flags = (unsigned)in[pos++] | 0xFF00u;
		}
		if (flags & 1) {
			unsigned char c;
			if (pos >= inlen) break;
			c = in[pos++];
			if (o < outcap) out[o++] = c;
			buf[r] = c; r = (r + 1) & (N - 1);
		} else {
			int i, j, k;
			if (pos + 1 >= inlen) break;
			i = in[pos]; j = in[pos + 1]; pos += 2;
			i |= (j & 0xF0) << 4;
			j = (j & 0x0F) + THRESHOLD;
			for (k = 0; k <= j; k++) {
				unsigned char c = buf[(i + k) & (N - 1)];
				if (o < outcap) out[o++] = c;
				buf[r] = c; r = (r + 1) & (N - 1);
			}
		}
	}
	return o;
}

/* -rf: print the board's Wi-Fi RF calibration as "mib=hex" lines for
 * `iwpriv wlan0 set_mib`. Layout (verified on a real dump): "H601" + u16 len,
 * then records id(u16 BE) + len(u16 BE) + data; the WLAN block is record 0x80C9,
 * itself a record stream holding 14-byte per-channel arrays. */
static int print_rf(const unsigned char *raw, unsigned rawlen)
{
	static const struct { unsigned id; const char *mib; } want[] = {
		{ 0x0385, "pwrlevelCCK_A" }, { 0x0387, "pwrlevelHT40_1S_A" },
		{ 0x038a, "pwrdiffHT20" },   { 0x038b, "pwrdiffOFDM" },
	};
	unsigned o = 6, n = 0, i, j;
	const unsigned char *w = NULL;
	unsigned wlen = 0;

	while (o + 4 <= rawlen) {
		unsigned id = (raw[o] << 8) | raw[o + 1], len = (raw[o + 2] << 8) | raw[o + 3];
		if (o + 4 + len > rawlen)
			break;
		if (id == 0x80c9) { w = raw + o + 4; wlen = len; break; }
		o += 4 + len;
	}
	if (!w)
		return 2;
	for (o = 0; o + 4 <= wlen; ) {
		unsigned id = (w[o] << 8) | w[o + 1], len = (w[o + 2] << 8) | w[o + 3];
		if (o + 4 + len > wlen)
			break;
		for (i = 0; i < sizeof(want) / sizeof(want[0]); i++)
			if (id == want[i].id && len == 14) {
				printf("%s=", want[i].mib);
				for (j = 0; j < 14; j++)
					printf("%02x", w[o + 4 + j]);
				printf("\n");
				n++;
			}
		o += 4 + len;
	}
	return n ? 0 : 2;
}

int main(int argc, char **argv)
{
	unsigned char blk[0x2000];
	static unsigned char dec[0x8000];
	const unsigned char *raw;
	unsigned rawlen, k;
	int fd;
	int rf = argc > 1 && strcmp(argv[1], "-rf") == 0;
	const char *dev = argc > 1 + rf ? argv[1 + rf] : "/dev/mtdblock0";

	fd = open(dev, O_RDONLY);
	if (fd < 0)
		return 1;
	if (lseek(fd, 0x6000, SEEK_SET) != 0x6000 ||
	    read(fd, blk, sizeof(blk)) != (ssize_t)sizeof(blk)) {
		close(fd);
		return 1;
	}
	close(fd);

	if (memcmp(blk, "COMP", 4) == 0) {
		/* comp_len at offset 6 (LE u32), compressed data at offset 12 — matches
		 * tools/mib.py, which is verified against real dumps. */
		unsigned comp_len = (unsigned)blk[6] | ((unsigned)blk[7] << 8) |
		                    ((unsigned)blk[8] << 16) | ((unsigned)blk[9] << 24);
		if (comp_len > sizeof(blk) - 12)
			comp_len = sizeof(blk) - 12;
		rawlen = lzss_decode(blk + 12, comp_len, dec, sizeof(dec));
		raw = dec;
	} else {
		raw = blk;
		rawlen = sizeof(blk);
	}

	if (rf)
		return print_rf(raw, rawlen);

	/* eth0 record: id 0xC9, u16-BE length 0x0006, then 6 MAC bytes. */
	for (k = 0; k + 9 <= rawlen; k++) {
		if (raw[k] == 0xC9 && raw[k + 1] == 0x00 && raw[k + 2] == 0x06) {
			const unsigned char *m = raw + k + 3;
			/* Reject the all-zero / broadcast degenerate cases. */
			if ((m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) == 0)
				return 2;
			printf("%02X:%02X:%02X:%02X:%02X:%02X\n",
			       m[0], m[1], m[2], m[3], m[4], m[5]);
			return 0;
		}
	}
	return 2;   /* not found — caller keeps the driver default */
}
