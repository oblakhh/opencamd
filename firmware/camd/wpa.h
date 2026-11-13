/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * wpa — minimal WPA2-PSK / CCMP supplicant logic (IEEE 802.11-2016 12.7).
 *
 * Pure: no I/O, no clocks, no globals. The caller (wpa_drv.c on the device,
 * tests/test_wpa.c on the host) feeds received EAPOL frames and association
 * changes in and executes the actions that come back (send a frame, install
 * a key, open the port). Only Key Descriptor Type 2 (RSN), Version 2
 * (HMAC-SHA1-128 MIC, AES key wrap), AKM PSK, CCMP pairwise + group.
 *
 * Fixes of the vendor supplicant review (docs/engineering-notes.md) by design:
 *   A1  Key Data is parsed as an element loop: RSNE/RSNXE/unknown skipped,
 *       GTK KDE (00-0F-AC:1) taken with its key index, IGTK KDE ignored.
 *   A2  replies echo the received descriptor type and version.
 *   A4  msg-1 Key Data is ignored (only its length is bounds-checked).
 *   A5/B1/B6  every length is checked against the frame and fixed bounds.
 *   A6  the replay counter must strictly increase once a MIC was verified.
 *   B2  msg-3 ANonce is compared by value with the msg-1 ANonce.
 *   B3  a retransmitted msg-3 / group msg-1 never reinstalls a key.
 *   B4  SNonce comes from the caller's DRBG callback (mbedtls_ctr_drbg).
 *   B5  PMK derivation errors (length limits, crypto) are reported.
 *   B7  nothing here runs in a signal handler; the caller is a plain loop.
 */
#ifndef CAMD_WPA_H
#define CAMD_WPA_H
#include <stddef.h>

#define WPA_PMK_LEN      32
#define WPA_NONCE_LEN    32
#define WPA_KCK_LEN      16
#define WPA_KEK_LEN      16
#define WPA_TK_LEN       16
#define WPA_PTK_LEN      48     /* KCK | KEK | TK (CCMP) */
#define WPA_GTK_LEN      16     /* CCMP group key */
#define WPA_RC_LEN        8
#define WPA_KEYDATA_MAX 512     /* largest Key Data we accept (bytes) */
#define WPA_TX_MAX      160     /* largest EAPOL frame we send */

/* EAPOL-Key frame offsets, counted from the EAPOL header (version byte). */
#define WPA_OFF_DESC       4
#define WPA_OFF_INFO       5
#define WPA_OFF_KEYLEN     7
#define WPA_OFF_RC         9
#define WPA_OFF_NONCE     17
#define WPA_OFF_IV        49
#define WPA_OFF_RSC       65
#define WPA_OFF_MIC       81
#define WPA_OFF_KDLEN     97
#define WPA_OFF_KD        99    /* = 4-byte EAPOL header + 95-byte key descriptor */

/* Key Information bits */
#define WPA_KI_VER_MASK  0x0007
#define WPA_KI_PAIRWISE  0x0008
#define WPA_KI_INSTALL   0x0040
#define WPA_KI_ACK       0x0080
#define WPA_KI_MIC       0x0100
#define WPA_KI_SECURE    0x0200
#define WPA_KI_ERROR     0x0400
#define WPA_KI_REQUEST   0x0800
#define WPA_KI_ENCDATA   0x1000
#define WPA_KI_SMK       0x2000

#define WPA_DESC_RSN     2
#define WPA_VER_AES      2      /* HMAC-SHA1-128 MIC + NIST AES key wrap */

/* ---- primitives ------------------------------------------------------- */
/* PMK = PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, 32). 0 ok; -1 if the
 * passphrase is not 8..63 printable ASCII or the SSID not 1..32 bytes, or
 * the crypto call failed (pmk is zeroed then). Slow on the device: cache it. */
int wpa_pmk(const char *passphrase, const unsigned char *ssid, size_t ssid_len,
            unsigned char pmk[WPA_PMK_LEN]);
/* 802.11 PRF (HMAC-SHA1): out = first out_len bytes of
 * HMAC(key, label || 0 || data || i), i = 0, 1, ... 0 ok. */
int wpa_prf(const unsigned char *key, size_t key_len, const char *label,
            const unsigned char *data, size_t data_len, unsigned char *out, size_t out_len);
/* PTK = PRF-384(PMK, "Pairwise key expansion", min/max(AA,SPA) || min/max(ANonce,SNonce)) */
int wpa_ptk(const unsigned char pmk[WPA_PMK_LEN], const unsigned char aa[6],
            const unsigned char spa[6], const unsigned char anonce[WPA_NONCE_LEN],
            const unsigned char snonce[WPA_NONCE_LEN], unsigned char ptk[WPA_PTK_LEN]);
/* RFC 3394 AES key unwrap; kek 16/24/32 bytes, in_len multiple of 8, >= 24.
 * out gets in_len - 8 bytes. 0 ok; -1 bad length or integrity check failed
 * (out is zeroed then). */
int wpa_aes_unwrap(const unsigned char *kek, size_t kek_len,
                   const unsigned char *in, size_t in_len, unsigned char *out);
/* Our RSN IE: WPA2-PSK, CCMP/CCMP, RSN capabilities 0 (no PMF). Returns 22. */
size_t wpa_rsn_ie(unsigned char ie[22]);

/* ---- Key Data (KDE / element) parsing ---------------------------------- */
typedef struct {
	int have_rsne;
	size_t rsne_off, rsne_len;     /* whole element (id+len+body) in the buffer */
	int have_gtk;
	int gtk_idx;                   /* 0..3 */
	int gtk_tx;
	size_t gtk_len;
	unsigned char gtk[32];
	int n_ignored;                 /* RSNXE, IGTK KDE, unknown elements/KDEs */
	int n_ids;                     /* layout, for logs: element id, or 0xdd00|type for a KDE */
	unsigned short ids[12];
} wpa_kd;
/* Walks [id][len][body] elements up to the end or the 0xdd 0x00 padding.
 * 0 ok; -1 malformed (truncated element, short KDE, bad GTK length, two GTKs). */
int wpa_parse_kd(const unsigned char *kd, size_t len, wpa_kd *out);
/* Sanity check of the AP's RSNE from msg-3: version 1, group CCMP, pairwise
 * list contains CCMP, AKM list contains PSK. 0 ok. */
int wpa_check_rsne(const unsigned char *ie, size_t len);

/* ---- state machine ----------------------------------------------------- */
typedef int (*wpa_rng_fn)(void *ctx, unsigned char *buf, size_t len);

enum { WPA_IDLE = 0, WPA_HANDSHAKE, WPA_DONE };

typedef struct {
	unsigned msg1;        /* msg-1 frames accepted */
	unsigned msg2;        /* msg-2 frames sent */
	unsigned retried;     /* AP re-sent msg-1 (higher counter) while our msg-2 was unanswered,
	                         before any PTK in this association */
	unsigned rejects;     /* consecutive associations that ended after msg-2 without msg-3
	                         (not counted for a roam to another BSSID) */
	unsigned done;        /* 4-way handshakes completed */
	unsigned msg3_dup;    /* retransmitted msg-3 answered without reinstall */
	unsigned gtk_rekeys;  /* group key handshakes completed */
	unsigned gtk_installs;
	unsigned drops;       /* frames dropped (malformed, replay, bad MIC, ...) */
	unsigned bad_mic;
	unsigned msg3_ok;     /* msg-3 frames whose MIC verified (ever, this run): the PMK is right */
} wpa_stats;

typedef struct {
	/* configuration */
	unsigned char pmk[WPA_PMK_LEN];
	unsigned char spa[6];          /* our MAC */
	unsigned char aa[6];           /* AP (BSSID) of the current association */
	unsigned char ie[24];          /* our RSN IE (also in the association request) */
	size_t ie_len;
	wpa_rng_fn rng;
	void *rng_ctx;
	/* association */
	int assoc;
	int state;
	/* replay protection: highest counter of a MIC-verified frame */
	int rc_set;
	unsigned char rc[WPA_RC_LEN];
	/* current 4-way handshake */
	int have_anonce;
	unsigned char anonce[WPA_NONCE_LEN];
	unsigned char snonce[WPA_NONCE_LEN];
	unsigned char tptk[WPA_PTK_LEN];
	int msg2_pending;              /* msg-2 sent, no valid msg-3 yet */
	int attempt_msg2;              /* msg-2 sent during this association, before success */
	int rekey;                     /* PTK rekey in progress (msg-1 after success); state stays DONE */
	int m1_rc_set;                 /* replay counter of the last msg-1 we answered */
	unsigned char m1_rc[WPA_RC_LEN];
	/* keys in use */
	int ptk_valid;
	unsigned char ptk[WPA_PTK_LEN];
	unsigned char ptk_anonce[WPA_NONCE_LEN];   /* handshake the installed PTK came from */
	int gtk_set;
	int gtk_idx;
	unsigned char gtk[WPA_GTK_LEN];
	wpa_stats st;
	const char *last_drop;         /* reason of the last dropped frame (static string) */
	int kd_n;                      /* element layout of the last decrypted Key Data (no key bytes) */
	unsigned short kd_ids[12];
} wpa_sm;

/* What the caller must do, in this order: send tx (an EAPOL frame from its
 * header on; the caller adds the Ethernet header, dst = aa), install the
 * pairwise key, install the group key, open the port. */
typedef struct {
	int msg;                       /* 1, 3 = 4-way msg handled; 5 = group msg-1; 0 = dropped */
	size_t tx_len;
	unsigned char tx[WPA_TX_MAX];
	int set_ptk;
	int ptk_rekey;                 /* set_ptk replaces an installed PTK: msg-4 went out encrypted
	                                  under the old one, so the caller must delay the install */
	unsigned char tk[WPA_TK_LEN];
	int set_gtk;
	int gtk_idx;
	unsigned char gtk[WPA_GTK_LEN];
	unsigned char rsc[WPA_RC_LEN];
	int open_port;
} wpa_out;

void wpa_init(wpa_sm *sm, const unsigned char pmk[WPA_PMK_LEN], const unsigned char spa[6],
              wpa_rng_fn rng, void *rng_ctx);
/* New association with aa: all handshake, replay and key state starts
 * over. Never counts a rejection (the caller may be reacting to a heuristic
 * that unauthenticated frames can trigger). */
void wpa_assoc(wpa_sm *sm, const unsigned char aa[6]);
/* Re-association with the same AP detected by a heuristic: = wpa_assoc(aa). */
void wpa_reassoc(wpa_sm *sm);
/* Association lost, as observed by the driver. Counts a rejected attempt if
 * our msg-2 was never answered with a valid msg-3. */
void wpa_disassoc(wpa_sm *sm);
/* Feed one EAPOL frame (from the EAPOL header on) received from src.
 * Returns 0 when handled (see out), -1 when dropped (sm->last_drop says why). */
int  wpa_rx(wpa_sm *sm, const unsigned char src[6], const unsigned char *frame, size_t len,
            wpa_out *out);
/* Zero every secret (PMK, nonces, PTK, GTK). */
void wpa_clear(wpa_sm *sm);
void wpa_out_clear(wpa_out *out);

/* Build a reply to rx into out->tx (exposed for tests): EAPOL version,
 * descriptor type and descriptor version are echoed from rx (A2). */
int  wpa_reply(wpa_out *out, const unsigned char *rx, unsigned ki, const unsigned char *nonce,
               const unsigned char *kd, size_t kdlen, const unsigned char *kck);

/* Wrong-passphrase verdict from the counters (pure, see wpa.c). */
int  wpa_wrong_passphrase(const wpa_stats *st);
#endif
