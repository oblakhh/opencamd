/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * wpa — WPA2-PSK / CCMP supplicant state machine and crypto (see wpa.h).
 */

#include "wpa.h"

#include <string.h>

#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/platform_util.h"

#define zero(p, n) mbedtls_platform_zeroize((p), (n))

static unsigned be16(const unsigned char *p)
{
	return ((unsigned)p[0] << 8) | p[1];
}

static void put_be16(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)(v >> 8);
	p[1] = (unsigned char)v;
}

/* constant-time equality */
static int ct_eq(const unsigned char *a, const unsigned char *b, size_t n)
{
	unsigned char d = 0;
	size_t i;
	for (i = 0; i < n; i++)
		d |= (unsigned char)(a[i] ^ b[i]);
	return d == 0;
}

static const mbedtls_md_info_t *sha1(void)
{
	return mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
}

/* ---- primitives ------------------------------------------------------- */

int wpa_pmk(const char *passphrase, const unsigned char *ssid, size_t ssid_len,
            unsigned char pmk[WPA_PMK_LEN])
{
	mbedtls_md_context_t md;
	size_t pl, i;
	int rc;

	zero(pmk, WPA_PMK_LEN);
	if (!passphrase || !ssid || ssid_len < 1 || ssid_len > 32)
		return -1;
	pl = strlen(passphrase);
	if (pl < 8 || pl > 63)
		return -1;
	for (i = 0; i < pl; i++)
		if ((unsigned char)passphrase[i] < 0x20 || (unsigned char)passphrase[i] > 0x7e)
			return -1;
	mbedtls_md_init(&md);
	rc = mbedtls_md_setup(&md, sha1(), 1);
	if (rc == 0)
		rc = mbedtls_pkcs5_pbkdf2_hmac(&md, (const unsigned char *)passphrase, pl,
		                               ssid, ssid_len, 4096, WPA_PMK_LEN, pmk);
	mbedtls_md_free(&md);
	if (rc != 0) {
		zero(pmk, WPA_PMK_LEN);
		return -1;
	}
	return 0;
}

int wpa_prf(const unsigned char *key, size_t key_len, const char *label,
            const unsigned char *data, size_t data_len, unsigned char *out, size_t out_len)
{
	mbedtls_md_context_t md;
	unsigned char h[20], zb = 0, ctr;
	size_t done = 0;
	int rc;

	mbedtls_md_init(&md);
	rc = mbedtls_md_setup(&md, sha1(), 1);
	for (ctr = 0; rc == 0 && done < out_len; ctr++) {
		size_t n = out_len - done < sizeof(h) ? out_len - done : sizeof(h);
		rc = mbedtls_md_hmac_starts(&md, key, key_len);
		if (rc == 0) rc = mbedtls_md_hmac_update(&md, (const unsigned char *)label, strlen(label));
		if (rc == 0) rc = mbedtls_md_hmac_update(&md, &zb, 1);
		if (rc == 0) rc = mbedtls_md_hmac_update(&md, data, data_len);
		if (rc == 0) rc = mbedtls_md_hmac_update(&md, &ctr, 1);
		if (rc == 0) rc = mbedtls_md_hmac_finish(&md, h);
		if (rc == 0) {
			memcpy(out + done, h, n);
			done += n;
		}
	}
	mbedtls_md_free(&md);
	zero(h, sizeof(h));
	if (rc != 0) {
		zero(out, out_len);
		return -1;
	}
	return 0;
}

int wpa_ptk(const unsigned char pmk[WPA_PMK_LEN], const unsigned char aa[6],
            const unsigned char spa[6], const unsigned char anonce[WPA_NONCE_LEN],
            const unsigned char snonce[WPA_NONCE_LEN], unsigned char ptk[WPA_PTK_LEN])
{
	unsigned char d[6 + 6 + WPA_NONCE_LEN + WPA_NONCE_LEN];
	int a_lt = memcmp(aa, spa, 6) < 0, n_lt = memcmp(anonce, snonce, WPA_NONCE_LEN) < 0, rc;

	memcpy(d, a_lt ? aa : spa, 6);
	memcpy(d + 6, a_lt ? spa : aa, 6);
	memcpy(d + 12, n_lt ? anonce : snonce, WPA_NONCE_LEN);
	memcpy(d + 12 + WPA_NONCE_LEN, n_lt ? snonce : anonce, WPA_NONCE_LEN);
	rc = wpa_prf(pmk, WPA_PMK_LEN, "Pairwise key expansion", d, sizeof(d), ptk, WPA_PTK_LEN);
	zero(d, sizeof(d));
	return rc;
}

int wpa_aes_unwrap(const unsigned char *kek, size_t kek_len,
                   const unsigned char *in, size_t in_len, unsigned char *out)
{
	mbedtls_aes_context aes;
	unsigned char a[8], b[16];
	size_t n, i;
	int j, rc = 0;

	if ((kek_len != 16 && kek_len != 24 && kek_len != 32) || in_len < 24 || in_len % 8)
		return -1;
	n = in_len / 8 - 1;
	memcpy(a, in, 8);
	memmove(out, in + 8, in_len - 8);
	mbedtls_aes_init(&aes);
	if (mbedtls_aes_setkey_dec(&aes, kek, (unsigned)(kek_len * 8)) != 0)
		rc = -1;
	for (j = 5; rc == 0 && j >= 0; j--) {
		for (i = n; rc == 0 && i >= 1; i--) {
			unsigned long t = (unsigned long)(n * (size_t)j + i);
			int k;
			memcpy(b, a, 8);
			for (k = 7; k >= 0 && t; k--, t >>= 8)
				b[k] ^= (unsigned char)(t & 0xff);
			memcpy(b + 8, out + (i - 1) * 8, 8);
			if (mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, b, b) != 0)
				rc = -1;
			memcpy(a, b, 8);
			memcpy(out + (i - 1) * 8, b + 8, 8);
		}
	}
	mbedtls_aes_free(&aes);
	if (rc == 0) {
		static const unsigned char iv[8] = { 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6 };
		if (!ct_eq(a, iv, 8))
			rc = -1;
	}
	zero(b, sizeof(b));
	if (rc != 0)
		zero(out, in_len - 8);
	return rc;
}

size_t wpa_rsn_ie(unsigned char ie[22])
{
	static const unsigned char rsn[22] = {
		0x30, 0x14,              /* RSN element, 20 bytes */
		0x01, 0x00,              /* version 1 */
		0x00, 0x0f, 0xac, 0x04,  /* group: CCMP */
		0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,   /* 1 pairwise: CCMP */
		0x01, 0x00, 0x00, 0x0f, 0xac, 0x02,   /* 1 AKM: PSK */
		0x00, 0x00               /* RSN capabilities: no PMF */
	};
	memcpy(ie, rsn, sizeof(rsn));
	return sizeof(rsn);
}

/* ---- Key Data ----------------------------------------------------------- */

static const unsigned char OUI_IEEE[3] = { 0x00, 0x0f, 0xac };

int wpa_parse_kd(const unsigned char *kd, size_t len, wpa_kd *out)
{
	size_t p = 0;

	memset(out, 0, sizeof(*out));
	while (p < len) {
		unsigned id, el;
		const unsigned char *body;
		if (len - p < 2) {
			if (kd[p] == 0xdd)     /* a lone padding byte at the very end */
				break;
			return -1;
		}
		id = kd[p];
		el = kd[p + 1];
		if (id == 0xdd && el == 0)  /* key wrap padding: 0xdd 0x00 ... */
			break;
		if (el > len - p - 2)
			return -1;             /* element runs past the end */
		body = kd + p + 2;
		if (out->n_ids < (int)(sizeof(out->ids) / sizeof(out->ids[0])))
			out->ids[out->n_ids++] = (unsigned short)(id == 0xdd && el >= 4 &&
			    memcmp(body, OUI_IEEE, 3) == 0 ? 0xdd00u | body[3] : id);
		if (id == 0x30) {
			if (!out->have_rsne) {
				out->have_rsne = 1;
				out->rsne_off = p;
				out->rsne_len = el + 2;
			} else {
				out->n_ignored++;
			}
		} else if (id == 0xdd && el >= 4 && memcmp(body, OUI_IEEE, 3) == 0) {
			switch (body[3]) {
			case 1:                /* GTK KDE: keyid/tx, reserved, GTK */
				if (el < 6 || el - 6 < 5 || el - 6 > sizeof(out->gtk) || out->have_gtk)
					return -1;
				out->have_gtk = 1;
				out->gtk_idx = body[4] & 0x03;
				out->gtk_tx = (body[4] >> 2) & 1;
				out->gtk_len = el - 6;
				memcpy(out->gtk, body + 6, out->gtk_len);
				break;
			default:               /* IGTK (9), PMKID (4), ... not used */
				out->n_ignored++;
				break;
			}
		} else {
			out->n_ignored++;          /* RSNXE (0xf4), vendor IEs, anything else */
		}
		p += 2 + el;
	}
	return 0;
}

static int has_suite(const unsigned char *p, size_t cnt, unsigned char type)
{
	size_t i;
	for (i = 0; i < cnt; i++)
		if (memcmp(p + i * 4, OUI_IEEE, 3) == 0 && p[i * 4 + 3] == type)
			return 1;
	return 0;
}

int wpa_check_rsne(const unsigned char *ie, size_t len)
{
	size_t p = 2, cnt;
	if (len < 2 || ie[0] != 0x30 || (size_t)ie[1] + 2 != len)
		return -1;
	/* version, group, pairwise list, AKM list are all required here: an
	 * omitted AKM list would default to 802.1X */
	if (len < p + 2 || ie[p] != 1 || ie[p + 1] != 0)
		return -1;
	p += 2;
	if (len < p + 4 || memcmp(ie + p, OUI_IEEE, 3) != 0 || ie[p + 3] != 4)
		return -1;
	p += 4;
	if (len < p + 2)
		return -1;
	cnt = ie[p] | ((size_t)ie[p + 1] << 8);
	p += 2;
	if (cnt == 0 || cnt > (len - p) / 4 || !has_suite(ie + p, cnt, 4))
		return -1;
	p += cnt * 4;
	if (len < p + 2)
		return -1;
	cnt = ie[p] | ((size_t)ie[p + 1] << 8);
	p += 2;
	if (cnt == 0 || cnt > (len - p) / 4 || !has_suite(ie + p, cnt, 2))
		return -1;
	return 0;
}

/* ---- state machine ------------------------------------------------------ */

void wpa_out_clear(wpa_out *out)
{
	zero(out, sizeof(*out));
}

static void reset_assoc(wpa_sm *sm)
{
	sm->state = WPA_IDLE;
	sm->rc_set = 0;
	zero(sm->rc, sizeof(sm->rc));
	sm->have_anonce = 0;
	zero(sm->anonce, sizeof(sm->anonce));
	zero(sm->snonce, sizeof(sm->snonce));
	zero(sm->tptk, sizeof(sm->tptk));
	sm->msg2_pending = 0;
	sm->attempt_msg2 = 0;
	sm->rekey = 0;
	sm->m1_rc_set = 0;
	zero(sm->m1_rc, sizeof(sm->m1_rc));
	sm->st.retried = 0;
	sm->ptk_valid = 0;
	zero(sm->ptk, sizeof(sm->ptk));
	zero(sm->ptk_anonce, sizeof(sm->ptk_anonce));
	sm->gtk_set = 0;
	sm->gtk_idx = 0;
	zero(sm->gtk, sizeof(sm->gtk));
}

void wpa_init(wpa_sm *sm, const unsigned char pmk[WPA_PMK_LEN], const unsigned char spa[6],
              wpa_rng_fn rng, void *rng_ctx)
{
	zero(sm, sizeof(*sm));
	memcpy(sm->pmk, pmk, WPA_PMK_LEN);
	memcpy(sm->spa, spa, 6);
	sm->ie_len = wpa_rsn_ie(sm->ie);
	sm->rng = rng;
	sm->rng_ctx = rng_ctx;
	sm->last_drop = "";
}

void wpa_disassoc(wpa_sm *sm)
{
	if (sm->assoc && sm->attempt_msg2)
		sm->st.rejects++;     /* the AP ended the association without accepting our msg-2 */
	sm->assoc = 0;
	reset_assoc(sm);
}

void wpa_assoc(wpa_sm *sm, const unsigned char aa[6])
{
	/* no rejection is counted here: a roam is not a rejection by this AP, and
	 * a re-association found by a heuristic may have been provoked by forged
	 * (unauthenticated) msg-1 frames (review M1) */
	reset_assoc(sm);
	memcpy(sm->aa, aa, 6);
	sm->assoc = 1;
}

void wpa_reassoc(wpa_sm *sm)
{
	unsigned char aa[6];
	memcpy(aa, sm->aa, 6);
	wpa_assoc(sm, aa);
}

void wpa_clear(wpa_sm *sm)
{
	zero(sm, sizeof(*sm));
}

static int drop(wpa_sm *sm, const char *why)
{
	sm->st.drops++;
	sm->last_drop = why;
	return -1;
}

/* MIC = HMAC-SHA1-128(KCK, frame with the MIC field zeroed) */
static int calc_mic(const unsigned char *kck, const unsigned char *f, size_t flen,
                    unsigned char mic[16])
{
	static const unsigned char z[16] = { 0 };
	mbedtls_md_context_t md;
	unsigned char h[20];
	int rc;

	mbedtls_md_init(&md);
	rc = mbedtls_md_setup(&md, sha1(), 1);
	if (rc == 0) rc = mbedtls_md_hmac_starts(&md, kck, WPA_KCK_LEN);
	if (rc == 0) rc = mbedtls_md_hmac_update(&md, f, WPA_OFF_MIC);
	if (rc == 0) rc = mbedtls_md_hmac_update(&md, z, 16);
	if (rc == 0) rc = mbedtls_md_hmac_update(&md, f + WPA_OFF_MIC + 16, flen - WPA_OFF_MIC - 16);
	if (rc == 0) rc = mbedtls_md_hmac_finish(&md, h);
	mbedtls_md_free(&md);
	if (rc == 0)
		memcpy(mic, h, 16);
	zero(h, sizeof(h));
	return rc == 0 ? 0 : -1;
}

static int mic_ok(const unsigned char *kck, const unsigned char *f, size_t flen)
{
	unsigned char m[16];
	int ok = calc_mic(kck, f, flen, m) == 0 && ct_eq(m, f + WPA_OFF_MIC, 16);
	zero(m, sizeof(m));
	return ok;
}

/* Build a reply into out->tx: echoed EAPOL version and descriptor type/version (A2). */
int wpa_reply(wpa_out *out, const unsigned char *rx, unsigned ki,
                 const unsigned char *nonce, const unsigned char *kd, size_t kdlen,
                 const unsigned char *kck)
{
	unsigned char *o = out->tx, ver = rx[0];
	size_t flen = WPA_OFF_KD + kdlen;

	if (flen > sizeof(out->tx))
		return -1;
	if (ver < 1)
		ver = 1;
	if (ver > 2)
		ver = 2;
	memset(o, 0, flen);
	o[0] = ver;
	o[1] = 3;                                  /* EAPOL-Key */
	put_be16(o + 2, (unsigned)(flen - 4));
	o[WPA_OFF_DESC] = rx[WPA_OFF_DESC];
	put_be16(o + WPA_OFF_INFO, (be16(rx + WPA_OFF_INFO) & WPA_KI_VER_MASK) | ki);
	/* Key Length 0 in replies (RSN) */
	memcpy(o + WPA_OFF_RC, rx + WPA_OFF_RC, WPA_RC_LEN);
	if (nonce)
		memcpy(o + WPA_OFF_NONCE, nonce, WPA_NONCE_LEN);
	put_be16(o + WPA_OFF_KDLEN, (unsigned)kdlen);
	if (kdlen)
		memcpy(o + WPA_OFF_KD, kd, kdlen);
	if (calc_mic(kck, o, flen, o + WPA_OFF_MIC) != 0)
		return -1;
	out->tx_len = flen;
	return 0;
}

/* Decrypt and parse the Key Data of a MIC-verified frame. */
static int key_data(wpa_sm *sm, const unsigned char *f, size_t kdlen, const unsigned char *kek,
                    unsigned char *plain, wpa_kd *kd)
{
	if (kdlen < 24 || kdlen > WPA_KEYDATA_MAX || kdlen % 8)
		return drop(sm, "encrypted key data length");
	if (wpa_aes_unwrap(kek, WPA_KEK_LEN, f + WPA_OFF_KD, kdlen, plain) != 0)
		return drop(sm, "key data unwrap failed");
	if (wpa_parse_kd(plain, kdlen - 8, kd) != 0)
		return drop(sm, "malformed key data");
	sm->kd_n = kd->n_ids;
	memcpy(sm->kd_ids, kd->ids, sizeof(sm->kd_ids));
	if (!kd->have_gtk || kd->gtk_len != WPA_GTK_LEN)
		return drop(sm, "no CCMP GTK in key data");
	return 0;
}

static void want_gtk(wpa_sm *sm, const wpa_kd *kd, const unsigned char *rsc, wpa_out *out)
{
	/* never reinstall the key that is already in use (B3 / KRACK group variant) */
	if (sm->gtk_set && sm->gtk_idx == kd->gtk_idx && ct_eq(sm->gtk, kd->gtk, WPA_GTK_LEN))
		return;
	sm->gtk_set = 1;
	sm->gtk_idx = kd->gtk_idx;
	memcpy(sm->gtk, kd->gtk, WPA_GTK_LEN);
	out->set_gtk = 1;
	out->gtk_idx = kd->gtk_idx;
	memcpy(out->gtk, kd->gtk, WPA_GTK_LEN);
	memcpy(out->rsc, rsc, WPA_RC_LEN);
	sm->st.gtk_installs++;
}

static int rx_msg1(wpa_sm *sm, const unsigned char *f, wpa_out *out)
{
	const unsigned char *an = f + WPA_OFF_NONCE;
	int resent;

	/* A4: the Key Data of msg-1 (PMKID KDE or anything else) is not used.
	 * msg-1 is not authenticated, so it only counts as "the AP re-sent msg-1"
	 * for the wrong-passphrase verdict before any PTK exists and only with a
	 * higher replay counter than the msg-1 we answered (a stale or replayed
	 * frame does not count); counted only once the reply is built. */
	resent = sm->msg2_pending && !sm->ptk_valid && sm->m1_rc_set &&
	         memcmp(f + WPA_OFF_RC, sm->m1_rc, WPA_RC_LEN) > 0;
	if (!(sm->state == WPA_HANDSHAKE || sm->rekey) || !sm->have_anonce ||
	    memcmp(sm->anonce, an, WPA_NONCE_LEN) != 0) {
		/* new handshake: fresh SNonce from the DRBG (B4) */
		if (!sm->rng || sm->rng(sm->rng_ctx, sm->snonce, WPA_NONCE_LEN) != 0) {
			zero(sm->snonce, sizeof(sm->snonce));
			return drop(sm, "no random SNonce");
		}
		memcpy(sm->anonce, an, WPA_NONCE_LEN);
		sm->have_anonce = 1;
		if (wpa_ptk(sm->pmk, sm->aa, sm->spa, sm->anonce, sm->snonce, sm->tptk) != 0)
			return drop(sm, "PTK derivation failed");
	}
	if (wpa_reply(out, f, WPA_KI_PAIRWISE | WPA_KI_MIC, sm->snonce, sm->ie, sm->ie_len, sm->tptk) != 0)
		return drop(sm, "cannot build msg-2");
	if (resent)
		sm->st.retried++;
	if (!sm->m1_rc_set || memcmp(f + WPA_OFF_RC, sm->m1_rc, WPA_RC_LEN) > 0) {
		memcpy(sm->m1_rc, f + WPA_OFF_RC, WPA_RC_LEN);
		sm->m1_rc_set = 1;
	}
	if (sm->ptk_valid)
		sm->rekey = 1;         /* PTK rekey: keys and port stay in use, state stays DONE (L8) */
	else
		sm->state = WPA_HANDSHAKE;
	sm->msg2_pending = 1;
	if (!sm->ptk_valid)
		sm->attempt_msg2 = 1;
	sm->st.msg1++;
	sm->st.msg2++;
	out->msg = 1;
	return 0;
}

static int rx_msg3(wpa_sm *sm, const unsigned char *f, size_t flen, unsigned ki, size_t kdlen,
                   wpa_out *out)
{
	const unsigned char *an = f + WPA_OFF_NONCE, *key;
	unsigned char plain[WPA_KEYDATA_MAX];
	wpa_kd kd;
	int fresh, rc = -1;

	/* B2: ANonce compared by value. A retransmitted msg-3 of the completed
	 * handshake is checked first against the installed PTK, so a forged
	 * msg-1 that reuses that ANonce cannot make us drop the AP's genuine
	 * retransmission; otherwise the ANonce must be the one our msg-2 answered. */
	if (!(ki & WPA_KI_ENCDATA))
		return drop(sm, "msg-3 key data not encrypted");
	if (sm->ptk_valid && memcmp(an, sm->ptk_anonce, WPA_NONCE_LEN) == 0 && mic_ok(sm->ptk, f, flen)) {
		key = sm->ptk;
		fresh = 0;
	} else if ((sm->state == WPA_HANDSHAKE || sm->rekey) && sm->have_anonce &&
	           memcmp(an, sm->anonce, WPA_NONCE_LEN) == 0) {
		key = sm->tptk;
		fresh = 1;
		if (!mic_ok(key, f, flen)) {
			sm->st.bad_mic++;
			return drop(sm, "msg-3 MIC mismatch");
		}
	} else if (sm->ptk_valid && memcmp(an, sm->ptk_anonce, WPA_NONCE_LEN) == 0) {
		sm->st.bad_mic++;
		return drop(sm, "msg-3 MIC mismatch");
	} else {
		return drop(sm, "msg-3 ANonce does not match msg-1");
	}
	/* A6: from here the frame is authentic; its counter becomes the floor */
	sm->rc_set = 1;
	memcpy(sm->rc, f + WPA_OFF_RC, WPA_RC_LEN);
	sm->st.msg3_ok++;          /* the AP accepted our msg-2: the passphrase is right */
	if (key_data(sm, f, kdlen, key + WPA_KCK_LEN, plain, &kd) != 0)
		goto out;
	if (!kd.have_rsne || wpa_check_rsne(plain + kd.rsne_off, kd.rsne_len) != 0) {
		drop(sm, "msg-3 RSNE missing or not WPA2-PSK/CCMP");
		goto out;
	}
	if (wpa_reply(out, f, WPA_KI_PAIRWISE | WPA_KI_MIC | WPA_KI_SECURE, NULL, NULL, 0, key) != 0) {
		drop(sm, "cannot build msg-4");
		goto out;
	}
	out->msg = 3;
	if (fresh) {
		out->ptk_rekey = sm->ptk_valid;
		memcpy(sm->ptk, sm->tptk, WPA_PTK_LEN);
		memcpy(sm->ptk_anonce, sm->anonce, WPA_NONCE_LEN);
		zero(sm->tptk, sizeof(sm->tptk));
		sm->ptk_valid = 1;
		out->set_ptk = 1;
		memcpy(out->tk, sm->ptk + WPA_KCK_LEN + WPA_KEK_LEN, WPA_TK_LEN);
		want_gtk(sm, &kd, f + WPA_OFF_RSC, out);
		out->open_port = !out->ptk_rekey;
		sm->state = WPA_DONE;
		sm->rekey = 0;
		sm->msg2_pending = 0;
		sm->attempt_msg2 = 0;
		sm->st.done++;
		sm->st.rejects = 0;
		sm->st.retried = 0;
	} else {
		sm->st.msg3_dup++;   /* B3: answer again, install nothing */
		if (sm->rekey && memcmp(sm->anonce, sm->ptk_anonce, WPA_NONCE_LEN) == 0) {
			/* a (forged) msg-1 had re-opened the finished handshake */
			sm->rekey = 0;
			sm->msg2_pending = 0;
			zero(sm->tptk, sizeof(sm->tptk));
		}
	}
	rc = 0;
out:
	zero(plain, sizeof(plain));
	zero(&kd, sizeof(kd));
	return rc;
}

static int rx_group1(wpa_sm *sm, const unsigned char *f, size_t flen, unsigned ki, size_t kdlen,
                     wpa_out *out)
{
	unsigned char plain[WPA_KEYDATA_MAX];
	wpa_kd kd;
	int rc = -1;

	if (!sm->ptk_valid)
		return drop(sm, "group msg-1 before the 4-way handshake");
	if (!(ki & WPA_KI_ENCDATA))
		return drop(sm, "group key data not encrypted");
	if (!mic_ok(sm->ptk, f, flen)) {
		sm->st.bad_mic++;
		return drop(sm, "group msg-1 MIC mismatch");
	}
	sm->rc_set = 1;
	memcpy(sm->rc, f + WPA_OFF_RC, WPA_RC_LEN);
	if (key_data(sm, f, kdlen, sm->ptk + WPA_KCK_LEN, plain, &kd) != 0)
		goto out;
	if (wpa_reply(out, f, WPA_KI_MIC | WPA_KI_SECURE, NULL, NULL, 0, sm->ptk) != 0) {
		drop(sm, "cannot build group msg-2");
		goto out;
	}
	want_gtk(sm, &kd, f + WPA_OFF_RSC, out);
	out->msg = 5;
	sm->st.gtk_rekeys++;
	rc = 0;
out:
	zero(plain, sizeof(plain));
	zero(&kd, sizeof(kd));
	return rc;
}

int wpa_rx(wpa_sm *sm, const unsigned char src[6], const unsigned char *f, size_t len, wpa_out *out)
{
	size_t flen, kdlen;
	unsigned ki;

	wpa_out_clear(out);
	if (len < 4 || f[1] != 3)
		return drop(sm, "not an EAPOL-Key frame");
	flen = 4 + be16(f + 2);                /* trailing bytes (padding) are ignored */
	if (flen > len)
		return drop(sm, "EAPOL length exceeds frame");
	if (flen < WPA_OFF_KD)
		return drop(sm, "EAPOL-Key frame too short");
	if (f[WPA_OFF_DESC] != WPA_DESC_RSN)
		return drop(sm, "not an RSN key descriptor");
	ki = be16(f + WPA_OFF_INFO);
	if ((ki & WPA_KI_VER_MASK) != WPA_VER_AES)
		return drop(sm, "unsupported key descriptor version");
	if (ki & (WPA_KI_ERROR | WPA_KI_REQUEST | WPA_KI_SMK))
		return drop(sm, "unexpected key information bits");
	kdlen = be16(f + WPA_OFF_KDLEN);
	if (kdlen > flen - WPA_OFF_KD)
		return drop(sm, "key data length exceeds frame");
	if (!sm->assoc)
		return drop(sm, "not associated");
	if (memcmp(src, sm->aa, 6) != 0)
		return drop(sm, "frame not from our AP");
	/* A6: replay counter strictly increasing once a MIC was verified */
	if (sm->rc_set && memcmp(f + WPA_OFF_RC, sm->rc, WPA_RC_LEN) <= 0)
		return drop(sm, "replay counter did not increase");

	if ((ki & (WPA_KI_PAIRWISE | WPA_KI_ACK | WPA_KI_MIC | WPA_KI_INSTALL)) ==
	    (WPA_KI_PAIRWISE | WPA_KI_ACK))
		return rx_msg1(sm, f, out);
	if ((ki & (WPA_KI_PAIRWISE | WPA_KI_ACK | WPA_KI_MIC | WPA_KI_INSTALL)) ==
	    (WPA_KI_PAIRWISE | WPA_KI_ACK | WPA_KI_MIC | WPA_KI_INSTALL))
		return rx_msg3(sm, f, flen, ki, kdlen, out);
	if ((ki & (WPA_KI_PAIRWISE | WPA_KI_ACK | WPA_KI_MIC | WPA_KI_SECURE | WPA_KI_INSTALL)) ==
	    (WPA_KI_ACK | WPA_KI_MIC | WPA_KI_SECURE))
		return rx_group1(sm, f, flen, ki, kdlen, out);
	return drop(sm, "unexpected EAPOL-Key message");
}

/* Wrong passphrase: the AP re-sent msg-1 WPA_RETRY_WRONG times after our
 * msg-2 instead of answering with msg-3 (it re-sends when the msg-2 MIC does
 * not verify), or WPA_REJECT_WRONG consecutive driver-observed associations
 * with this AP ended after our msg-2 without a msg-3 (APs that deauth at the first refusal; two,
 * because one can also be beacon loss). Measured on the FRITZ! APs: see
 * docs/firmware.md (Wi-Fi). */
#define WPA_RETRY_WRONG  2
#define WPA_REJECT_WRONG 2
int wpa_wrong_passphrase(const wpa_stats *st)
{
	/* never once a msg-3 MIC verified (the AP accepted our msg-2), and not
	 * when an msg-3 MIC failed (something else is going on) */
	if (st->msg3_ok || st->bad_mic)
		return 0;
	return st->rejects >= WPA_REJECT_WRONG || st->retried >= WPA_RETRY_WRONG;
}
