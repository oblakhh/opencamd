/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * Host tests for wpa.c: standard vectors, a full synthetic 4-way + group
 * rekey against an AP implemented here, and one negative case per finding
 * of the supplicant review (docs/engineering-notes.md).
 */
#include "wpa.h"
#include "wpa_drv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/aes.h"
#include "mbedtls/md.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void hex(const char *h, unsigned char *out, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		unsigned v;
		sscanf(h + 2 * i, "%2x", &v);
		out[i] = (unsigned char)v;
	}
}

static int eqhex(const unsigned char *b, const char *h)
{
	unsigned char t[128];
	size_t n = strlen(h) / 2;
	hex(h, t, n);
	return memcmp(b, t, n) == 0;
}

/* ---- vectors ------------------------------------------------------------ */

static void t_pmk(void)
{
	unsigned char pmk[32];
	/* IEEE 802.11-2016 Annex J.4.2 */
	CHECK(wpa_pmk("password", (const unsigned char *)"IEEE", 4, pmk) == 0);
	CHECK(eqhex(pmk, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"));
	CHECK(wpa_pmk("ThisIsAPassword", (const unsigned char *)"ThisIsASSID", 11, pmk) == 0);
	CHECK(eqhex(pmk, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af"));
	CHECK(wpa_pmk("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
	              (const unsigned char *)"ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ", 32, pmk) == 0);
	CHECK(eqhex(pmk, "becb93866bb8c3832cb777c2f559807c8c59afcb6eae734885001300a981cc62"));
	/* B5: invalid input is an error and never yields a usable (zero) PMK */
	memset(pmk, 0x55, sizeof(pmk));
	CHECK(wpa_pmk("short", (const unsigned char *)"IEEE", 4, pmk) == -1);
	CHECK(eqhex(pmk, "0000000000000000000000000000000000000000000000000000000000000000"));
	CHECK(wpa_pmk("0123456789012345678901234567890123456789012345678901234567890123",
	              (const unsigned char *)"IEEE", 4, pmk) == -1);   /* 64 chars */
	CHECK(wpa_pmk("password", (const unsigned char *)"x", 0, pmk) == -1);
	CHECK(wpa_pmk("password", (const unsigned char *)"012345678901234567890123456789012", 33, pmk) == -1);
	CHECK(wpa_pmk("pass\tword", (const unsigned char *)"IEEE", 4, pmk) == -1);
	CHECK(wpa_pmk(NULL, (const unsigned char *)"IEEE", 4, pmk) == -1);
}

static void t_prf(void)
{
	unsigned char k[20], o[64];
	/* IEEE 802.11-2016 Annex J.3 (PRF test vectors), test case 1 and 2 */
	memset(k, 0x0b, 20);
	CHECK(wpa_prf(k, 20, "prefix", (const unsigned char *)"Hi There", 8, o, 64) == 0);
	CHECK(eqhex(o, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
	               "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a"));
	CHECK(wpa_prf((const unsigned char *)"Jefe", 4, "prefix-2",
	              (const unsigned char *)"what do ya want for nothing?", 28, o, 64) == 0);
	CHECK(eqhex(o, "47c4908e30c947521ad20be9053450ecbea23d3aa604b77326d8b3825ff7475c"
	               "06f51fb9c5313d1e9f90d897d134b72e090fc23150bc8414382043418678e700"));
	/* PTK = PRF-384; cross-checked with Python's hmac/hashlib (independent code) */
	{
		unsigned char pmk[32], aa[6], spa[6], an[32], sn[32], ptk[48];
		int i;
		hex("0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af", pmk, 32);
		hex("a0a1a1a3a4a5", aa, 6);
		hex("b0b1b2b3b4b5", spa, 6);
		for (i = 0; i < 32; i++) { an[i] = (unsigned char)(0xe0 + i); sn[i] = (unsigned char)(0xc0 + i); }
		CHECK(wpa_ptk(pmk, aa, spa, an, sn, ptk) == 0);
		CHECK(eqhex(ptk, "e79498d36bd81b9b41819b48f3c97f966e620972f3d263238df571d65fa2b8fa"
		                 "d503e8b5891f81c74422468fc2d4dd39"));
		/* argument order of AA/SPA and nonces does not matter (min/max) */
		{
			unsigned char p2[48];
			CHECK(wpa_ptk(pmk, spa, aa, sn, an, p2) == 0 && memcmp(ptk, p2, 48) == 0);
		}
	}
}

/* RFC 3394 section 4 */
static void t_unwrap(void)
{
	static const struct { const char *kek, *pt, *ct; } v[] = {
		{ "000102030405060708090A0B0C0D0E0F", "00112233445566778899AABBCCDDEEFF",
		  "1FA68B0A8112B447AEF34BD8FB5A7B829D3E862371D2CFE5" },
		{ "000102030405060708090A0B0C0D0E0F1011121314151617", "00112233445566778899AABBCCDDEEFF",
		  "96778B25AE6CA435F92B5B97C050AED2468AB8A17AD84E5D" },
		{ "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", "00112233445566778899AABBCCDDEEFF",
		  "64E8C3F9CE0F5BA263E9777905818A2A93C8191E7D6E8AE7" },
		{ "000102030405060708090A0B0C0D0E0F1011121314151617", "00112233445566778899AABBCCDDEEFF0001020304050607",
		  "031D33264E15D33268F24EC260743EDCE1C6C7DDEE725A936BA814915C6762D2" },
		{ "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", "00112233445566778899AABBCCDDEEFF0001020304050607",
		  "A8F9BC1612C68B3FF6E6F4FBE30E71E4769C8B80A32CB8958CD5D17D6B254DA1" },
		{ "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F",
		  "00112233445566778899AABBCCDDEEFF000102030405060708090A0B0C0D0E0F",
		  "28C9F404C4B810F4CBCCB35CFB87F8263F5786E2D80ED326CBC7F0E71A99F43BFB988B9B7A02DD21" },
	};
	size_t i;
	for (i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		unsigned char kek[32], ct[40], out[40], pt[32];
		size_t kl = strlen(v[i].kek) / 2, cl = strlen(v[i].ct) / 2;
		hex(v[i].kek, kek, kl);
		hex(v[i].ct, ct, cl);
		hex(v[i].pt, pt, cl - 8);
		CHECK(wpa_aes_unwrap(kek, kl, ct, cl, out) == 0 && memcmp(out, pt, cl - 8) == 0);
		ct[3] ^= 1;   /* integrity check must fail, output zeroed */
		CHECK(wpa_aes_unwrap(kek, kl, ct, cl, out) == -1 && out[0] == 0 && out[cl - 9] == 0);
	}
	{
		unsigned char k[16] = { 0 }, b[40] = { 0 };
		CHECK(wpa_aes_unwrap(k, 16, b, 16, b) == -1);   /* too short */
		CHECK(wpa_aes_unwrap(k, 16, b, 28, b) == -1);   /* not a multiple of 8 */
		CHECK(wpa_aes_unwrap(k, 15, b, 24, b) == -1);   /* bad KEK size */
	}
}

/* ---- AP side ------------------------------------------------------------ */

static void aes_wrap(const unsigned char *kek, const unsigned char *in, size_t n8, unsigned char *out)
{
	mbedtls_aes_context a;
	unsigned char b[16];
	size_t i, n = n8 / 8;
	int j;
	memset(out, 0xa6, 8);
	memcpy(out + 8, in, n8);
	mbedtls_aes_init(&a);
	mbedtls_aes_setkey_enc(&a, kek, 128);
	for (j = 0; j <= 5; j++)
		for (i = 1; i <= n; i++) {
			unsigned long t = (unsigned long)(n * j + i);
			int k;
			memcpy(b, out, 8);
			memcpy(b + 8, out + 8 * i, 8);
			mbedtls_aes_crypt_ecb(&a, MBEDTLS_AES_ENCRYPT, b, b);
			for (k = 7; k >= 0; k--, t >>= 8)
				b[k] ^= (unsigned char)(t & 0xff);
			memcpy(out, b, 8);
			memcpy(out + 8 * i, b + 8, 8);
		}
	mbedtls_aes_free(&a);
}

static void hmac_sha1(const unsigned char *k, size_t kl, const unsigned char *d, size_t dl, unsigned char out[20])
{
	mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), k, kl, d, dl, out);
}

typedef struct {
	unsigned char pmk[32], aa[6], spa[6], anonce[32], snonce[32], ptk[48];
	unsigned long long rc;
	unsigned char gtk[16];
	int gtk_idx;
	int eapol_ver;
} ap_t;

static const unsigned char AA[6] = { 0x08, 0xb6, 0x57, 0xb4, 0x2d, 0x69 };
static const unsigned char SPA[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
/* RSNE of a WPA2/WPA3 transition AP: PSK + SAE, CCMP, MFPC */
static const char *AP_RSNE = "30180100000fac040100000fac040200000fac02000fac088000";
static const char *RSNXE = "f40120";            /* SAE-H2E */

static size_t ap_frame(ap_t *ap, unsigned char *f, unsigned ki, const unsigned char *nonce,
                       const unsigned char *kd, size_t kdlen, int mic)
{
	size_t flen = WPA_OFF_KD + kdlen;
	int i;
	memset(f, 0, flen);
	f[0] = (unsigned char)ap->eapol_ver;
	f[1] = 3;
	f[2] = (unsigned char)((flen - 4) >> 8);
	f[3] = (unsigned char)(flen - 4);
	f[4] = 2;
	f[5] = (unsigned char)(ki >> 8);
	f[6] = (unsigned char)ki;
	f[8] = 16;
	ap->rc++;
	for (i = 0; i < 8; i++)
		f[WPA_OFF_RC + i] = (unsigned char)(ap->rc >> (8 * (7 - i)));
	if (nonce)
		memcpy(f + WPA_OFF_NONCE, nonce, 32);
	f[WPA_OFF_RSC] = 0x05;                    /* GTK RSC */
	f[WPA_OFF_KDLEN] = (unsigned char)(kdlen >> 8);
	f[WPA_OFF_KDLEN + 1] = (unsigned char)kdlen;
	if (kdlen)
		memcpy(f + WPA_OFF_KD, kd, kdlen);
	if (mic) {
		unsigned char h[20];
		hmac_sha1(ap->ptk, 16, f, flen, h);
		memcpy(f + WPA_OFF_MIC, h, 16);
	}
	return flen;
}

static size_t ap_msg1(ap_t *ap, unsigned char *f, const unsigned char *kd, size_t kdlen)
{
	return ap_frame(ap, f, 0x008a, ap->anonce, kd, kdlen, 0);
}

/* Verify the STA's msg-2 like an AP would; derives the PTK. 1 = MIC ok. */
static int ap_check_msg2(ap_t *ap, const unsigned char *m, size_t len)
{
	unsigned char t[256], h[20], ie[22];
	if (len > sizeof(t) || len < WPA_OFF_KD)
		return 0;
	memcpy(ap->snonce, m + WPA_OFF_NONCE, 32);
	wpa_ptk(ap->pmk, ap->aa, ap->spa, ap->anonce, ap->snonce, ap->ptk);
	memcpy(t, m, len);
	memset(t + WPA_OFF_MIC, 0, 16);
	hmac_sha1(ap->ptk, 16, t, len, h);
	wpa_rsn_ie(ie);
	return memcmp(h, m + WPA_OFF_MIC, 16) == 0 &&
	       ((m[5] << 8) | m[6]) == 0x010a && m[4] == 2 &&
	       ((m[WPA_OFF_KDLEN] << 8) | m[WPA_OFF_KDLEN + 1]) == 22 && memcmp(m + WPA_OFF_KD, ie, 22) == 0;
}

static int ap_check_mic(ap_t *ap, const unsigned char *m, size_t len, unsigned ki)
{
	unsigned char t[256], h[20];
	if (len > sizeof(t) || len < WPA_OFF_KD)
		return 0;
	memcpy(t, m, len);
	memset(t + WPA_OFF_MIC, 0, 16);
	hmac_sha1(ap->ptk, 16, t, len, h);
	return memcmp(h, m + WPA_OFF_MIC, 16) == 0 && ((unsigned)(m[5] << 8) | m[6]) == ki;
}

/* plaintext key data -> padded, wrapped */
static size_t ap_wrap(ap_t *ap, const unsigned char *pt, size_t n, unsigned char *out)
{
	unsigned char b[600];
	size_t p = n;
	memcpy(b, pt, n);
	if (p % 8 || p < 16) {
		b[p++] = 0xdd;
		while (p % 8 || p < 16)
			b[p++] = 0;
	}
	aes_wrap(ap->ptk + 16, b, p, out);
	return p + 8;
}

static size_t gtk_kde(unsigned char *o, int idx, const unsigned char *gtk)
{
	o[0] = 0xdd; o[1] = 22; o[2] = 0x00; o[3] = 0x0f; o[4] = 0xac; o[5] = 1;
	o[6] = (unsigned char)idx; o[7] = 0;
	memcpy(o + 8, gtk, 16);
	return 24;
}

static size_t igtk_kde(unsigned char *o)
{
	size_t i;
	o[0] = 0xdd; o[1] = 28; o[2] = 0x00; o[3] = 0x0f; o[4] = 0xac; o[5] = 9;
	for (i = 6; i < 30; i++) o[i] = (unsigned char)i;
	return 30;
}

/* msg-3 Key Data like the FRITZ! mesh sends it: RSNE, RSNXE, GTK KDE, IGTK KDE */
static size_t ap_msg3(ap_t *ap, unsigned char *f, int rsnxe, int igtk)
{
	unsigned char pt[200], w[220];
	size_t n = 0;
	hex(AP_RSNE, pt, strlen(AP_RSNE) / 2);
	n += strlen(AP_RSNE) / 2;
	if (rsnxe) { hex(RSNXE, pt + n, 3); n += 3; }
	n += gtk_kde(pt + n, ap->gtk_idx, ap->gtk);
	if (igtk) n += igtk_kde(pt + n);
	n = ap_wrap(ap, pt, n, w);
	return ap_frame(ap, f, 0x13ca, ap->anonce, w, n, 1);
}

static size_t ap_group1(ap_t *ap, unsigned char *f)
{
	unsigned char pt[64], w[80];
	size_t n = gtk_kde(pt, ap->gtk_idx, ap->gtk);
	n = ap_wrap(ap, pt, n, w);
	return ap_frame(ap, f, 0x1382, NULL, w, n, 1);
}

/* deterministic "DRBG" for the tests: counts calls so SNonces differ */
static int rng_calls;
static int test_rng(void *ctx, unsigned char *b, size_t n)
{
	size_t i;
	(void)ctx;
	rng_calls++;
	for (i = 0; i < n; i++)
		b[i] = (unsigned char)(0x40 + rng_calls * 7 + i);
	return 0;
}
static int bad_rng(void *ctx, unsigned char *b, size_t n)
{
	(void)ctx; (void)b; (void)n;
	return -1;
}

static void setup(ap_t *ap, wpa_sm *sm, const char *sta_pass)
{
	unsigned char pmk[32];
	int i;
	memset(ap, 0, sizeof(*ap));
	wpa_pmk("correct horse battery", (const unsigned char *)"example-ssid", 12, ap->pmk);
	memcpy(ap->aa, AA, 6);
	memcpy(ap->spa, SPA, 6);
	for (i = 0; i < 32; i++) ap->anonce[i] = (unsigned char)(0x90 + i);
	for (i = 0; i < 16; i++) ap->gtk[i] = (unsigned char)(0x10 + i);
	ap->gtk_idx = 1;
	ap->eapol_ver = 2;
	ap->rc = 0;
	wpa_pmk(sta_pass, (const unsigned char *)"example-ssid", 12, pmk);
	wpa_init(sm, pmk, SPA, test_rng, NULL);
	wpa_assoc(sm, AA);
}

/* one complete 4-way; returns 1 if every step checked out */
static int handshake(ap_t *ap, wpa_sm *sm, int rsnxe, int igtk)
{
	unsigned char f[600];
	size_t n;
	wpa_out o;
	int ok = 1;
	n = ap_msg1(ap, f, NULL, 0);
	ok &= wpa_rx(sm, AA, f, n, &o) == 0 && o.msg == 1 && o.tx_len > 0;
	ok &= ap_check_msg2(ap, o.tx, o.tx_len);
	n = ap_msg3(ap, f, rsnxe, igtk);
	ok &= wpa_rx(sm, AA, f, n, &o) == 0 && o.msg == 3;
	ok &= ap_check_mic(ap, o.tx, o.tx_len, 0x030a);                 /* msg-4 */
	ok &= o.set_ptk && memcmp(o.tk, ap->ptk + 32, 16) == 0 && !o.ptk_rekey;
	ok &= o.set_gtk && o.gtk_idx == ap->gtk_idx && memcmp(o.gtk, ap->gtk, 16) == 0;
	ok &= o.rsc[0] == 0x05;
	ok &= o.open_port && sm->state == WPA_DONE;
	return ok;
}

/* ---- scenarios ---------------------------------------------------------- */

static void t_full(void)
{
	ap_t ap;
	wpa_sm sm;
	wpa_out o;
	unsigned char f[600];
	size_t n;

	/* plain WPA2 AP (no RSNXE) */
	setup(&ap, &sm, "correct horse battery");
	CHECK(handshake(&ap, &sm, 0, 0));
	CHECK(sm.st.done == 1 && sm.st.gtk_installs == 1 && sm.st.drops == 0);

	/* A1 (the production bug): RSNXE between RSNE and GTK KDE, IGTK KDE after it */
	setup(&ap, &sm, "correct horse battery");
	CHECK(handshake(&ap, &sm, 1, 1));

	/* group key rekey: new GTK with the other index, RSC passed through */
	ap.gtk_idx = 2;
	ap.gtk[0] ^= 0xff;
	n = ap_group1(&ap, f);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == 0 && o.msg == 5);
	CHECK(ap_check_mic(&ap, o.tx, o.tx_len, 0x0302));   /* A2: echoed version 2, MIC|Secure */
	CHECK(o.tx[4] == 2 && o.tx_len == WPA_OFF_KD);
	CHECK(o.set_gtk && o.gtk_idx == 2 && memcmp(o.gtk, ap.gtk, 16) == 0 && !o.set_ptk && !o.open_port);
	CHECK(sm.st.gtk_rekeys == 1);
	/* B3 (group): the same GTK again (AP retransmission, higher counter) -> reply, no reinstall */
	n = ap_group1(&ap, f);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == 0 && o.msg == 5 && !o.set_gtk && o.tx_len > 0);
	/* A6: replay of that exact frame -> dropped */
	CHECK(wpa_rx(&sm, AA, f, n, &o) == -1 && o.tx_len == 0);
	CHECK(strstr(sm.last_drop, "replay") != NULL);

	/* PTK rekey within the association: new 4-way, new PTK installed, GTK unchanged */
	ap.anonce[0] ^= 0x55;
	n = ap_msg1(&ap, f, NULL, 0);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == 0 && o.msg == 1 && ap_check_msg2(&ap, o.tx, o.tx_len));
	n = ap_msg3(&ap, f, 1, 1);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == 0 && o.set_ptk && memcmp(o.tk, ap.ptk + 32, 16) == 0 && !o.set_gtk);
	CHECK(o.ptk_rekey == 1 && !o.open_port && sm.state == WPA_DONE && !sm.rekey);   /* M2 flag */
	CHECK(sm.st.done == 2);

	/* EAPOL version echo: an 802.1X-2001 AP gets version 1 back */
	setup(&ap, &sm, "correct horse battery");
	ap.eapol_ver = 1;
	n = ap_msg1(&ap, f, NULL, 0);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == 0 && o.tx[0] == 1);
}

static void t_retransmit_msg3(void)
{
	ap_t ap;
	wpa_sm sm;
	wpa_out o;
	unsigned char f[600];
	size_t n;
	/* B3/KRACK: msg-3 retransmitted (msg-4 lost) -> msg-4 again, no key reinstall */
	setup(&ap, &sm, "correct horse battery");
	CHECK(handshake(&ap, &sm, 1, 0));
	n = ap_msg3(&ap, f, 1, 0);          /* same ANonce, counter + 1, valid MIC */
	CHECK(wpa_rx(&sm, AA, f, n, &o) == 0 && o.msg == 3 && o.tx_len > 0);
	CHECK(ap_check_mic(&ap, o.tx, o.tx_len, 0x030a));
	CHECK(!o.set_ptk && !o.set_gtk && !o.open_port);
	CHECK(sm.st.msg3_dup == 1 && sm.st.done == 1);
	/* A6: the identical frame replayed -> dropped */
	CHECK(wpa_rx(&sm, AA, f, n, &o) == -1 && o.tx_len == 0);
}

static void t_msg1_extras(void)
{
	ap_t ap;
	wpa_sm sm;
	wpa_out o;
	unsigned char f[600], kd[200];
	size_t n, i;
	/* A4: msg-1 with a PMKID KDE plus other KDEs and junk -> msg-2 still sent */
	setup(&ap, &sm, "correct horse battery");
	n = 0;
	kd[n++] = 0xdd; kd[n++] = 20; kd[n++] = 0; kd[n++] = 0x0f; kd[n++] = 0xac; kd[n++] = 4;
	for (i = 0; i < 16; i++) kd[n++] = (unsigned char)i;                 /* PMKID */
	kd[n++] = 0xdd; kd[n++] = 5; kd[n++] = 0x00; kd[n++] = 0x0f; kd[n++] = 0xac; kd[n++] = 0x0c; kd[n++] = 1;
	kd[n++] = 0xf4; kd[n++] = 0xff;                                      /* truncated garbage */
	n = ap_msg1(&ap, f, kd, n);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == 0 && o.msg == 1 && ap_check_msg2(&ap, o.tx, o.tx_len));
	n = ap_msg3(&ap, f, 1, 1);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == 0 && o.open_port);
}

static void t_bounds(void)
{
	ap_t ap;
	wpa_sm sm;
	wpa_out o;
	unsigned char f[1600], pt[600], w[620];
	size_t n, k;

	/* A5/B1/B6: Key Data length larger than the frame -> dropped before any crypto */
	setup(&ap, &sm, "correct horse battery");
	n = ap_msg1(&ap, f, NULL, 0);
	f[WPA_OFF_KDLEN] = 0x10;
	CHECK(wpa_rx(&sm, AA, f, n, &o) == -1 && o.tx_len == 0);
	/* EAPOL body length larger than what was received */
	n = ap_msg1(&ap, f, NULL, 0);
	CHECK(wpa_rx(&sm, AA, f, n - 1, &o) == -1);
	CHECK(wpa_rx(&sm, AA, f, 50, &o) == -1);
	CHECK(wpa_rx(&sm, AA, f, 3, &o) == -1);
	/* trailing padding after the EAPOL body is ignored */
	n = ap_msg1(&ap, f, NULL, 0);
	CHECK(wpa_rx(&sm, AA, f, n + 20, &o) == 0 && o.msg == 1);
	ap_check_msg2(&ap, o.tx, o.tx_len);

	/* oversized encrypted Key Data (valid MIC, 600 bytes) -> dropped, no crash */
	memset(pt, 0, sizeof(pt));
	n = ap_wrap(&ap, pt, 592, w);           /* 600 bytes */
	n = ap_frame(&ap, f, 0x13ca, ap.anonce, w, n, 1);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == -1 && strstr(sm.last_drop, "length") && !o.set_ptk);
	/* encrypted Key Data not a multiple of 8 */
	n = ap_frame(&ap, f, 0x13ca, ap.anonce, w, 30, 1);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == -1 && !o.set_ptk);
	/* garbage plaintext: element length runs past the end */
	k = 0;
	pt[k++] = 0x30; pt[k++] = 0x14; memset(pt + k, 1, 20); k += 20;
	pt[k++] = 0xdd; pt[k++] = 0xf0; pt[k++] = 0; pt[k++] = 0x0f; pt[k++] = 0xac; pt[k++] = 1;
	n = ap_wrap(&ap, pt, k, w);
	n = ap_frame(&ap, f, 0x13ca, ap.anonce, w, n, 1);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == -1 && !o.set_ptk && !o.open_port);
	/* GTK KDE claiming a 40-byte key */
	k = 0;
	hex(AP_RSNE, pt, strlen(AP_RSNE) / 2); k = strlen(AP_RSNE) / 2;
	pt[k++] = 0xdd; pt[k++] = 46; pt[k++] = 0; pt[k++] = 0x0f; pt[k++] = 0xac; pt[k++] = 1;
	memset(pt + k, 7, 42); k += 42;
	n = ap_wrap(&ap, pt, k, w);
	n = ap_frame(&ap, f, 0x13ca, ap.anonce, w, n, 1);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == -1 && !o.set_ptk);
	/* no GTK at all */
	k = strlen(AP_RSNE) / 2;
	n = ap_wrap(&ap, pt, k, w);
	n = ap_frame(&ap, f, 0x13ca, ap.anonce, w, n, 1);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == -1 && !o.set_ptk);
	/* msg-3 RSNE that is not WPA2-PSK/CCMP (AKM SAE only) */
	k = 0;
	hex("30140100000fac040100000fac040100000fac080000", pt, 22); k = 22;
	k += gtk_kde(pt + k, 1, ap.gtk);
	n = ap_wrap(&ap, pt, k, w);
	n = ap_frame(&ap, f, 0x13ca, ap.anonce, w, n, 1);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == -1 && strstr(sm.last_drop, "RSNE") && !o.set_ptk);
	/* none of the above opened the port; the real msg-3 still works */
	CHECK(sm.state == WPA_HANDSHAKE);
	n = ap_msg3(&ap, f, 1, 1);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == 0 && o.open_port);

	/* random garbage frames: must never crash or produce output */
	{
		unsigned seed = 12345;
		int it;
		for (it = 0; it < 20000; it++) {
			size_t len = (seed = seed * 1103515245u + 12345u) % 700, i;
			for (i = 0; i < len; i++)
				f[i] = (unsigned char)((seed = seed * 1103515245u + 12345u) >> 16);
			if (it & 1) { f[1] = 3; f[4] = 2; f[6] = (unsigned char)((f[6] & ~7) | 2); }
			if (len > 4 && (it & 2)) { f[2] = (unsigned char)((len - 4) >> 8); f[3] = (unsigned char)(len - 4); }
			wpa_rx(&sm, AA, f, len, &o);
			if (o.set_ptk || o.set_gtk || o.open_port) { CHECK(0); break; }
		}
	}
}

static void t_parse_kd(void)
{
	unsigned char b[200];
	wpa_kd kd;
	size_t n = 0;
	/* element loop: RSNE, RSNXE, vendor IE, IGTK, GTK, padding */
	hex(AP_RSNE, b, 26); n = 26;
	hex(RSNXE, b + n, 3); n += 3;
	b[n++] = 0xdd; b[n++] = 3; b[n++] = 0x00; b[n++] = 0x50; b[n++] = 0xf2;   /* short vendor IE */
	n += igtk_kde(b + n);
	{
		unsigned char g[16];
		memset(g, 0x33, 16);
		b[n] = 0; n += gtk_kde(b + n, 3, g);
		b[n - 24 + 6] = 0x07;   /* key id 3, tx bit set */
	}
	b[n++] = 0xdd; b[n++] = 0; b[n++] = 0; b[n++] = 0;
	CHECK(wpa_parse_kd(b, n, &kd) == 0 && kd.have_rsne && kd.rsne_off == 0 && kd.rsne_len == 26);
	CHECK(kd.have_gtk && kd.gtk_idx == 3 && kd.gtk_tx == 1 && kd.gtk_len == 16 && kd.gtk[15] == 0x33);
	CHECK(kd.n_ignored == 3);
	CHECK(kd.n_ids == 5 && kd.ids[0] == 0x30 && kd.ids[1] == 0xf4 && kd.ids[2] == 0xdd &&
	      kd.ids[3] == 0xdd09 && kd.ids[4] == 0xdd01);
	CHECK(wpa_check_rsne(b, 26) == 0);
	/* two GTK KDEs -> malformed */
	{
		unsigned char g[16] = { 0 };
		size_t m = gtk_kde(b, 1, g);
		m += gtk_kde(b + m, 2, g);
		CHECK(wpa_parse_kd(b, m, &kd) == -1);
	}
	/* truncated: last element claims more than is left */
	b[0] = 0xf4; b[1] = 10; b[2] = 0;
	CHECK(wpa_parse_kd(b, 5, &kd) == -1);
	/* a single trailing byte that is not padding */
	b[0] = 0xf4; b[1] = 1; b[2] = 0; b[3] = 0x30;
	CHECK(wpa_parse_kd(b, 4, &kd) == -1);
	b[3] = 0xdd;
	CHECK(wpa_parse_kd(b, 4, &kd) == 0 && !kd.have_gtk);
	CHECK(wpa_parse_kd(b, 0, &kd) == 0);
	/* RSNE checks */
	CHECK(wpa_check_rsne((const unsigned char *)"\x30\x02\x01\x00", 4) == -1);   /* no AKM list */
	hex("30140100000fac020100000fac040100000fac020000", b, 22);                  /* group TKIP */
	CHECK(wpa_check_rsne(b, 22) == -1);
	hex("30140100000fac040100000fac040100000fac020000", b, 22);
	CHECK(wpa_check_rsne(b, 22) == 0);
	b[1] = 0x30;
	CHECK(wpa_check_rsne(b, 22) == -1);                                          /* length lies */
	hex("30140100000fac040500000fac040100000fac020000", b, 22);                  /* count past end */
	CHECK(wpa_check_rsne(b, 22) == -1);
}

static void t_negative(void)
{
	ap_t ap;
	wpa_sm sm;
	wpa_out o;
	unsigned char f[600];
	size_t n;
	unsigned char other[6] = { 0x08, 0xb6, 0x57, 0xb4, 0x2d, 0x6a };

	/* wrong MIC on msg-3 -> dropped, no msg-4, nothing installed */
	setup(&ap, &sm, "correct horse battery");
	n = ap_msg1(&ap, f, NULL, 0);
	wpa_rx(&sm, AA, f, n, &o);
	ap_check_msg2(&ap, o.tx, o.tx_len);
	n = ap_msg3(&ap, f, 1, 0);
	f[WPA_OFF_MIC + 3] ^= 0x20;
	CHECK(wpa_rx(&sm, AA, f, n, &o) == -1 && o.tx_len == 0 && !o.set_ptk && sm.st.bad_mic == 1);
	CHECK(!sm.rc_set);                     /* an unauthenticated frame never moves the counter */
	/* B2: msg-3 with a different ANonce (valid MIC with the PTK of another nonce) */
	ap.anonce[5] ^= 1;
	n = ap_msg3(&ap, f, 1, 0);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == -1 && strstr(sm.last_drop, "ANonce"));
	ap.anonce[5] ^= 1;
	/* frame from another BSSID */
	n = ap_msg3(&ap, f, 1, 0);
	CHECK(wpa_rx(&sm, other, f, n, &o) == -1 && strstr(sm.last_drop, "not from our AP"));
	/* A2: WPA1 descriptor (254) and descriptor version 1 (MD5/RC4) are refused */
	n = ap_msg3(&ap, f, 1, 0);
	f[4] = 254;
	CHECK(wpa_rx(&sm, AA, f, n, &o) == -1);
	n = ap_msg3(&ap, f, 1, 0);
	f[6] = (unsigned char)((f[6] & ~7) | 1);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == -1);
	/* the genuine msg-3 still completes */
	n = ap_msg3(&ap, f, 1, 0);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == 0 && o.open_port);
	/* group msg-1 with an old counter (replay) / bad MIC */
	{
		unsigned long long save = ap.rc;
		ap.rc = 1;
		n = ap_group1(&ap, f);
		CHECK(wpa_rx(&sm, AA, f, n, &o) == -1 && strstr(sm.last_drop, "replay"));
		ap.rc = save;
		n = ap_group1(&ap, f);
		f[WPA_OFF_MIC] ^= 1;
		CHECK(wpa_rx(&sm, AA, f, n, &o) == -1 && !o.set_gtk);
	}
	/* group msg-1 before any 4-way */
	setup(&ap, &sm, "correct horse battery");
	n = ap_group1(&ap, f);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == -1);
	/* nothing at all while not associated */
	wpa_disassoc(&sm);
	n = ap_msg1(&ap, f, NULL, 0);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == -1 && strstr(sm.last_drop, "not associated"));
	/* RNG failure -> no msg-2 with a predictable nonce */
	setup(&ap, &sm, "correct horse battery");
	sm.rng = bad_rng;
	n = ap_msg1(&ap, f, NULL, 0);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == -1 && o.tx_len == 0);
}

static void t_snonce(void)
{
	ap_t ap;
	wpa_sm sm;
	wpa_out o;
	unsigned char f[600], s1[32], b[32];
	size_t n;
	int before;
	/* B4: the SNonce is exactly what the DRBG callback returned, new per handshake,
	 * kept for a retransmitted msg-1 with the same ANonce */
	setup(&ap, &sm, "correct horse battery");
	before = rng_calls;
	n = ap_msg1(&ap, f, NULL, 0);
	wpa_rx(&sm, AA, f, n, &o);
	memcpy(s1, o.tx + WPA_OFF_NONCE, 32);
	rng_calls = before;
	test_rng(NULL, b, 32);
	CHECK(memcmp(s1, b, 32) == 0);
	n = ap_msg1(&ap, f, NULL, 0);
	wpa_rx(&sm, AA, f, n, &o);
	CHECK(memcmp(o.tx + WPA_OFF_NONCE, s1, 32) == 0);
	ap.anonce[0] ^= 1;
	n = ap_msg1(&ap, f, NULL, 0);
	wpa_rx(&sm, AA, f, n, &o);
	CHECK(memcmp(o.tx + WPA_OFF_NONCE, s1, 32) != 0);
}

static void t_wrong_passphrase(void)
{
	ap_t ap;
	wpa_sm sm;
	wpa_out o;
	unsigned char f[600], g[600];
	size_t n, m;
	static const unsigned char AA2[6] = { 0xb0, 0xf2, 0x08, 0xc8, 0xc1, 0xbd };
	/* the AP cannot verify msg-2 and re-sends msg-1 (higher counter) */
	setup(&ap, &sm, "wrong horse battery");
	n = ap_msg1(&ap, f, NULL, 0);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == 0 && !ap_check_msg2(&ap, o.tx, o.tx_len));
	CHECK(!wpa_wrong_passphrase(&sm.st));
	/* the same frame again (stale copy / replay) is answered but not counted */
	CHECK(wpa_rx(&sm, AA, f, n, &o) == 0 && sm.st.retried == 0);
	n = ap_msg1(&ap, f, NULL, 0);
	wpa_rx(&sm, AA, f, n, &o);
	CHECK(sm.st.retried == 1 && !wpa_wrong_passphrase(&sm.st));   /* one lost msg-2 is not proof */
	n = ap_msg1(&ap, f, NULL, 0);
	wpa_rx(&sm, AA, f, n, &o);
	CHECK(sm.st.retried == 2 && wpa_wrong_passphrase(&sm.st));
	/* the AP gives up and deauths: one rejected association; per-association
	 * counters start over (review P3) */
	wpa_disassoc(&sm);
	CHECK(sm.st.rejects == 1 && sm.st.retried == 0 && !wpa_wrong_passphrase(&sm.st));
	/* a heuristic/silent re-association never counts (review M1) ... */
	wpa_assoc(&sm, AA);
	n = ap_msg1(&ap, f, NULL, 0);
	wpa_rx(&sm, AA, f, n, &o);
	wpa_reassoc(&sm);
	CHECK(sm.st.rejects == 1);
	/* ... a second driver-observed loss after msg-2 does */
	n = ap_msg1(&ap, f, NULL, 0);
	wpa_rx(&sm, AA, f, n, &o);
	wpa_disassoc(&sm);
	wpa_assoc(&sm, AA);
	CHECK(sm.st.rejects == 2 && wpa_wrong_passphrase(&sm.st));
	/* association lost before any msg-1: not a rejection (e.g. out of range) */
	wpa_disassoc(&sm);
	wpa_assoc(&sm, AA);
	wpa_disassoc(&sm);
	CHECK(sm.st.rejects == 2);
	/* roam to another BSSID during a handshake: not a rejection by this AP */
	setup(&ap, &sm, "wrong horse battery");
	n = ap_msg1(&ap, f, NULL, 0);
	wpa_rx(&sm, AA, f, n, &o);
	wpa_assoc(&sm, AA2);
	CHECK(sm.st.rejects == 0);

	/* correct passphrase with one lost msg-2: success resets the counters */
	setup(&ap, &sm, "correct horse battery");
	n = ap_msg1(&ap, f, NULL, 0);
	wpa_rx(&sm, AA, f, n, &o);              /* "lost" */
	n = ap_msg1(&ap, f, NULL, 0);
	wpa_rx(&sm, AA, f, n, &o);
	CHECK(ap_check_msg2(&ap, o.tx, o.tx_len) && sm.st.retried == 1);
	n = ap_msg3(&ap, f, 1, 1);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == 0 && o.open_port);
	CHECK(sm.st.retried == 0 && sm.st.rejects == 0 && !wpa_wrong_passphrase(&sm.st));
	/* forged msg-1s while connected (unauthenticated) never count (review P2) */
	{
		ap_t fake = ap;
		int k;
		fake.anonce[0] ^= 0x77;
		for (k = 0; k < 3; k++) {
			m = ap_msg1(&fake, g, NULL, 0);
			wpa_rx(&sm, AA, g, m, &o);
		}
		CHECK(sm.st.retried == 0 && !wpa_wrong_passphrase(&sm.st));
	}
	/* a deauth after success is not a rejection */
	wpa_disassoc(&sm);
	CHECK(sm.st.rejects == 0);

	/* review P4: after success a forged msg-1 reusing the current ANonce must not
	 * break the AP's genuine msg-3 retransmission (msg-4 lost) */
	setup(&ap, &sm, "correct horse battery");
	CHECK(handshake(&ap, &sm, 1, 1));
	{
		ap_t fake = ap;
		fake.rc += 100;
		m = ap_msg1(&fake, g, NULL, 0);
		/* L8: the finished handshake is not re-opened; only a rekey is pending */
		CHECK(wpa_rx(&sm, AA, g, m, &o) == 0 && sm.state == WPA_DONE && sm.rekey == 1);
	}
	n = ap_msg3(&ap, f, 1, 1);               /* genuine retransmission, old PTK */
	CHECK(wpa_rx(&sm, AA, f, n, &o) == 0 && o.msg == 3 && !o.set_ptk && !o.set_gtk);
	CHECK(ap_check_mic(&ap, o.tx, o.tx_len, 0x030a) && sm.state == WPA_DONE && sm.st.msg3_dup == 1);
	(void)g;
}

static void t_clear(void)
{
	ap_t ap;
	wpa_sm sm;
	size_t i;
	int nz = 0;
	setup(&ap, &sm, "correct horse battery");
	CHECK(handshake(&ap, &sm, 1, 1));
	wpa_clear(&sm);
	for (i = 0; i < sizeof(sm); i++)
		nz |= ((unsigned char *)&sm)[i];
	CHECK(nz == 0);
}

/* ---- review round 2 ------------------------------------------------------ */

/* A2 made falsifiable: the reply builder must echo EAPOL version, descriptor
 * type and descriptor version of whatever it answers (a hard-coded 2/2/2
 * passes every frame wpa_rx accepts, so test the builder directly). */
static void t_a2_echo(void)
{
	unsigned char rx[WPA_OFF_KD], kck[16];
	wpa_out o;
	memset(rx, 0, sizeof(rx));
	memset(kck, 1, sizeof(kck));
	rx[0] = 1; rx[1] = 3; rx[4] = 254; rx[5] = 0x00; rx[6] = 0x89;   /* WPA1 desc, version 1 */
	CHECK(wpa_reply(&o, rx, WPA_KI_MIC | WPA_KI_SECURE, NULL, NULL, 0, kck) == 0);
	CHECK(o.tx[0] == 1 && o.tx[4] == 254 && (((o.tx[5] << 8) | o.tx[6]) & 7) == 1);
	CHECK(((o.tx[5] << 8) | o.tx[6]) == (0x0001 | WPA_KI_MIC | WPA_KI_SECURE));
	rx[0] = 2; rx[4] = 2; rx[6] = 0x8a;
	CHECK(wpa_reply(&o, rx, WPA_KI_PAIRWISE | WPA_KI_MIC, NULL, NULL, 0, kck) == 0);
	CHECK(o.tx[0] == 2 && o.tx[4] == 2 && ((o.tx[5] << 8) | o.tx[6]) == 0x010a);
	rx[0] = 3;                                                     /* 802.1X-2010 -> 2 */
	CHECK(wpa_reply(&o, rx, 0, NULL, NULL, 0, kck) == 0 && o.tx[0] == 2);
}

/* msg-3 / group msg-1 whose Key Data is not marked encrypted are refused
 * even with a valid MIC (the GTK must never travel in the clear). */
static void t_encdata(void)
{
	ap_t ap;
	wpa_sm sm;
	wpa_out o;
	unsigned char f[600], pt[100];
	size_t n, k;
	setup(&ap, &sm, "correct horse battery");
	n = ap_msg1(&ap, f, NULL, 0);
	wpa_rx(&sm, AA, f, n, &o);
	ap_check_msg2(&ap, o.tx, o.tx_len);
	hex(AP_RSNE, pt, 26); k = 26;
	k += gtk_kde(pt + k, 1, ap.gtk);
	n = ap_frame(&ap, f, 0x13ca & ~WPA_KI_ENCDATA, ap.anonce, pt, k, 1);   /* plaintext GTK */
	CHECK(wpa_rx(&sm, AA, f, n, &o) == -1 && strstr(sm.last_drop, "not encrypted") && !o.set_gtk);
	n = ap_msg3(&ap, f, 1, 0);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == 0 && o.open_port);
	n = ap_frame(&ap, f, 0x1382 & ~WPA_KI_ENCDATA, NULL, pt + 26, 24, 1);
	CHECK(wpa_rx(&sm, AA, f, n, &o) == -1 && strstr(sm.last_drop, "not encrypted") && !o.set_gtk);
}

/* direct fuzz of the parsers (the frame fuzz never gets past the MIC) */
static void t_parser_fuzz(void)
{
	unsigned char b[300];
	unsigned seed = 777;
	int it, bad = 0;
	for (it = 0; it < 200000; it++) {
		size_t len = (seed = seed * 1103515245u + 12345u) % 260, i;
		wpa_kd kd;
		for (i = 0; i < len; i++) {
			seed = seed * 1103515245u + 12345u;
			b[i] = (unsigned char)(seed >> 16);
			/* bias towards interesting ids / small lengths */
			if ((seed & 0x700) == 0) b[i] = 0xdd;
			else if ((seed & 0x700) == 0x100) b[i] = 0x30;
			else if ((seed & 0x700) == 0x200) b[i] = (unsigned char)(b[i] & 0x1f);
		}
		if (wpa_parse_kd(b, len, &kd) == 0) {
			if (kd.have_rsne && (kd.rsne_off + kd.rsne_len > len || wpa_check_rsne(b + kd.rsne_off, kd.rsne_len) > 0))
				bad++;
			if (kd.have_gtk && (kd.gtk_len < 5 || kd.gtk_len > 32 || kd.gtk_idx > 3))
				bad++;
			if (kd.n_ids > 12)
				bad++;
		}
		if (len >= 2) {
			b[0] = 0x30;
			b[1] = (unsigned char)(len - 2);
		}
		wpa_check_rsne(b, len);
	}
	CHECK(bad == 0);
}

/* ---- wpa_drv.c harness ---------------------------------------------------- */

static unsigned char drv_bssid[6];
static int drv_valid, drv_ctrl, drv_setkey_pw, drv_setkey_grp, drv_delkey, drv_setport;
static long drv_setkey_pw_at;
static unsigned char drv_tx[WPA_TX_MAX];
static size_t drv_tx_len;
static long drv_tx_at;

int wpa_drv_t_rng(void *ctx, unsigned char *b, size_t n) { return test_rng(ctx, b, n); }

static int stub_req(void *p, size_t len)
{
	unsigned char *r = p;
	(void)len;
	switch (r[0]) {
	case 36: {
		unsigned int v = (unsigned int)drv_valid;   /* IsValid: native u32 */
		memcpy(r + 4, &v, 4);
		memcpy(r + 8, drv_bssid, 6);
		break;
	}
	case 11: if (r[12] == 1) { drv_setkey_pw++; drv_setkey_pw_at = wpa_drv_t_now; } else drv_setkey_grp++; break;
	case 12: drv_setport++; break;
	case 13: drv_delkey++; break;
	}
	return 0;
}
static int stub_ctrl(void) { return drv_ctrl; }
static int stub_send(const unsigned char *f, size_t len)
{
	memcpy(drv_tx, f, len); drv_tx_len = len; drv_tx_at = wpa_drv_t_now;
	return 0;
}

static void drv_feed(const unsigned char *src, const unsigned char *f, size_t n)
{
	unsigned char e[700];
	memcpy(e, SPA, 6); memcpy(e + 6, src, 6); e[12] = 0x88; e[13] = 0x8e;
	memcpy(e + 14, f, n);
	wpa_drv_t_eapol(e, n + 14);
}

static void drv_setup(ap_t *ap, const char *pass)
{
	wpa_sm dummy;
	unsigned char pmk[32];
	setup(ap, &dummy, pass);
	wpa_pmk(pass, (const unsigned char *)"example-ssid", 12, pmk);
	wpa_drv_t_req = stub_req; wpa_drv_t_ctrlport = stub_ctrl; wpa_drv_t_send = stub_send;
	wpa_drv_t_now = 100000;
	drv_valid = 1; memcpy(drv_bssid, AA, 6); drv_ctrl = 0;
	drv_setkey_pw = drv_setkey_grp = drv_delkey = drv_setport = 0;
	wpa_drv_t_setup(pmk, SPA);
	wpa_drv_t_poll();                       /* associated */
}

/* drive a full 4-way through wpa_drv.c */
static int drv_handshake(ap_t *ap)
{
	unsigned char f[600];
	size_t n = ap_msg1(ap, f, NULL, 0);
	drv_feed(AA, f, n);
	if (!ap_check_msg2(ap, drv_tx, drv_tx_len))
		return 0;
	n = ap_msg3(ap, f, 1, 1);
	drv_feed(AA, f, n);
	drv_ctrl = 1;
	return wpa_drv_t_port();
}

static void t_drv_m1(void)
{
	ap_t ap, fake;
	unsigned char f[600];
	size_t n;
	int k;
	/* M1: forged msg-1s with a high counter make the AP's genuine one look like
	 * "counter went back" -> heuristic re-association. That must never count a
	 * rejection, however often it happens. */
	drv_setup(&ap, "correct horse battery");
	for (k = 0; k < 5; k++) {
		fake = ap;
		fake.rc = 1000 + 10 * k;
		n = ap_msg1(&fake, f, NULL, 0);
		drv_feed(AA, f, n);
		n = ap_msg1(&ap, f, NULL, 0);      /* genuine, lower counter */
		drv_feed(AA, f, n);
		ap_check_msg2(&ap, drv_tx, drv_tx_len);
	}
	CHECK(wpa_drv_t_sm()->st.rejects == 0 && !wpa_wrong_passphrase(&wpa_drv_t_sm()->st));
	/* the join still completes */
	n = ap_msg3(&ap, f, 1, 1);
	drv_feed(AA, f, n);
	CHECK(wpa_drv_t_port() == 1);

	/* a forged msg-1 storm before msg-3 can reach the retry rule, but one
	 * MIC-verified msg-3 clears the verdict for good */
	drv_setup(&ap, "correct horse battery");
	n = ap_msg1(&ap, f, NULL, 0);
	drv_feed(AA, f, n);
	for (k = 0; k < 3; k++) {
		fake = ap;
		fake.rc = 50 + k;
		fake.anonce[0] ^= 0x33;
		n = ap_msg1(&fake, f, NULL, 0);
		drv_feed(AA, f, n);
	}
	CHECK(wpa_wrong_passphrase(&wpa_drv_t_sm()->st));
	ap.rc = 60;
	CHECK(drv_handshake(&ap) == 1);
	CHECK(!wpa_wrong_passphrase(&wpa_drv_t_sm()->st) && wpa_drv_t_sm()->st.msg3_ok == 1);
	drv_valid = 0;                         /* even after a later disassociation */
	wpa_drv_t_poll();
	CHECK(!wpa_wrong_passphrase(&wpa_drv_t_sm()->st));

	/* only driver-observed disassociations count: wrong passphrase, AP deauths twice */
	drv_setup(&ap, "wrong horse battery");
	for (k = 0; k < 2; k++) {
		drv_valid = 1;
		wpa_drv_t_poll();
		n = ap_msg1(&ap, f, NULL, 0);
		drv_feed(AA, f, n);
		drv_valid = 0;
		wpa_drv_t_poll();
	}
	CHECK(wpa_drv_t_sm()->st.rejects == 2 && wpa_wrong_passphrase(&wpa_drv_t_sm()->st));
	/* L7: each loss dropped the group key and cleared the GTK state */
	CHECK(drv_delkey == 2 && !wpa_drv_t_sm()->gtk_set);
	/* a bad msg-3 MIC (not a passphrase symptom) suppresses the verdict */
	drv_setup(&ap, "correct horse battery");
	n = ap_msg1(&ap, f, NULL, 0);
	drv_feed(AA, f, n);
	n = ap_msg3(&ap, f, 1, 0);
	f[WPA_OFF_MIC] ^= 1;
	drv_feed(AA, f, n);
	wpa_drv_t_sm()->st.rejects = 5;
	CHECK(!wpa_wrong_passphrase(&wpa_drv_t_sm()->st));
}

static void t_drv_m2(void)
{
	ap_t ap;
	unsigned char f[600];
	size_t n;
	/* first handshake: PTK installed at once (msg-4 is plaintext) */
	drv_setup(&ap, "correct horse battery");
	CHECK(drv_handshake(&ap) == 1 && drv_setkey_pw == 1 && drv_setkey_pw_at == drv_tx_at);
	/* PTK rekey: msg-4 goes out under the old TK; the new TK only >= 50 ms later */
	ap.anonce[1] ^= 0x5a;
	n = ap_msg1(&ap, f, NULL, 0);
	drv_feed(AA, f, n);
	CHECK(ap_check_msg2(&ap, drv_tx, drv_tx_len));
	n = ap_msg3(&ap, f, 1, 1);
	drv_feed(AA, f, n);
	CHECK(ap_check_mic(&ap, drv_tx, drv_tx_len, 0x030a));   /* msg-4 sent */
	CHECK(drv_setkey_pw == 1);                                /* not yet */
	wpa_drv_t_now += 20;
	wpa_drv_t_timers();
	CHECK(drv_setkey_pw == 1);
	wpa_drv_t_now += 30;
	wpa_drv_t_timers();
	CHECK(drv_setkey_pw == 2 && drv_setkey_pw_at - drv_tx_at >= 50);
	wpa_drv_t_now += 100;
	wpa_drv_t_timers();
	CHECK(drv_setkey_pw == 2);                                /* once */
	/* a pending rekey TK is dropped if the association goes away first */
	ap.anonce[2] ^= 0x5a;
	n = ap_msg1(&ap, f, NULL, 0);
	drv_feed(AA, f, n);
	n = ap_msg3(&ap, f, 1, 1);
	drv_feed(AA, f, n);
	drv_valid = 0;
	wpa_drv_t_poll();
	wpa_drv_t_now += 100;
	wpa_drv_t_timers();
	CHECK(drv_setkey_pw == 2);
}

static void t_drv_logs(void)
{
	ap_t ap;
	unsigned char f[600];
	size_t n;
	int k;
	unsigned before;
	/* L2: 1000 garbage/forged frames -> a handful of log lines, not 1000 */
	drv_setup(&ap, "correct horse battery");
	before = wpa_drv_t_logs;
	for (k = 0; k < 1000; k++) {
		n = ap_msg1(&ap, f, NULL, 0);
		f[WPA_OFF_DESC] = 254;                       /* dropped */
		drv_feed(AA, f, n);
		n = ap_msg1(&ap, f, NULL, 0);                /* answered msg-1 */
		drv_feed(AA, f, n);
	}
	CHECK(wpa_drv_t_logs - before <= 12);
	wpa_drv_t_now += 10000;                              /* next window: summary + line */
	before = wpa_drv_t_logs;
	n = ap_msg1(&ap, f, NULL, 0);
	f[WPA_OFF_DESC] = 254;
	drv_feed(AA, f, n);
	CHECK(wpa_drv_t_logs - before == 2);
}

static void t_drv_pmk_forget(void)
{
	unsigned char pmk[32];
	/* L4: Wi-Fi disabled -> the cached PMK and its cache key are gone */
	CHECK(wpa_drv_pmk("example-ssid", "correct horse battery", pmk) == 0 && wpa_drv_t_pmk_cached());
	wpa_drv_forget();
	CHECK(!wpa_drv_t_pmk_cached());
}

int main(void)
{
	t_pmk();
	t_prf();
	t_unwrap();
	t_parse_kd();
	t_full();
	t_retransmit_msg3();
	t_msg1_extras();
	t_bounds();
	t_negative();
	t_snonce();
	t_wrong_passphrase();
	t_clear();
	t_a2_echo();
	t_encdata();
	t_parser_fuzz();
	t_drv_m1();
	t_drv_m2();
	t_drv_logs();
	t_drv_pmk_forget();
	printf("test_wpa: %d/%d checks passed\n", checks - fails, checks);
	return fails ? 1 : 0;
}
